#ifndef FARM_AUTOMATION_BLOCKPUZZLEPLAYER_H
#define FARM_AUTOMATION_BLOCKPUZZLEPLAYER_H

#include <atomic>

#include <QMutex>
#include <QObject>
#include <QStringList>

#include "core/taskexecutor.h"

namespace farm {

/**
 * Launches Block Puzzle on the selected phones and plays it from screenshots.
 * Intentionally picks the worst placement each turn (minimises GreedyBot score)
 * so the board fills up quickly and the game ends naturally. Play Again /
 * Claim / Resume buttons are tapped automatically; the bot loops until stopped.
 * The bottom ad banner is never tapped.
 *
 * One worker per device on the automation lane. Touch is `input swipe`.
 */
class BlockPuzzlePlayer : public QObject
{
    Q_OBJECT
public:
    static BlockPuzzlePlayer &instance();

    bool running() const { return m_active.load() > 0; }
    void start(const QStringList &deviceIds);
    void stop();

signals:
    void runningChanged(bool running);
    void statusChanged(const QString &deviceId, const QString &text);

private:
    explicit BlockPuzzlePlayer(QObject *parent = nullptr);
    void playDevice(const QString &deviceId, CancellationToken token);
    void publish(const QString &deviceId, const QString &text);
    void finishedOne();

    std::atomic<int> m_active{0};
    CancellationToken m_token;
    QMutex m_mutex;
};

} // namespace farm

#endif // FARM_AUTOMATION_BLOCKPUZZLEPLAYER_H
