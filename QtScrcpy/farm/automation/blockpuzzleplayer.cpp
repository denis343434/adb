#include "blockpuzzleplayer.h"

#include <algorithm>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QScopeGuard>
#include <QThread>

#include "adb/adbexecutor.h"
#include "adb/adbquote.h"
#include "automation/blockpuzzle.h"
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

void BlockPuzzlePlayer::startAdScript()
{
    if (m_adProcess != nullptr) {
        return; // already running
    }
    const QString appDir = QCoreApplication::applicationDirPath();
    const QString scriptPath = QDir(appDir).filePath(QStringLiteral("skip_ads/skip_ads.py"));
    if (!QFile::exists(scriptPath)) {
        FarmLog::instance().warning(QStringLiteral("autoplay"),
                                    QStringLiteral("skip_ads.py not found at %1 — ad script disabled").arg(scriptPath));
        return;
    }
    m_adProcess = new QProcess(this);
    m_adProcess->setWorkingDirectory(QDir(appDir).filePath(QStringLiteral("skip_ads")));
    m_adProcess->start(QStringLiteral("python"), QStringList{ scriptPath });
    if (!m_adProcess->waitForStarted(3000)) {
        FarmLog::instance().warning(QStringLiteral("autoplay"),
                                    QStringLiteral("Could not start skip_ads.py — python not found or script error"));
        delete m_adProcess;
        m_adProcess = nullptr;
        return;
    }
    FarmLog::instance().info(QStringLiteral("autoplay"),
                             QStringLiteral("Ad skip script started (pid %1)").arg(m_adProcess->processId()));
    ActivityLog::instance().info(ActivityEntry::Automation,
                                 QStringLiteral("Ad skip script started"));
}

void BlockPuzzlePlayer::stopAdScript()
{
    if (m_adProcess == nullptr) {
        return;
    }
    m_adProcess->terminate();
    if (!m_adProcess->waitForFinished(3000)) {
        m_adProcess->kill();
    }
    FarmLog::instance().info(QStringLiteral("autoplay"), QStringLiteral("Ad skip script stopped"));
    delete m_adProcess;
    m_adProcess = nullptr;
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
    m_adScriptUsers.store(0);
    const int needed = static_cast<int>(ids.size());
    m_active.store(needed);
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
    stopAdScript();
    ActivityLog::instance().info(ActivityEntry::Automation, QStringLiteral("Autoplay stopping"));
}

void BlockPuzzlePlayer::finishedOne()
{
    const int left = m_active.fetch_sub(1) - 1;
    if (left <= 0) {
        m_active.store(0);
        stopAdScript();
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
        ActivityLog::instance().error(ActivityEntry::Automation, QStringLiteral("Could not launch %1").arg(package), deviceId);
        return;
    }
    if (!waitMs(400, token)) {
        return;
    }

    int placed = 0;
    int shotFails = 0;   // consecutive null screenshots
    int noBoardMs = 0;   // ms spent without seeing the board (resets when board/button found)
    int buttonTaps = 0;
    int stalls = 0;
    bool waitingForLand = false;
    bool inAdPhase = false; // true while this device has no board (ad or transition)
    QString lastSeen;
    QVector<PuzzleMove> rejected;
    // Release this device's claim on the ad script when the worker exits for any reason.
    auto adPhaseCleanup = qScopeGuard([this, &inAdPhase]() {
        if (inAdPhase && m_adScriptUsers.fetch_sub(1) == 1) {
            QMetaObject::invokeMethod(this, [this]() { stopAdScript(); }, Qt::QueuedConnection);
        }
    });
    // A short drag still lands on this game. A drag that misses is retried slower.
    int swipeMs = 700;
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

        // Buttons: green menus, dark-circle restart, red ad play buttons.
        if (scene.hasButton) {
            noBoardMs = 0;
            ++buttonTaps;
            if (buttonTaps > 20) {
                publish(deviceId, QStringLiteral("Stuck on a screen — restarting"));
                ActivityLog::instance().warning(ActivityEntry::Automation, QStringLiteral("Autoplay stuck on a screen"), deviceId);
                return;
            }
            publish(deviceId, QStringLiteral("Tapping button at %1,%2").arg(scene.button.x()).arg(scene.button.y()));
            shell(deviceId,
                  QStringLiteral("input tap %1 %2").arg(scene.button.x()).arg(scene.button.y()),
                  8000, token);
            rejected.clear();
            lastSeen.clear();
            waitingForLand = false;
            if (!waitMs(600, token)) {
                return;
            }
            continue;
        }
        buttonTaps = 0;

        if (!scene.boardFound) {
            // Start ad script on first frame without a board.
            if (!inAdPhase) {
                inAdPhase = true;
                if (m_adScriptUsers.fetch_add(1) == 0) {
                    QMetaObject::invokeMethod(this, [this]() { startAdScript(); }, Qt::QueuedConnection);
                }
            }
            // Between game-over and the next round there may be ads (interstitials,
            // video overlays, reward screens) that have no detectable button.
            // We periodically tap the center of the screen to advance past them.
            noBoardMs += 400;
            if (noBoardMs > 60000) {
                // One minute with no board and no detectable button — give up.
                publish(deviceId, QStringLiteral("Board lost for too long"));
                ActivityLog::instance().warning(ActivityEntry::Automation, QStringLiteral("Autoplay lost the board"), deviceId);
                return;
            }
            // Every ~3 s tap the center to try to dismiss/advance ads.
            if (noBoardMs % 3000 < 400) {
                const int cx = shot.width() / 2;
                const int cy = shot.height() / 2;
                publish(deviceId, QStringLiteral("No board — tapping center to dismiss ad"));
                shell(deviceId,
                      QStringLiteral("input tap %1 %2").arg(cx).arg(cy),
                      8000, token);
            }
            if (!waitMs(400, token)) {
                return;
            }
            continue;
        }
        noBoardMs = 0;
        // Board is back — stop ad script if this was the last device in ad phase.
        if (inAdPhase) {
            inAdPhase = false;
            if (m_adScriptUsers.fetch_sub(1) == 1) {
                QMetaObject::invokeMethod(this, [this]() { stopAdScript(); }, Qt::QueuedConnection);
            }
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
        }
        const PuzzleMove move = BlockPuzzle::choose(scene, rejected);
        if (!move.ok()) {
            ++stalls;
            if (stalls > 8) {
                publish(deviceId, QStringLiteral("No legal move"));
                ActivityLog::instance().info(ActivityEntry::Automation, QStringLiteral("Autoplay has no legal move"), deviceId);
                return;
            }
            publish(deviceId, QStringLiteral("No legal move — waiting"));
            if (!waitMs(250, token)) {
                return;
            }
            continue;
        }
        stalls = 0;
        const PuzzleDrag drag = BlockPuzzle::dragFor(scene, move);
        if (!drag.ok) {
            rejected.append(move);
            continue;
        }
        const int x1 = std::clamp(drag.from.x(), 1, shot.width() - 2);
        const int y1 = std::clamp(drag.from.y(), 1, shot.height() - 2);
        const int x2 = std::clamp(drag.to.x(), 1, shot.width() - 2);
        // The piece is drawn about two cells above the finger, so the release
        // sits two cells below the target. A release on the cell itself drops
        // the piece above the board.
        int lift = 0;
        if (scene.rowCenter.size() > 1) {
            lift = static_cast<int>(std::lround((scene.rowCenter.at(1) - scene.rowCenter.at(0)) * 2.0));
        }
        const int y2 = std::clamp(drag.to.y() + lift, 1, static_cast<int>(shot.height() * 0.92));
        shell(deviceId,
              QStringLiteral("input swipe %1 %2 %3 %4 %5").arg(x1).arg(y1).arg(x2).arg(y2).arg(swipeMs),
              8000 + swipeMs, token);
        waitingForLand = true;
        publish(deviceId, QStringLiteral("Dragging %1 toward row %2").arg(shapeText(scene.pieces.at(move.piece).shape)).arg(move.row + 1));
        rejected.append(move);
        if (rejected.size() >= 3) {
            swipeMs = 1100;
        }
        if (rejected.size() > 12) {
            publish(deviceId, QStringLiteral("Drags are not landing — stopped"));
            ActivityLog::instance().warning(ActivityEntry::Automation, QStringLiteral("Autoplay drags were not landing"), deviceId);
            return;
        }
        if (!waitMs(80, token)) {
            return;
        }
    }
}

} // namespace farm
