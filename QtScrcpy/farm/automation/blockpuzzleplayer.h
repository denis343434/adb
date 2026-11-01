#ifndef FARM_AUTOMATION_BLOCKPUZZLEPLAYER_H
#define FARM_AUTOMATION_BLOCKPUZZLEPLAYER_H

#include <atomic>

#include <QObject>
#include <QStringList>

#include "core/taskexecutor.h"

namespace farm {

/**
 * Launches Block Puzzle on the selected phones and plays it from screenshots.
 * Intentionally picks the worst placement each turn (minimises GreedyBot score)
 * so the board fills up quickly and the game ends naturally. Play Again /
 * Continue / Claim buttons are tapped automatically; the bot loops until stopped.
 *
 * While a full-screen ad is showing the bot does not play, but it looks for the
 * ad's own Skip (">|") and Close ("x") controls on every frame and taps one when
 * a crop in skip_ads/buttons matches (see AdButtonFinder). A tap lands on a
 * random point inside the control, and consecutive taps are 2..5 s apart. Ads
 * without a control on screen are watched to the end, so a rewarded ad keeps its
 * reward; play resumes as soon as the game screen is readable again (playable
 * board, menu/claim button, or the game-over card). The only other action during
 * an ad is a Back key press after three minutes, for a stuck creative.
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
};

} // namespace farm

#endif // FARM_AUTOMATION_BLOCKPUZZLEPLAYER_H
