#ifndef FARM_AUTOMATION_BLOCKPUZZLEBOT_H
#define FARM_AUTOMATION_BLOCKPUZZLEBOT_H

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QPointF>
#include <QTimer>
#include <QVector>

#include "QtScrcpyCore.h"
#include "blockpuzzle.h"

namespace farm {

/**
 * Plays BlockPuzzle on one mirrored phone. It looks at the frames the mirror
 * already decodes (the last one is re-read with replayLastFrame, analysed in
 * place, never stored) and plays through scrcpy touch injection.
 *
 * Ads are never watched: CONTINUE (revive for a rewarded video) is never
 * pressed, nothing near the banner is touched, and when a run ends late in a
 * session the game is restarted instead, because its interstitial is only due
 * 180 s after launch — a fresh session ends the run without one.
 *
 * GUI thread only; the solver runs on the automation lane.
 */
class BlockPuzzleBot : public QObject, public qsc::DeviceObserver
{
    Q_OBJECT
public:
    BlockPuzzleBot(const QString &id, QObject *parent = nullptr);
    ~BlockPuzzleBot() override;

    QString id() const { return m_id; }
    void attach(qsc::IDevice *device);
    void detach();
    bool isAttached() const { return !m_device.isNull(); }
    int games() const { return m_games; }
    int moves() const { return m_moves; }

    void onFrame(int width, int height, uint8_t *dataY, uint8_t *dataU, uint8_t *dataV, int linesizeY, int linesizeU, int linesizeV) override;

signals:
    void gameOver(const QString &id, int games, int moves);

private:
    enum class State { Idle, Observing, Solving, Acting };

    void tick();
    void handle(const bp::Scene &scene);
    void onSolved(const bp::Move &move, quint64 generation);
    void relaunch(const QString &why);
    void waitFor(int ms);
    void drag(const QPointF &from, const QPointF &to, int durationMs);
    void tap(const QPointF &point);
    void playSteps();
    void sendTouch(QEvent::Type type, const QPointF &point);

    struct Step
    {
        QEvent::Type type;
        QPointF point;
        int delayMs;    // before this step
    };

    QString m_id;
    QString m_diagnostics;        // colours of the last unrecognised screen, for the log
    QPointer<qsc::IDevice> m_device;
    QTimer m_tick;
    QTimer m_stepTimer;
    QVector<Step> m_steps;
    int m_stepIndex = 0;
    State m_state = State::Idle;
    bool m_wantFrame = false;
    bp::Layout m_layout;
    bp::Scene m_last;
    int m_stable = 0;
    QElapsedTimer m_clock;
    qint64 m_waitUntil = 0;
    qint64 m_sessionStart = -1;   // when the bot last (re)started the game; -1 = unknown
    qint64 m_unknownSince = -1;
    bool m_backSent = false;
    quint64 m_generation = 0;
    bp::Move m_pending;           // the move just dropped, checked on the next stable scene
    int m_sensitivity = 0;        // index into the game's drag-sensitivity levels
    int m_misses = 0;
    int m_games = 0;
    int m_moves = 0;
    int m_gameMoves = 0;
};

/// The farm-wide Autoplay switch: one bot per selected phone, attached while it mirrors.
class BlockPuzzleAutoplay : public QObject
{
    Q_OBJECT
public:
    static BlockPuzzleAutoplay &instance();

    /// Starts bots on `ids` (phones that aren't mirroring yet are started). Returns how many got a bot.
    int start(const QStringList &ids);
    void stop();
    bool isRunning() const { return !m_bots.isEmpty(); }
    QStringList deviceIds() const { return m_bots.keys(); }

signals:
    void runningChanged(bool running);

private:
    explicit BlockPuzzleAutoplay(QObject *parent = nullptr);
    void onMirrorStarted(const QString &id);
    void onMirrorStopped(const QString &id);

    QHash<QString, BlockPuzzleBot *> m_bots;
};

} // namespace farm

#endif // FARM_AUTOMATION_BLOCKPUZZLEBOT_H
