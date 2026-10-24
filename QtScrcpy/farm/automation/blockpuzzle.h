#ifndef FARM_AUTOMATION_BLOCKPUZZLE_H
#define FARM_AUTOMATION_BLOCKPUZZLE_H

#include <array>
#include <cstdint>

#include <QPoint>
#include <QPointF>
#include <QSize>
#include <QString>
#include <QVector>

namespace farm {
namespace bp {

/**
 * BlockPuzzle (com.BlockPuzzle.BlockPuzzle) as seen from the mirror stream: screen
 * geometry, scene recognition straight from the decoded YUV frame (nothing is
 * captured or saved) and the move solver. Pure functions, no Qt objects, so the
 * solver can run on a worker lane and everything is unit-testable.
 *
 * Geometry follows the game's canvas: 1080x1920 reference, scaled by width on
 * tall screens and by height otherwise; board 880 units wide (8 cells of 103 +
 * 8 spacing) centred 150 above the screen centre; hand slots 330 apart, 325
 * above the bottom edge; pieces in the hand drawn at a 52 pitch.
 */

constexpr int kSize = 8;
using Mask = quint64;

inline Mask bit(int x, int y) { return Mask(1) << (y * kSize + x); }

struct Shape
{
    QVector<QPoint> cells;    // normalised: min x and min y are 0
    int width = 0;
    int height = 0;

    bool isEmpty() const { return cells.isEmpty(); }
    Mask maskAt(int x, int y) const;
    bool operator==(const Shape &o) const { return width == o.width && height == o.height && cells == o.cells; }
    bool operator!=(const Shape &o) const { return !(*this == o); }
    static Shape fromCells(QVector<QPoint> cells);
};

using Hand = std::array<Shape, 3>;

/// Frame-pixel positions of everything the bot looks at or touches.
struct Layout
{
    QSize frame;
    double scale = 1.0;           // frame pixels per canvas unit

    static Layout forFrame(const QSize &frame);
    bool isValid() const { return !frame.isEmpty(); }
    /// Canvas point (units from the screen centre, y up) in frame pixels.
    QPointF canvas(double cx, double cy) const;
    double boardPitch() const { return 111.0 * scale; }
    QPointF cellCenter(int x, int y) const;
    QPointF slotCenter(int slot) const;
    double handPitch() const { return 52.0 * scale; }
    /// Where the dragged piece's centre has to be for `shape` to land at (x, y).
    QPointF dropCenter(const Shape &shape, int x, int y) const;
    /// How far above the finger the game draws a dragged piece (8% of the screen height).
    double dragLift() const { return frame.height() * 0.08; }
};

/// Read-only view of one decoded frame (planar YUV 4:2:0, as the decoder hands it out).
struct YuvView
{
    int width = 0;
    int height = 0;
    const uint8_t *y = nullptr;
    const uint8_t *u = nullptr;
    const uint8_t *v = nullptr;
    int strideY = 0;
    int strideU = 0;
    int strideV = 0;

    struct Rgb { int r = 0, g = 0, b = 0; };
    Rgb rgbAt(int x, int yy) const;
    /// Average colour of a (2*radius+1)^2 grid of samples `step` pixels apart.
    Rgb average(QPointF center, int radius, double step) const;
};

enum class ScreenKind {
    Unknown,
    Playing,          // board and hand visible
    GameOverOffer,    // game over with the revive-for-an-ad offer / countdown: never tapped
    GameOverFinal,    // PLAY AGAIN is up
    Menu,             // main menu with CLASSIC
};

struct Scene
{
    ScreenKind kind = ScreenKind::Unknown;
    Mask board = 0;
    Hand hand;

    int handCount() const;
    bool operator==(const Scene &o) const { return kind == o.kind && board == o.board && hand == o.hand; }
    bool operator!=(const Scene &o) const { return !(*this == o); }
};

Scene analyze(const YuvView &frame, const Layout &layout);

/// One log line with the colours at the points analyze() looks at (diagnosing an unrecognised screen).
QString describe(const YuvView &frame, const Layout &layout);

/// Places `piece` (already positioned) and clears full rows/columns.
Mask place(Mask board, Mask piece, int *lines = nullptr);

struct Move
{
    int slot = -1;
    int x = 0;
    int y = 0;
    Mask before = 0;
    Mask after = 0;
    bool isValid() const { return slot >= 0; }
};

/// First move of the best order/placement of the whole hand (lines cleared, then how open the board stays).
Move bestMove(Mask board, const Hand &hand);

} // namespace bp
} // namespace farm

#endif // FARM_AUTOMATION_BLOCKPUZZLE_H
