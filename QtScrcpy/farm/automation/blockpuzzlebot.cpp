#include "blockpuzzlebot.h"

#include <QCoreApplication>
#include <QMouseEvent>
#include <QRandomGenerator>

#include <algorithm>
#include <cmath>
#include <utility>

#include "adb/adbexecutor.h"
#include "adb/adbquote.h"
#include "core/activitylog.h"
#include "core/farmlog.h"
#include "core/farmsettings.h"
#include "core/taskexecutor.h"
#include "devices/deviceservice.h"
#include "mock/mockdeviceprovider.h"

namespace farm {

namespace {
const char kComponent[] = "autoplay";
constexpr int kTickMs = 250;
constexpr int kLaunchWaitMs = 9000;          // cold start + loading screen
constexpr qint64 kAdSafeSessionMs = 150000;  // the game's interstitial is due 180 s after launch
constexpr int kUnknownBackMs = 8000;
constexpr int kUnknownRelaunchMs = 25000;
constexpr int kMaxMisses = 10;
// The game's drag sensitivity levels (Settings › Sensitivity 1..5): 1 + 0.2 per level.
constexpr double kSensitivity[] = {1.0, 1.2, 1.4, 1.6, 1.8};
constexpr int kSensitivityLevels = 5;

int randomMs(int lo, int hi) { return lo + static_cast<int>(QRandomGenerator::global()->bounded(hi - lo + 1)); }

QString package()
{
    return FarmSettings::instance().stringValue(QStringLiteral("tools/autoplayPackage"), QStringLiteral("com.BlockPuzzle.BlockPuzzle"));
}

QString activity()
{
    return FarmSettings::instance().stringValue(QStringLiteral("tools/autoplayActivity"), QStringLiteral("com.unity3d.player.UnityPlayerGameActivity"));
}
} // namespace

BlockPuzzleBot::BlockPuzzleBot(const QString &id, QObject *parent)
    : QObject(parent)
    , m_id(id)
{
    m_clock.start();
    m_tick.setInterval(kTickMs);
    connect(&m_tick, &QTimer::timeout, this, &BlockPuzzleBot::tick);
    m_stepTimer.setSingleShot(true);
    connect(&m_stepTimer, &QTimer::timeout, this, &BlockPuzzleBot::playSteps);
}

BlockPuzzleBot::~BlockPuzzleBot()
{
    detach();
}

void BlockPuzzleBot::attach(qsc::IDevice *device)
{
    detach();
    if (!device) {
        return;
    }
    m_device = device;
    m_device->registerDeviceObserver(this);
    m_tick.start();
    relaunch(QStringLiteral("autoplay started"));
}

void BlockPuzzleBot::detach()
{
    m_tick.stop();
    m_stepTimer.stop();
    if (m_device && m_state == State::Acting && !m_steps.isEmpty() && m_stepIndex > 0) {
        // Never leave a finger down on the phone.
        sendTouch(QEvent::MouseButtonRelease, m_steps.at(m_stepIndex - 1).point);
    }
    m_steps.clear();
    if (m_device) {
        m_device->deRegisterDeviceObserver(this);
    }
    m_device.clear();
    m_state = State::Idle;
    m_wantFrame = false;
    ++m_generation;
}

void BlockPuzzleBot::waitFor(int ms)
{
    m_waitUntil = m_clock.elapsed() + ms;
}

void BlockPuzzleBot::tick()
{
    if (!m_device || m_state == State::Solving || m_state == State::Acting || m_clock.elapsed() < m_waitUntil) {
        return;
    }
    m_state = State::Observing;
    m_wantFrame = true;
    m_device->replayLastFrame(this);    // synchronous: onFrame runs before this returns
    m_wantFrame = false;
}

void BlockPuzzleBot::onFrame(int width, int height, uint8_t *dataY, uint8_t *dataU, uint8_t *dataV, int linesizeY, int linesizeU, int linesizeV)
{
    // Every live frame comes through here too; only the one asked for in tick() is looked at.
    if (!m_wantFrame || width <= 0 || height <= 0) {
        return;
    }
    m_wantFrame = false;
    if (m_layout.frame != QSize(width, height)) {
        m_layout = bp::Layout::forFrame(QSize(width, height));
    }
    bp::YuvView view;
    view.width = width;
    view.height = height;
    view.y = dataY;
    view.u = dataU;
    view.v = dataV;
    view.strideY = linesizeY;
    view.strideU = linesizeU;
    view.strideV = linesizeV;
    const bp::Scene scene = bp::analyze(view, m_layout);
    if (scene.kind == bp::ScreenKind::Unknown) {
        m_diagnostics = bp::describe(view, m_layout);
    }
    handle(scene);
}

void BlockPuzzleBot::handle(const bp::Scene &scene)
{
    const qint64 now = m_clock.elapsed();
    // Act only on a screen that held still for two looks (animations, line clears, pieces popping in).
    m_stable = scene == m_last ? m_stable + 1 : 1;
    m_last = scene;
    if (scene.kind != bp::ScreenKind::Unknown) {
        m_unknownSince = -1;
        m_backSent = false;
    }
    if (m_stable < 2) {
        return;
    }

    switch (scene.kind) {
    case bp::ScreenKind::Playing: {
        if (m_pending.isValid()) {
            if (scene.board == m_pending.after) {
                ++m_moves;
                ++m_gameMoves;
                m_misses = 0;
            } else {
                // The piece came back or landed elsewhere: the phone uses another drag sensitivity.
                m_sensitivity = (m_sensitivity + 1) % kSensitivityLevels;
                FarmLog::instance().info(QLatin1String(kComponent),
                    QStringLiteral("drop missed (slot %1 -> %2,%3), trying drag sensitivity %4")
                        .arg(m_pending.slot).arg(m_pending.x).arg(m_pending.y).arg(kSensitivity[m_sensitivity]), m_id);
                if (++m_misses >= kMaxMisses) {
                    m_pending = bp::Move();
                    m_misses = 0;
                    relaunch(QStringLiteral("drops keep missing"));
                    return;
                }
            }
            m_pending = bp::Move();
        }
        if (scene.handCount() == 0) {
            return;    // new hand still being dealt
        }
        m_state = State::Solving;
        const quint64 generation = m_generation;
        const bp::Mask board = scene.board;
        const bp::Hand hand = scene.hand;
        QPointer<BlockPuzzleBot> self(this);
        const bool queued = TaskExecutor::instance().run(QStringLiteral("automation"), [self, board, hand, generation]() {
            const bp::Move move = bp::bestMove(board, hand);
            QMetaObject::invokeMethod(QCoreApplication::instance(), [self, move, generation]() {
                if (self) {
                    self->onSolved(move, generation);
                }
            }, Qt::QueuedConnection);
        });
        if (!queued) {
            m_state = State::Observing;
        }
        return;
    }
    case bp::ScreenKind::GameOverFinal:
        ++m_games;
        ActivityLog::instance().info(ActivityEntry::Automation,
            tr("BlockPuzzle autoplay: game %1 over after %2 moves").arg(m_games).arg(m_gameMoves), m_id);
        emit gameOver(m_id, m_games, m_moves);
        m_gameMoves = 0;
        m_pending = bp::Move();
        tap(m_layout.canvas(0, 80));    // PLAY AGAIN
        return;
    case bp::ScreenKind::GameOverOffer:
        // The offer is a rewarded video: never taken. When the countdown runs out the game shows an
        // interstitial if one is due, so late in a session the game is restarted instead.
        m_pending = bp::Move();
        if (m_sessionStart < 0 || now - m_sessionStart > kAdSafeSessionMs) {
            relaunch(QStringLiteral("game over late in the session (skipping the interstitial)"));
        }
        return;
    case bp::ScreenKind::Menu:
        m_pending = bp::Move();
        tap(m_layout.canvas(0, 40));    // CLASSIC
        return;
    case bp::ScreenKind::Unknown:
        if (m_unknownSince < 0) {
            m_unknownSince = now;
        } else if (now - m_unknownSince > kUnknownRelaunchMs) {
            relaunch(QStringLiteral("unrecognised screen (%1)").arg(m_diagnostics));
        } else if (now - m_unknownSince > kUnknownBackMs && !m_backSent && m_device) {
            m_backSent = true;    // a popup (or anything else in the way): Back closes it
            m_device->postGoBack();
        }
        return;
    }
}

void BlockPuzzleBot::onSolved(const bp::Move &move, quint64 generation)
{
    if (generation != m_generation || m_state != State::Solving || !m_device) {
        return;
    }
    m_state = State::Observing;
    if (!move.isValid()) {
        return;    // nothing fits: the game is about to end the run by itself
    }
    const bp::Shape &shape = m_last.hand[static_cast<size_t>(move.slot)];
    // The game draws the dragged piece dragLift above the finger and moves it `sensitivity` times as far
    // as the finger travelled from where it pressed, so aim the finger accordingly.
    const QPointF from = m_layout.slotCenter(move.slot);
    const QPointF target = m_layout.dropCenter(shape, move.x, move.y);
    const double k = kSensitivity[m_sensitivity];
    QPointF to = from + (target + QPointF(0, m_layout.dragLift()) - from) / k;
    to.setX(qBound(1.0, to.x(), m_layout.frame.width() - 2.0));
    to.setY(qBound(1.0, to.y(), m_layout.frame.height() - 2.0));
    m_pending = move;
    drag(from, to, randomMs(350, 650));
}

void BlockPuzzleBot::relaunch(const QString &why)
{
    ++m_generation;
    m_pending = bp::Move();
    m_stable = 0;
    m_last = bp::Scene();
    m_unknownSince = -1;
    m_backSent = false;
    m_misses = 0;
    m_state = State::Observing;
    waitFor(kLaunchWaitMs);
    m_sessionStart = m_clock.elapsed();
    FarmLog::instance().info(QLatin1String(kComponent), QStringLiteral("restarting the game: %1").arg(why), m_id);
    const QString pkg = adb::shellQuote(package());
    // Wake the screen and drop a swipe lock first: a sleeping phone shows no game to play.
    const QString script = QStringLiteral("pm path %1 >/dev/null || { echo 'game not installed'; exit 3; }; "
                                          "input keyevent KEYCODE_WAKEUP; wm dismiss-keyguard; am force-stop %1; am start -n %2")
                               .arg(pkg, adb::shellQuote(package() + QLatin1Char('/') + activity()));
    const QString id = m_id;
    AdbExecutor::instance().shell(m_id, script, this, [id](const AdbResult &r) {
        if (!r.ok) {
            ActivityLog::instance().warning(ActivityEntry::Automation,
                tr("BlockPuzzle autoplay: can't start the game: %1").arg(r.combined().trimmed().isEmpty() ? r.error : r.combined().trimmed()), id);
        }
    }, 20000);
}

void BlockPuzzleBot::drag(const QPointF &from, const QPointF &to, int durationMs)
{
    m_steps.clear();
    // A short "thinking" pause, the press, then an eased carry at ~60 Hz and a brief hover before letting go.
    // The path is a slightly bent curve (random side and depth) with a small hand tremor that dies out
    // on approach, so the finger still ends exactly on the drop point.
    QRandomGenerator *rng = QRandomGenerator::global();
    const QPointF delta = to - from;
    const double length = std::hypot(delta.x(), delta.y());
    const QPointF normal = length > 0 ? QPointF(-delta.y() / length, delta.x() / length) : QPointF();
    const double side = rng->bounded(2) ? 1.0 : -1.0;
    const double bend = side * std::min(length * (0.06 + rng->generateDouble() * 0.12), 90.0 * m_layout.scale);
    const QPointF control = from + delta * (0.35 + rng->generateDouble() * 0.3) + normal * bend * 2.0;
    const double tremor = 3.0 * m_layout.scale;
    const double phaseX = rng->generateDouble() * 6.283;
    const double phaseY = rng->generateDouble() * 6.283;
    const double freq = 9.0 + rng->generateDouble() * 6.0;

    m_steps.append({QEvent::MouseButtonPress, from, randomMs(300, 900)});
    const int frames = std::max(8, durationMs / 16);
    for (int i = 1; i <= frames; ++i) {
        const double t = static_cast<double>(i) / frames;
        const double e = t * t * (3 - 2 * t);
        // Quadratic Bezier from -> control -> to.
        QPointF p = from * ((1 - e) * (1 - e)) + control * (2 * e * (1 - e)) + to * (e * e);
        const double fade = 1.0 - e;
        p += QPointF(std::sin(t * freq + phaseX), std::sin(t * freq * 1.3 + phaseY)) * (tremor * fade);
        p.setX(qBound(1.0, p.x(), m_layout.frame.width() - 2.0));
        p.setY(qBound(1.0, p.y(), m_layout.frame.height() - 2.0));
        m_steps.append({QEvent::MouseMove, p, i == 1 ? 60 : 16});
    }
    m_steps.append({QEvent::MouseMove, to, randomMs(60, 160)});
    m_steps.append({QEvent::MouseButtonRelease, to, 30});
    m_stepIndex = 0;
    m_state = State::Acting;
    m_stepTimer.start(m_steps.first().delayMs);
}

void BlockPuzzleBot::tap(const QPointF &point)
{
    m_steps.clear();
    m_steps.append({QEvent::MouseButtonPress, point, randomMs(400, 900)});
    m_steps.append({QEvent::MouseButtonRelease, point, randomMs(60, 120)});
    m_stepIndex = 0;
    m_state = State::Acting;
    m_stepTimer.start(m_steps.first().delayMs);
}

void BlockPuzzleBot::playSteps()
{
    if (!m_device || m_stepIndex >= m_steps.size()) {
        m_state = m_device ? State::Observing : State::Idle;
        return;
    }
    const Step step = m_steps.at(m_stepIndex++);
    sendTouch(step.type, step.point);
    if (m_stepIndex < m_steps.size()) {
        m_stepTimer.start(m_steps.at(m_stepIndex).delayMs);
        return;
    }
    m_steps.clear();
    m_state = State::Observing;
    m_stable = 0;
    waitFor(500);    // let the drop / line clear / new hand start animating before looking again
}

void BlockPuzzleBot::sendTouch(QEvent::Type type, const QPointF &point)
{
    if (!m_device || !m_layout.isValid()) {
        return;
    }
    const Qt::MouseButtons buttons = type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton;
    const Qt::MouseButton button = type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton;
    QMouseEvent event(type, point, point, point, button, buttons, Qt::NoModifier);
    // Points are in frame pixels, so the frame is also the "shown" size.
    m_device->mouseEvent(&event, m_layout.frame, m_layout.frame);
}

// ---------------------------------------------------------------------------

BlockPuzzleAutoplay &BlockPuzzleAutoplay::instance()
{
    static BlockPuzzleAutoplay inst;
    return inst;
}

BlockPuzzleAutoplay::BlockPuzzleAutoplay(QObject *parent)
    : QObject(parent)
{
    connect(&DeviceService::instance(), &DeviceService::mirrorStarted, this, [this](const QString &id, const QSize &) { onMirrorStarted(id); });
    connect(&DeviceService::instance(), &DeviceService::mirrorStopped, this, &BlockPuzzleAutoplay::onMirrorStopped);
}

int BlockPuzzleAutoplay::start(const QStringList &ids)
{
    const bool wasRunning = isRunning();
    QStringList toMirror;
    for (const QString &id : ids) {
        if (m_bots.contains(id) || MockDeviceProvider::isMockId(id)) {
            continue;
        }
        auto *bot = new BlockPuzzleBot(id, this);
        m_bots.insert(id, bot);
        if (auto d = DeviceService::instance().device(id)) {
            bot->attach(d);
        } else if (!DeviceService::instance().isStarting(id)) {
            toMirror << id;    // the bot plays through the mirror; it attaches once mirroring starts
        }
    }
    if (!toMirror.isEmpty()) {
        DeviceService::instance().startMirror(toMirror);
    }
    if (isRunning() != wasRunning) {
        emit runningChanged(true);
    }
    FarmLog::instance().info(QLatin1String(kComponent), QStringLiteral("autoplay on %1 device(s)").arg(m_bots.size()));
    return static_cast<int>(m_bots.size());
}

void BlockPuzzleAutoplay::stop()
{
    if (m_bots.isEmpty()) {
        return;
    }
    int games = 0;
    for (BlockPuzzleBot *bot : std::as_const(m_bots)) {
        games += bot->games();
        delete bot;
    }
    m_bots.clear();
    ActivityLog::instance().info(ActivityEntry::Automation, tr("BlockPuzzle autoplay stopped (%1 games played)").arg(games));
    emit runningChanged(false);
}

void BlockPuzzleAutoplay::onMirrorStarted(const QString &id)
{
    BlockPuzzleBot *bot = m_bots.value(id);
    if (bot && !bot->isAttached()) {
        if (auto d = DeviceService::instance().device(id)) {
            bot->attach(d);
        }
    }
}

void BlockPuzzleAutoplay::onMirrorStopped(const QString &id)
{
    if (BlockPuzzleBot *bot = m_bots.value(id)) {
        bot->detach();    // resumes on the next mirrorStarted (reconnects included)
    }
}

} // namespace farm
