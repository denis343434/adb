#include "blockpuzzleplayer.h"

#include <algorithm>
#include <initializer_list>

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QHash>
#include <QRandomGenerator>
#include <QThread>

#include "adb/adbexecutor.h"
#include "adb/adbquote.h"
#include "automation/adbuttonfinder.h"
#include "automation/blockpuzzle.h"
#include "automation/ocrprovider.h"
#include "core/activitylog.h"
#include "core/farmlog.h"
#include "core/taskexecutor.h"
#include "devices/deviceregistry.h"

namespace farm {

namespace {

bool waitMs(int ms, CancellationToken token)
{
    const int step = 100;
    for (int spent = 0; spent < ms; spent += step) {
        if (token.isCancelled()) {
            return false;
        }
        QThread::msleep(static_cast<unsigned long>(std::min(step, ms - spent)));
    }
    return !token.isCancelled();
}

// A random point inside an ad control's box: repeated taps on a skip/close
// button must not land on the same pixel every time. A fifth of the box is kept
// as a rim so the tap stays on the control itself.
QPoint randomPointIn(const QRect &box)
{
    if (!box.isValid() || box.width() < 4 || box.height() < 4) {
        return box.center();
    }
    const int insetX = std::max(1, box.width() / 5);
    const int insetY = std::max(1, box.height() / 5);
    const int spanX = std::max(1, box.width() - 2 * insetX);
    const int spanY = std::max(1, box.height() - 2 * insetY);
    QRandomGenerator *rng = QRandomGenerator::global();
    return QPoint(box.left() + insetX + rng->bounded(spanX),
                  box.top() + insetY + rng->bounded(spanY));
}

// Time between two taps on an ad control: 2..5 s, so the rhythm is not fixed.
qint64 adTapDelayMs()
{
    return 2000 + static_cast<qint64>(QRandomGenerator::global()->bounded(3001));
}

struct NudgeOffset
{
    int dx;
    int dy;
};

// Tried in turn when a placement is refused: the target itself first, then one
// cell below/above/right/left, the diagonals, and finally two cells away.
// +dy is down the screenshot.
constexpr NudgeOffset kNudges[] = {
    { 0, 0 }, { 0, 1 }, { 0, -1 }, { 1, 0 }, { -1, 0 },
    { 1, 1 }, { -1, 1 }, { 1, -1 }, { -1, -1 },
    { 0, 2 }, { 0, -2 }, { 2, 0 }, { -2, 0 },
};
constexpr int kNudgeCount = static_cast<int>(sizeof(kNudges) / sizeof(kNudges[0]));

AdbResult shell(const QString &serial, const QString &script, int timeoutMs, CancellationToken token)
{
    AdbCommand command;
    command.serial = serial;
    command.args << QStringLiteral("shell") << script;
    command.timeoutMs = timeoutMs;
    command.label = QStringLiteral("autoplay");
    return AdbExecutor::instance().runSync(command, token);
}

QImage capture(const QString &serial, CancellationToken token, QString *error)
{
    AdbCommand command;
    command.serial = serial;
    command.args << QStringLiteral("exec-out") << QStringLiteral("screencap") << QStringLiteral("-p");
    command.timeoutMs = 20000;
    command.binaryOutput = true;
    command.label = QStringLiteral("autoplay-shot");
    const AdbResult result = AdbExecutor::instance().runSync(command, token);
    if (!result.ok) {
        if (error != nullptr) {
            *error = result.error;
        }
        return QImage();
    }
    const QImage image = QImage::fromData(result.rawStdOut, "PNG");
    if (image.isNull() && error != nullptr) {
        *error = QStringLiteral("screencap returned no image");
    }
    return image;
}

QString fingerprint(const PuzzleScene &scene)
{
    QString id;
    for (quint8 cell : scene.filled) {
        id.append(cell ? QLatin1Char('#') : QLatin1Char('.'));
    }
    for (const PuzzlePiece &piece : scene.pieces) {
        id.append(QLatin1Char('|'));
        for (quint8 cell : piece.shape.cells) {
            id.append(cell ? QLatin1Char('#') : QLatin1Char('.'));
        }
    }
    return id;
}

QString shapeText(const PuzzleShape &shape)
{
    QString text;
    for (int r = 0; r < shape.rows; ++r) {
        if (r > 0) {
            text.append(QLatin1Char(' '));
        }
        for (int c = 0; c < shape.cols; ++c) {
            text.append(shape.at(r, c) ? QLatin1Char('#') : QLatin1Char('.'));
        }
    }
    return text;
}

// OCR confuses I/L/1/0/5/8 in this game's rounded font ("CONTlNUE"). Folding the
// look-alikes to one character lets a plain `contains` match both spellings.
QString normalizedLabel(const QString &text)
{
    QString out;
    out.reserve(text.size());
    for (QChar ch : text) {
        const QChar upper = ch.toUpper();
        if (upper.isLetterOrNumber()) {
            out.append(upper);
        }
    }
    out.replace(QLatin1Char('L'), QLatin1Char('I'));
    out.replace(QLatin1Char('1'), QLatin1Char('I'));
    out.replace(QLatin1Char('0'), QLatin1Char('O'));
    out.replace(QLatin1Char('5'), QLatin1Char('S'));
    out.replace(QLatin1Char('8'), QLatin1Char('B'));
    return out;
}

QStringList normalizedList(std::initializer_list<const char *> raw)
{
    QStringList list;
    for (const char *text : raw) {
        list.append(normalizedLabel(QString::fromUtf8(text)));
    }
    return list;
}

// Full-screen ads carry their own chrome: the provider label above the creative
// ("РЕКЛАМА · Mintegral"), a skip counter, or a download button. The permanent
// in-game ad banner sits in the bottom strip and must not count as an ad.
bool looksLikeAd(const OcrResult &ocr, int imageHeight)
{
    static const QStringList kMarkers = normalizedList({
        "РЕКЛАМА",
        "REKLAMA",
        "ADVERTISEMENT",
        "ADVERTISING",
        "SPONSORED",
        "ПРОПУСТИТЬ",
        "СКАЧАТЬ",
        "УСТАНОВИТЬ",
        "LEARN MORE",
        "INSTALL",
        "DOWNLOAD",
    });
    const int bannerTop = imageHeight * 88 / 100;
    for (const OcrLine &line : ocr.lines) {
        if (line.rect.center().y() >= bannerTop) {
            continue;
        }
        const QString text = normalizedLabel(line.text);
        for (const QString &marker : kMarkers) {
            if (text.contains(marker)) {
                return true;
            }
        }
    }
    return false;
}

// The game-over card shows "SCORE <n>"; the in-game header does not. It is used
// to find the card's green button by colour when OCR cannot read its label.
bool mentionsGameOver(const OcrResult &ocr)
{
    static const QString kScore = normalizedLabel(QStringLiteral("SCORE"));
    for (const OcrLine &line : ocr.lines) {
        if (normalizedLabel(line.text).contains(kScore)) {
            return true;
        }
    }
    return false;
}

QPoint ocrActionPoint(const OcrResult &ocr)
{
    static const QStringList kLabels = normalizedList({
        "PLAY AGAIN",
        // The card's "PLAY AGAIN" is read as "AGAIN" on the dark theme (Windows OCR
        // drops the leading word), and the Russian build says "ПРОДОЛЖИТЬ".
        "AGAIN",
        "ПРОДОЛЖИТЬ",
        "RESTART",
        "PLAY",
        "CONTINUE",
        "CLAIM",
        "COLLECT",
        "CLOSE",
        "START",
        "CLASSIC",
        "NEXT",
        "RESUME",
        "DONE",
    });
    static const QStringList kAvoid = normalizedList({
        "MENU",
        "ADVENTURE",
        "THEMES",
        "REWARD",
        "LEADERBOARD",
        "ONLINE",
        "DAILY",
        "SETTINGS",
        "SHOP",
        "PURCHASE",
    });
    for (const QString &label : kLabels) {
        for (const OcrLine &line : ocr.lines) {
            const QString text = normalizedLabel(line.text);
            if (!text.contains(label)) {
                continue;
            }
            bool avoided = false;
            for (const QString &avoid : kAvoid) {
                if (text.contains(avoid)) {
                    avoided = true;
                    break;
                }
            }
            if (avoided) {
                continue;
            }
            return line.rect.center();
        }
    }
    return QPoint();
}

bool gameIsFocused(const QString &serial, const QString &package, CancellationToken token)
{
    AdbCommand command;
    command.serial = serial;
    command.args << QStringLiteral("shell")
                 << QStringLiteral("dumpsys window | grep -E 'mCurrentFocus'");
    command.timeoutMs = 8000;
    command.label = QStringLiteral("autoplay-focus");
    const AdbResult result = AdbExecutor::instance().runSync(command, token);
    if (!result.ok) {
        return true; // a transient dumpsys failure is not proof that the game left
    }
    for (const QString &line : result.stdOut.split(QLatin1Char('\n'))) {
        if (line.contains(QLatin1String("mCurrentFocus"))) {
            return line.contains(package);
        }
    }
    return true;
}

} // namespace

BlockPuzzlePlayer &BlockPuzzlePlayer::instance()
{
    static BlockPuzzlePlayer player;
    return player;
}

BlockPuzzlePlayer::BlockPuzzlePlayer(QObject *parent)
    : QObject(parent)
{
}

void BlockPuzzlePlayer::start(const QStringList &deviceIds)
{
    if (m_active.load() > 0) {
        return;
    }
    QStringList ids;
    for (const QString &id : deviceIds) {
        if (!id.isEmpty() && !ids.contains(id)) {
            ids.append(id);
        }
    }
    if (ids.isEmpty()) {
        return;
    }
    m_token = CancellationToken();
    const CancellationToken token = m_token;
    const int needed = static_cast<int>(ids.size());
    m_active.store(needed);
    // The Skip/Close crops of the ad phase live next to the executable; without
    // them the autoplay still works, it just watches ads instead of skipping.
    const QString cropsDir = QCoreApplication::applicationDirPath() + QStringLiteral("/skip_ads/buttons");
    if (AdButtonFinder::instance().loadFromDirectory(cropsDir) == 0) {
        FarmLog::instance().warning(QStringLiteral("autoplay"),
                                    QStringLiteral("No ad-button crops in %1 — ads will only be watched").arg(cropsDir));
    }
    // Each worker holds its thread for the entire session (infinite loop), so
    // the automation lane needs at least one slot per device.
    const int current = TaskExecutor::instance().laneConcurrency(QStringLiteral("automation"));
    if (needed > current) {
        TaskExecutor::instance().setLaneConcurrency(QStringLiteral("automation"), needed);
    }
    emit runningChanged(true);
    ActivityLog::instance().info(ActivityEntry::Automation,
                                 QStringLiteral("Autoplay started on %1 device(s): %2")
                                     .arg(ids.size())
                                     .arg(ids.join(QStringLiteral(", "))));
    for (const QString &id : ids) {
        const bool queued = TaskExecutor::instance().run(QStringLiteral("automation"), [this, id, token]() {
            playDevice(id, token);
            QMetaObject::invokeMethod(this, [this]() { finishedOne(); }, Qt::QueuedConnection);
        });
        if (!queued) {
            QMetaObject::invokeMethod(this, [this]() { finishedOne(); }, Qt::QueuedConnection);
        }
    }
}

void BlockPuzzlePlayer::stop()
{
    m_token.cancel();
    ActivityLog::instance().info(ActivityEntry::Automation, QStringLiteral("Autoplay stopping"));
}

void BlockPuzzlePlayer::finishedOne()
{
    const int left = m_active.fetch_sub(1) - 1;
    if (left <= 0) {
        m_active.store(0);
        TaskExecutor::instance().setLaneConcurrency(QStringLiteral("automation"), 5);
        emit runningChanged(false);
        emit statusChanged(QString(), QStringLiteral("Autoplay stopped"));
    }
}

void BlockPuzzlePlayer::publish(const QString &deviceId, const QString &text)
{
    FarmLog::instance().info(QStringLiteral("autoplay"), text, deviceId);
    QMetaObject::invokeMethod(this, [this, deviceId, text]() { emit statusChanged(deviceId, text); }, Qt::QueuedConnection);
}

void BlockPuzzlePlayer::playDevice(const QString &deviceId, CancellationToken token)
{
    publish(deviceId, QStringLiteral("Worker started"));
    const DeviceRecord record = DeviceRegistry::instance().snapshot(deviceId);
    if (!record.id.isEmpty() && !record.isOnline()) {
        publish(deviceId, QStringLiteral("Device is offline — skipping"));
        ActivityLog::instance().warning(ActivityEntry::Automation,
                                        QStringLiteral("Autoplay skipped offline device %1").arg(deviceId), deviceId);
        return;
    }

    publish(deviceId, QStringLiteral("Waking the screen"));
    shell(deviceId, QStringLiteral("input keyevent KEYCODE_WAKEUP"), 8000, token);
    if (token.isCancelled()) {
        return;
    }

    const AdbResult listed = shell(deviceId, QStringLiteral("pm list packages"), 20000, token);
    if (!listed.ok) {
        publish(deviceId, listed.error.isEmpty() ? QStringLiteral("Could not list packages") : listed.error);
        ActivityLog::instance().error(ActivityEntry::Automation, QStringLiteral("Autoplay could not list packages"), deviceId);
        return;
    }
    const QString package = BlockPuzzle::pickPackage(listed.stdOut.split(QLatin1Char('\n')));
    if (package.isEmpty()) {
        publish(deviceId, QStringLiteral("Block Puzzle is not installed"));
        ActivityLog::instance().error(ActivityEntry::Automation, QStringLiteral("Block Puzzle is not installed"), deviceId);
        return;
    }

    publish(deviceId, QStringLiteral("Launching %1").arg(package));
    const QString launch = QStringLiteral("monkey -p %1 -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1 && echo launched")
                               .arg(adb::shellQuote(package));
    const AdbResult started = shell(deviceId, launch, 20000, token);
    if (!started.ok || !started.stdOut.contains(QLatin1String("launched"))) {
        publish(deviceId, QStringLiteral("Could not launch Block Puzzle"));
        ActivityLog::instance().error(ActivityEntry::Automation, QStringLiteral("Autoplay could not launch %1").arg(package), deviceId);
        return;
    }
    if (!waitMs(400, token)) {
        return;
    }

    const bool ocrAvailable = OcrProvider::available();
    int placed = 0;
    int games = 0;
    int shotFails = 0;        // consecutive null screenshots
    int quietMs = 0;          // time without a playable board (menu, game over, ad, loading)
    int stalls = 0;
    int nudgeIndex = 0;       // offset tried for the current placement
    bool retryMove = false;   // the next drag repeats the previous move
    PuzzleMove lastMove;
    int sameTaps = 0;         // taps on the same point without the screen changing
    int relaunches = 0;
    bool waitingForLand = false;
    bool everSawBoard = false;
    bool adPlaying = false;
    qint64 adStartedMs = 0;
    qint64 lastOcrMs = 0;
    qint64 lastOcrOkMs = 0;
    qint64 lastFocusMs = 0;
    int focusMisses = 0;
    bool returnedWithBack = false;   // Back already tried against a click-through
    bool adSeen = false;      // last OCR result
    bool gameOverSeen = false;
    qint64 boardFullMs = 0;   // when the board was last seen full (game over)
    // When a colour button was first seen on a screen whose text OCR did not turn
    // into an action; after a few seconds the button is taken anyway.
    qint64 buttonWaitSinceMs = 0;
    QPoint actionPoint;
    QString lastTapKey;
    QString lastSeen;
    QVector<PuzzleMove> rejected;
    // A short drag still lands on this game. A drag that misses is retried slower.
    int swipeMs = 700;
    // The game draws the dragged piece above the finger by roughly its own height,
    // which can be off by about a cell on some screen sizes. A nudge that finally
    // landed tells us the correction, per piece height.
    QHash<int, double> liftExtraCells;
    NudgeOffset lastNudgeUsed{ 0, 0 };
    int lastNudgeRows = 0;
    // No tap may happen before this timestamp: a rewarded ad needs a moment to
    // load after the button that starts it, and a second tap would hit the ad.
    qint64 noTapUntilMs = 0;

    // Ad controls: the Skip/Close crops are searched every ~0.35 s, a few times
    // per ad, and only while an ad is actually on screen. Each tap goes to a
    // random point inside the control, and repeating the tap on the same control
    // waits 2..5 s — but a different control (the close cross after the skip) is
    // taken at once, so the ad does not slip away while the bot is waiting.
    int adTaps = 0;
    qint64 lastAdScanMs = 0;
    qint64 nextAdTapMs = 0;
    QString lastAdTapName;
    QRect lastAdTapRect;
    double adProbeScore = 0.0;   // best candidate of the last failed search
    QString adProbeName;
    qint64 lastAdReportMs = 0;
    bool adShotSaved = false;
    bool adControlSeen = false;   // the last search found a control
    // The control seen on the previous frame: a tap waits until the same crop
    // matches at the same place once more (see tapAdButton).
    QString pendingAdName;
    QPoint pendingAdCenter;
    const auto tapAdButton = [&](const QImage &shot, qint64 now) -> bool {
        if (adTaps >= 10 || now - lastAdScanMs < 350) {
            return false;
        }
        lastAdScanMs = now;
        double probeScore = 0.0;
        QString probeName;
        const AdButtonHit hit = AdButtonFinder::instance().find(shot, &probeScore, &probeName);
        adProbeScore = probeScore;
        adProbeName = probeName;
        adControlSeen = hit.ok();
        if (!hit.ok()) {
            pendingAdName.clear();
            return false;
        }
        // One frame is not proof. A photo fragment or a run of ad text can
        // correlate with a glyph crop by chance, and a tap on the creative opens
        // the advertiser's page in the browser — which costs the run. The same
        // control must be found twice in a row, at the same place, before it is
        // touched; a real Skip/Close does not move between two frames.
        if (hit.name != pendingAdName || (hit.rect.center() - pendingAdCenter).manhattanLength() > 24) {
            pendingAdName = hit.name;
            pendingAdCenter = hit.rect.center();
            return false;
        }
        pendingAdName.clear();
        const bool sameControl = hit.name == lastAdTapName
                                 && (hit.rect.center() - lastAdTapRect.center()).manhattanLength() < 40;
        if (sameControl && now < nextAdTapMs) {
            return false;   // the same button again: keep the 2..5 s rhythm
        }
        ++adTaps;
        adStartedMs = now;
        lastAdTapName = hit.name;
        lastAdTapRect = hit.rect;
        const QPoint at = randomPointIn(hit.rect);
        publish(deviceId, QStringLiteral("Ad: tapping %1 at %2,%3 (match %4)")
                             .arg(hit.name)
                             .arg(at.x())
                             .arg(at.y())
                             .arg(hit.score, 0, 'f', 2));
        shell(deviceId, QStringLiteral("input tap %1 %2").arg(at.x()).arg(at.y()), 8000, token);
        nextAdTapMs = now + adTapDelayMs();
        noTapUntilMs = now + 1200;
        waitMs(300, token);
        return true;
    };

    // Diagnostics for an ad whose control is not recognised: log how close the
    // best crop came, and keep one screenshot per ad so a missing glyph can be
    // added as a crop without guessing what the creative looked like.
    const auto reportAdProbe = [&](const QImage &shot, qint64 now) {
        if (!adControlSeen && now - lastAdReportMs >= 5000) {
            lastAdReportMs = now;
            publish(deviceId, QStringLiteral("Ad: no control matched (best %1 at %2)")
                                 .arg(adProbeName.isEmpty() ? QStringLiteral("nothing") : adProbeName)
                                 .arg(adProbeScore, 0, 'f', 2));
        }
        if (!adControlSeen && !adShotSaved && now - adStartedMs > 3000) {
            adShotSaved = true;
            const QString folder = FarmLog::instance().directory() + QStringLiteral("/ad-shots");
            QDir().mkpath(folder);
            const QString path = folder + QStringLiteral("/%1-%2.png")
                                               .arg(deviceId, QDateTime::currentDateTime().toString(QStringLiteral("yyMMdd-HHmmss")));
            if (shot.save(path)) {
                FarmLog::instance().info(QStringLiteral("autoplay"),
                                         QStringLiteral("Ad screen saved to %1").arg(path), deviceId);
            }
        }
    };

    while (!token.isCancelled()) {
        QString error;
        const QImage shot = capture(deviceId, token, &error);
        if (shot.isNull()) {
            ++shotFails;
            if (shotFails > 6) {
                publish(deviceId, error.isEmpty() ? QStringLiteral("Lost the screen") : error);
                ActivityLog::instance().error(ActivityEntry::Automation, QStringLiteral("Autoplay lost the screen"), deviceId);
                return;
            }
            if (!waitMs(150, token)) {
                return;
            }
            continue;
        }
        shotFails = 0;

        const PuzzleScene scene = BlockPuzzle::read(shot);
        const bool playable = scene.boardFound && !scene.pieces.isEmpty();
        const qint64 now = QDateTime::currentMSecsSinceEpoch();

        // Ads are checked on every frame, not only when the board is missing: an
        // ad can cover the board while the reader still recognises cells behind it,
        // and the bot must stop touching the screen in that case too.
        // While a drag is being verified the board is read anyway; skipping OCR
        // there saves a second per attempt on slow phones, which is what makes a
        // misplaced piece feel like it takes forever.
        if (ocrAvailable && !waitingForLand && now - lastOcrMs >= 800) {
            lastOcrMs = now;
            const OcrResult scan = OcrProvider::recognize(shot);
            if (scan.ok()) {
                lastOcrOkMs = now;
                adSeen = looksLikeAd(scan, shot.height());
                gameOverSeen = mentionsGameOver(scan);
                actionPoint = ocrActionPoint(scan);
            }
        }
        const bool ocrFresh = ocrAvailable && (now - lastOcrOkMs < 10000);
        if (!ocrFresh) {
            adSeen = false;
            gameOverSeen = false;
            actionPoint = QPoint();
        }

        if (adSeen) {
            if (!adPlaying) {
                adPlaying = true;
                adStartedMs = now;
                quietMs = 0;
                adTaps = 0;
                nextAdTapMs = 0;
                adShotSaved = false;
                adControlSeen = false;
                pendingAdName.clear();
                adProbeScore = 0.0;
                adProbeName.clear();
                publish(deviceId, QStringLiteral("Ad started — looking for its Skip button"));
                ActivityLog::instance().info(ActivityEntry::Automation,
                                             QStringLiteral("Autoplay paused: ad is playing, looking for Skip/Close"), deviceId);
            }
            // Only the ad's own Skip (">|") or Close ("x") control is tapped here:
            // a tap anywhere else opens the advertiser's page and can lose the
            // reward. If no control is on screen the ad is watched to the end.
            if (tapAdButton(shot, now)) {
                if (token.isCancelled()) {
                    return;
                }
                continue;
            }
            // Nothing was tapped: say how close the best crop came, and keep one
            // screenshot of the ad, so a missing glyph can be added as a crop
            // without guessing what the creative looked like.
            reportAdProbe(shot, now);
            quietMs = 0;
            if (now - adStartedMs > 180000) {
                publish(deviceId, QStringLiteral("Ad has been up for 3 min — pressing Back"));
                shell(deviceId, QStringLiteral("input keyevent KEYCODE_BACK"), 8000, token);
                adStartedMs = now;
                if (!waitMs(1500, token)) {
                    return;
                }
            }
            if (!waitMs(350, token)) {
                return;
            }
            continue;
        }
        if (adPlaying) {
            // Markers are gone. Resume only when the game is readable again — a
            // playable board, a readable button, or the game-over card. An ad end
            // card can drop its provider label, so "no marker" alone is not enough.
            if (!playable && actionPoint.isNull() && !gameOverSeen) {
                // A markerless creative (a plain video) is still an ad: stay off
                // the screen. Only a mis-tap that sent the device to another app
                // makes the worker bring the game back. The provider marker can
                // disappear before the ad's own Close cross does, so the control
                // is still looked for here.
                if (tapAdButton(shot, now)) {
                    if (token.isCancelled()) {
                        return;
                    }
                    continue;
                }
                quietMs = 0;
                reportAdProbe(shot, now);
                if (now - adStartedMs > 180000) {
                    publish(deviceId, QStringLiteral("Ad has been up for 3 min — pressing Back"));
                    shell(deviceId, QStringLiteral("input keyevent KEYCODE_BACK"), 8000, token);
                    adStartedMs = now;
                    if (!waitMs(1500, token)) {
                        return;
                    }
                }
                if (now - lastFocusMs >= 6000) {
                    lastFocusMs = now;
                    if (!gameIsFocused(deviceId, package, token)) {
                        ++focusMisses;
                    } else {
                        focusMisses = 0;
                        returnedWithBack = false;
                    }
                }
                // The first sign of a click-through (the store, the browser, the
                // advertiser's page) is answered with Back: it comes back to the ad
                // or the game, while a relaunch costs the run. Only a second miss
                // means Back did not help.
                if (focusMisses == 1 && !returnedWithBack) {
                    returnedWithBack = true;
                    publish(deviceId, QStringLiteral("Ad opened another app — pressing Back"));
                    shell(deviceId, QStringLiteral("input keyevent KEYCODE_BACK"), 8000, token);
                    if (!waitMs(900, token)) {
                        return;
                    }
                }
                if (focusMisses >= 2) {
                    publish(deviceId, QStringLiteral("Ad opened another app — relaunching the game"));
                    ActivityLog::instance().warning(ActivityEntry::Automation,
                                                    QStringLiteral("Autoplay relaunching: the ad left the game"), deviceId);
                    shell(deviceId, launch, 20000, token);
                    focusMisses = 0;
                    returnedWithBack = false;
                    adPlaying = false;
                    quietMs = 0;
                    lastOcrMs = 0;
                    lastSeen.clear();
                    rejected.clear();
                    waitingForLand = false;
                    if (!waitMs(1500, token)) {
                        return;
                    }
                    continue;
                }
                if (!waitMs(400, token)) {
                    return;
                }
                continue;
            }
            adPlaying = false;
            adTaps = 0;
            adShotSaved = false;
            adControlSeen = false;
            pendingAdName.clear();
            publish(deviceId, QStringLiteral("Ad finished — resuming"));
            ActivityLog::instance().info(ActivityEntry::Automation,
                                         QStringLiteral("Autoplay resumed after the ad"), deviceId);
        }

        if (playable) {
            if (!everSawBoard) {
                ++games;
                publish(deviceId, QStringLiteral("Game %1 started").arg(games));
                ActivityLog::instance().info(ActivityEntry::Automation,
                                             QStringLiteral("Autoplay game %1 started").arg(games), deviceId);
            } else if (quietMs > 5000) {
                ++games;
                publish(deviceId, QStringLiteral("Game %1 started").arg(games));
                ActivityLog::instance().info(ActivityEntry::Automation,
                                             QStringLiteral("Autoplay game %1 started").arg(games), deviceId);
            }
            everSawBoard = true;
            quietMs = 0;
            relaunches = 0;
            focusMisses = 0;
            adSeen = false;
            gameOverSeen = false;
            adTaps = 0;
            boardFullMs = 0;

            // The pause card (gear, top-right) keeps the dimmed board visible below
            // its green RESUME button. A green button below the board is that one,
            // not a board colour run: take it and the run continues.
            if (scene.hasButton && !scene.rowCenter.isEmpty()
                && scene.button.y() > scene.rowCenter.last() + 24) {
                publish(deviceId, QStringLiteral("Tapping the Resume button at %1,%2").arg(scene.button.x()).arg(scene.button.y()));
                shell(deviceId, QStringLiteral("input tap %1 %2").arg(scene.button.x()).arg(scene.button.y()), 8000, token);
                lastSeen.clear();
                rejected.clear();
                waitingForLand = false;
                if (!waitMs(900, token)) {
                    return;
                }
                continue;
            }

            if (scene.pieces.isEmpty()) {
                if (!waitMs(100, token)) {
                    return;
                }
                continue;
            }

            const QString seen = fingerprint(scene);
            if (seen != lastSeen) {
                if (waitingForLand) {
                    ++placed;
                    waitingForLand = false;
                    nudgeIndex = 0;
                    retryMove = false;
                    // A nudge that had to move the drop point up or down shows the
                    // lift guess was off by that much; remember half of it for the
                    // next piece of the same height.
                    if (lastNudgeUsed.dy != 0) {
                        const double learned = liftExtraCells.value(lastNudgeRows, 0.0) + lastNudgeUsed.dy * 0.5;
                        liftExtraCells[lastNudgeRows] = std::clamp(learned, -1.5, 1.5);
                    }
                    const QString text = QStringLiteral("Move %1").arg(placed);
                    publish(deviceId, text);
                    if (placed == 1 || placed % 20 == 0) {
                        ActivityLog::instance().info(ActivityEntry::Automation, text, deviceId);
                    }
                }
                rejected.clear();
                swipeMs = 700;
                stalls = 0;
                lastSeen = seen;
            } else if (waitingForLand) {
                // The drag did not change the board. Keep the same piece and retry the
                // same spot with a small offset — below, above, right, left — until the
                // game accepts the placement.
                waitingForLand = false;
                ++nudgeIndex;
                if (nudgeIndex >= kNudgeCount) {
                    rejected.append(lastMove);
                    nudgeIndex = 0;
                    retryMove = false;
                    publish(deviceId, QStringLiteral("No placement accepted here — trying another spot"));
                    if (rejected.size() >= 3) {
                        swipeMs = 1100;
                    }
                    if (rejected.size() > 12) {
                        rejected.clear();
                        swipeMs = 1500;
                        lastSeen.clear();
                        publish(deviceId, QStringLiteral("Drags not landing — slowing down and retrying"));
                    }
                } else {
                    retryMove = true;
                }
            }

            PuzzleMove move;
            if (retryMove && lastMove.ok()) {
                move = lastMove;
            } else {
                move = BlockPuzzle::choose(scene, rejected);
            }
            if (!move.ok()) {
                ++stalls;
                const bool boardFull = std::none_of(scene.filled.cbegin(), scene.filled.cend(),
                                                    [](quint8 cell) { return cell == 0; });
                if (boardFull) {
                    // Board is full — game over. The overlay handler taps Play Again.
                    boardFullMs = now;
                    publish(deviceId, QStringLiteral("Board full — waiting for game-over screen"));
                    ActivityLog::instance().info(ActivityEntry::Automation, QStringLiteral("Autoplay board full — game over"), deviceId);
                    stalls = 0;
                    if (!waitMs(2000, token)) {
                        return;
                    }
                } else if (!rejected.isEmpty() && stalls >= 3) {
                    // Every placement was tried and none landed: forget the rejects
                    // and read the board again instead of waiting for a game over.
                    rejected.clear();
                    swipeMs = 700;
                    stalls = 0;
                    lastSeen.clear();
                    waitingForLand = false;
                    retryMove = false;
                    publish(deviceId, QStringLiteral("Drags did not land — rereading the board"));
                    if (!waitMs(600, token)) {
                        return;
                    }
                } else {
                    publish(deviceId, QStringLiteral("No legal move — waiting (%1)").arg(stalls));
                    if (!waitMs(300, token)) {
                        return;
                    }
                }
                continue;
            }
            stalls = 0;
            const PuzzleDrag drag = BlockPuzzle::dragFor(scene, move);
            if (!drag.ok) {
                rejected.append(move);
                retryMove = false;
                continue;
            }
            const double cell = scene.rowCenter.size() > 1 ? scene.rowCenter.at(1) - scene.rowCenter.at(0) : 0.0;
            const NudgeOffset nudge = kNudges[nudgeIndex];
            const int x1 = std::clamp(drag.from.x(), 1, shot.width() - 2);
            const int y1 = std::clamp(drag.from.y(), 1, shot.height() - 2);
            const int x2 = std::clamp(drag.to.x() + static_cast<int>(std::lround(nudge.dx * cell)), 1, shot.width() - 2);
            // The piece is drawn about two cells above the finger, so the release
            // sits two cells below the target. A release on the cell itself drops
            // the piece above the board. The exact offset differs a little between
            // screens, so a nudge that finally landed (below) calibrates it.
            const int pieceRows = scene.pieces.at(move.piece).shape.rows;
            int lift = 0;
            if (cell > 0.0) {
                lift = static_cast<int>(std::lround(cell * (2.0 + liftExtraCells.value(pieceRows, 0.0))));
            }
            lastNudgeUsed = nudge;
            lastNudgeRows = pieceRows;
            const int y2 = std::clamp(drag.to.y() + lift + static_cast<int>(std::lround(nudge.dy * cell)),
                                      1, static_cast<int>(shot.height() * 0.92));
            shell(deviceId,
                  QStringLiteral("input swipe %1 %2 %3 %4 %5").arg(x1).arg(y1).arg(x2).arg(y2).arg(swipeMs),
                  8000 + swipeMs, token);
            waitingForLand = true;
            lastMove = move;
            retryMove = false;
            publish(deviceId, QStringLiteral("Dragging %1 toward row %2, nudge %3,%4 (%5,%6 → %7,%8)")
                                .arg(shapeText(scene.pieces.at(move.piece).shape))
                                .arg(move.row + 1)
                                .arg(nudge.dx)
                                .arg(nudge.dy)
                                .arg(x1)
                                .arg(y1)
                                .arg(x2)
                                .arg(y2));
            if (!waitMs(500, token)) {
                return;
            }
            continue;
        }

        // ── No playable board: menu, game over, loading, or another app ──
        quietMs += 400;

        // A drag cannot be verified once the board is gone: the piece either ended
        // the game (the game-over card is up) or an ad covered the screen. Leaving
        // this flag set froze the OCR gate further down for good — OCR only runs
        // while a drag is not being awaited — so the card's "PLAY AGAIN" /
        // "CONTINUE (WATCH AD)" button was never tapped and the worker sat on the
        // card until the stall timer relaunched the game (~90 s, every game over).
        if (waitingForLand) {
            waitingForLand = false;
            nudgeIndex = 0;
            retryMove = false;
            lastOcrMs = 0;   // this is a new screen: read it at once
        }

        // A rewarded ad needs a moment to load after the button that starts it.
        // Tapping again during that window hits the ad creative (often a green
        // "Install" button) and opens the advertiser's page.
        if (now < noTapUntilMs) {
            if (!waitMs(250, token)) {
                return;
            }
            continue;
        }

        // Without OCR an ad is just "no board"; with OCR an ad can still miss the
        // provider marker. Once the board has been gone for a moment, give the
        // ad's own Skip/Close control a chance instead of waiting for the stall
        // timer to relaunch the game.
        if (quietMs > 800 && tapAdButton(shot, now)) {
            if (token.isCancelled()) {
                return;
            }
            continue;
        }

        // Someone else is in front: an ad landing page, the store, the launcher.
        if (now - lastFocusMs >= 6000) {
            lastFocusMs = now;
            if (!gameIsFocused(deviceId, package, token)) {
                ++focusMisses;
            } else {
                focusMisses = 0;
                returnedWithBack = false;
            }
        }
        // A click-through is undone with Back first; only a second miss, where Back
        // did not help, is worth relaunching the game for.
        if (focusMisses == 1 && !returnedWithBack) {
            returnedWithBack = true;
            publish(deviceId, QStringLiteral("Game is not in front — pressing Back"));
            shell(deviceId, QStringLiteral("input keyevent KEYCODE_BACK"), 8000, token);
            if (!waitMs(900, token)) {
                return;
            }
        }
        if (focusMisses >= 2) {
            publish(deviceId, QStringLiteral("Game is not in front — relaunching"));
            ActivityLog::instance().warning(ActivityEntry::Automation,
                                            QStringLiteral("Autoplay relaunching: the game left the foreground"), deviceId);
            shell(deviceId, launch, 20000, token);
            focusMisses = 0;
            returnedWithBack = false;
            quietMs = 0;
            lastOcrMs = 0;
            lastSeen.clear();
            rejected.clear();
            waitingForLand = false;
            if (!waitMs(1500, token)) {
                return;
            }
            continue;
        }

        // Green menu buttons (Play / Claim / Resume). On the game-over card this
        // is "CONTINUE (WATCH AD)": the autoplay takes that reward on purpose.
        //
        // With OCR available the colour button is trusted only on the game-over
        // card. A full-screen ad has wide green "Install" buttons too, so on any
        // other screen the button must carry a label that `ocrActionPoint` reads
        // (handled below). Without OCR the colour check is the only path. A board
        // that was just seen full counts as the game-over card even when OCR
        // misses its "SCORE" line — otherwise the "CONTINUE (WATCH AD)" button is
        // never pressed and the card times out.
        const bool labelsKnown = !ocrAvailable || ocrFresh;
        const bool justFinished = gameOverSeen || (boardFullMs > 0 && now - boardFullMs < 15000);
        // Windows OCR does not always read this card: on the dark theme "PLAY
        // AGAIN" comes out as "AGAIN" and the "CONTINUE" glyph can be missed
        // entirely, and while a scan has not succeeded yet (`labelsKnown` false)
        // no text can be trusted at all. A wide green banner button on a screen
        // with no readable action is that card, so after a grace period it is
        // taken — the previous autoplay tapped it unconditionally, and waiting
        // longer only lets the stall timer restart the game.
        const bool ocrSilent = !labelsKnown || actionPoint.isNull();
        bool buttonTrusted = !ocrAvailable || justFinished;
        if (scene.hasButton && !buttonTrusted) {
            if (!ocrSilent) {
                // OCR reads an action label here: the text path below knows the
                // exact button, so leave the colour match alone.
                buttonWaitSinceMs = 0;
            } else if (buttonWaitSinceMs == 0) {
                buttonWaitSinceMs = now;
            } else if (now - buttonWaitSinceMs >= 8000) {
                buttonTrusted = true;
                publish(deviceId, QStringLiteral("No readable action for 8 s — taking the button at %1,%2")
                                     .arg(scene.button.x())
                                     .arg(scene.button.y()));
            } else {
                if (!waitMs(250, token)) {
                    return;
                }
                continue;
            }
        } else {
            buttonWaitSinceMs = 0;
        }
        if (scene.hasButton && buttonTrusted) {
            const QPoint at = scene.button;
            const QString key = QStringLiteral("%1,%2").arg(at.x()).arg(at.y());
            if (key == lastTapKey) {
                ++sameTaps;
            } else {
                sameTaps = 1;
                lastTapKey = key;
            }
            if (sameTaps > 5) {
                publish(deviceId, QStringLiteral("Button does not respond — pressing Back"));
                shell(deviceId, QStringLiteral("input keyevent KEYCODE_BACK"), 8000, token);
                sameTaps = 0;
                lastTapKey.clear();
                if (!waitMs(1200, token)) {
                    return;
                }
                continue;
            }
            publish(deviceId, QStringLiteral("Tapping button at %1,%2").arg(at.x()).arg(at.y()));
            shell(deviceId, QStringLiteral("input tap %1 %2").arg(at.x()).arg(at.y()), 8000, token);
            quietMs = 0;
            buttonWaitSinceMs = 0;
            lastSeen.clear();
            rejected.clear();
            waitingForLand = false;
            // On the game-over card this tap starts a rewarded ad; leave the
            // screen alone until the ad has had time to come up (the card stays
            // visible for a few seconds while the ad loads).
            noTapUntilMs = QDateTime::currentMSecsSinceEpoch() + (justFinished ? 6000 : 2500);
            if (!waitMs(justFinished ? 1500 : 900, token)) {
                return;
            }
            continue;
        }

        // Text buttons the colour check cannot see, e.g. the orange PLAY AGAIN,
        // the dark CLOSE on the reward card, or CONTINUE on the title screen.
        if (ocrFresh) {
            const QPoint at = actionPoint;
            if (!at.isNull()) {
                const QString key = QStringLiteral("%1,%2").arg(at.x()).arg(at.y());
                if (key == lastTapKey) {
                    ++sameTaps;
                } else {
                    sameTaps = 1;
                    lastTapKey = key;
                }
                if (sameTaps > 5) {
                    publish(deviceId, QStringLiteral("Button does not respond — pressing Back"));
                    shell(deviceId, QStringLiteral("input keyevent KEYCODE_BACK"), 8000, token);
                    sameTaps = 0;
                    lastTapKey.clear();
                    if (!waitMs(1200, token)) {
                        return;
                    }
                    continue;
                }
                publish(deviceId, QStringLiteral("Tapping text button at %1,%2").arg(at.x()).arg(at.y()));
                shell(deviceId, QStringLiteral("input tap %1 %2").arg(at.x()).arg(at.y()), 8000, token);
                quietMs = 0;
                buttonWaitSinceMs = 0;
                lastSeen.clear();
                rejected.clear();
                waitingForLand = false;
                noTapUntilMs = QDateTime::currentMSecsSinceEpoch() + 2500;
                if (!waitMs(900, token)) {
                    return;
                }
                continue;
            }
        }

        if (quietMs > 30000) {
            ++relaunches;
            if (relaunches > 3) {
                publish(deviceId, QStringLiteral("No game screen for too long — giving up"));
                ActivityLog::instance().error(ActivityEntry::Automation,
                                              QStringLiteral("Autoplay gave up: no game screen"), deviceId);
                return;
            }
            publish(deviceId, QStringLiteral("Stuck on an unknown screen — relaunching"));
            ActivityLog::instance().warning(ActivityEntry::Automation,
                                            QStringLiteral("Autoplay relaunching after a long stall"), deviceId);
            shell(deviceId, launch, 20000, token);
            quietMs = 0;
            lastOcrMs = 0;
            lastSeen.clear();
            rejected.clear();
            waitingForLand = false;
            if (!waitMs(2000, token)) {
                return;
            }
            continue;
        }

        if (!waitMs(400, token)) {
            return;
        }
    }
}

} // namespace farm
