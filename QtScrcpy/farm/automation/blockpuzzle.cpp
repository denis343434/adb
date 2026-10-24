#include "blockpuzzle.h"

#include <QtGlobal>

#include <algorithm>
#include <limits>

namespace farm {
namespace bp {

namespace {

// Canvas geometry (units of the game's 1080x1920 reference canvas, y up from the screen centre).
constexpr double kBoardLeft = -440.0;     // board cells: 880 wide, centred 150 above the middle
constexpr double kBoardTop = 590.0;
constexpr double kCell = 103.0;
constexpr double kPitch = 111.0;
constexpr double kSlotSpacing = 330.0;
constexpr double kSlotFromBottom = 325.0; // banner reserve 155 + 170
constexpr double kSlotHalf = 140.0;       // largest piece is 5 x 52 = 260 wide
constexpr double kHandPitch = 52.0;
constexpr double kHandCell = 52.0 * 0.92;

constexpr Mask kCol0 = 0x0101010101010101ULL;

Mask columnsFrom(int first, int last)
{
    Mask m = 0;
    for (int x = first; x <= last; ++x) {
        m |= kCol0 << x;
    }
    return m;
}

/// Moves every cell by (dx, dy); cells pushed off the board disappear.
Mask shift(Mask m, int dx, int dy)
{
    if (dx > 0) {
        m = (m & columnsFrom(0, kSize - 1 - dx)) << dx;
    } else if (dx < 0) {
        m = (m & columnsFrom(-dx, kSize - 1)) >> -dx;
    }
    if (dy > 0) {
        m <<= 8 * dy;
    } else if (dy < 0) {
        m >>= 8 * -dy;
    }
    return m;
}

int popcount(Mask m) { return static_cast<int>(qPopulationCount(m)); }

/// Origins where a w x h rectangle fits into the free cells.
Mask rectFits(Mask freeCells, int w, int h)
{
    Mask fits = freeCells;
    for (int dy = 0; dy < h; ++dy) {
        for (int dx = 0; dx < w; ++dx) {
            fits &= shift(freeCells, -dx, -dy);
        }
    }
    // Origins whose rectangle would leave the board.
    return fits & columnsFrom(0, kSize - w) & (~Mask(0) >> (8 * (h - 1)));
}

/// How good a board is to keep playing on: free space, room for the big pieces, no single-cell holes.
double evaluate(Mask board)
{
    const Mask freeCells = ~board;
    const int empty = popcount(freeCells);
    const Mask right = shift(freeCells, -1, 0);
    const Mask left = shift(freeCells, 1, 0);
    const Mask down = shift(freeCells, 0, -1);
    const Mask up = shift(freeCells, 0, 1);
    const int holes = popcount(freeCells & ~right & ~left & ~down & ~up);
    // Filled/empty changes along rows and columns (the edges count as filled): a ragged board.
    const int transitions = popcount(board ^ (shift(board, 1, 0) | kCol0)) + popcount(freeCells & (kCol0 << 7))
                          + popcount(board ^ (shift(board, 0, 1) | Mask(0xFF))) + popcount(freeCells & (Mask(0xFF) << 56));
    double score = empty * 2.0 - holes * 6.0 - transitions * 1.0;
    if (rectFits(freeCells, 3, 3)) score += 15;
    if (rectFits(freeCells, 5, 1)) score += 6;
    if (rectFits(freeCells, 1, 5)) score += 6;
    if (rectFits(freeCells, 2, 2)) score += 4;
    return score;
}

struct Search
{
    const Hand *hand = nullptr;
    int bestPlaced = -1;
    double bestValue = -std::numeric_limits<double>::infinity();
    Move best;

    void run(Mask board, int used, int placed, double gained, const Move &first)
    {
        bool any = false;
        for (int slot = 0; slot < 3; ++slot) {
            const Shape &s = (*hand)[static_cast<size_t>(slot)];
            if ((used & (1 << slot)) || s.isEmpty()) {
                continue;
            }
            for (int y = 0; y + s.height <= kSize; ++y) {
                for (int x = 0; x + s.width <= kSize; ++x) {
                    const Mask piece = s.maskAt(x, y);
                    if (board & piece) {
                        continue;
                    }
                    any = true;
                    int lines = 0;
                    const Mask next = place(board, piece, &lines);
                    Move m = first;
                    if (!m.isValid()) {
                        m.slot = slot;
                        m.x = x;
                        m.y = y;
                        m.before = board;
                        m.after = next;
                    }
                    // Clearing several lines in one move scores combos in the game.
                    run(next, used | (1 << slot), placed + 1, gained + lines * 20.0 + (lines > 1 ? lines * lines * 5.0 : 0.0), m);
                }
            }
        }
        if (any || !first.isValid()) {
            return;
        }
        const double value = gained + evaluate(board);
        if (placed > bestPlaced || (placed == bestPlaced && value > bestValue)) {
            bestPlaced = placed;
            bestValue = value;
            best = first;
        }
    }
};

int saturation(const YuvView::Rgb &c) { return std::max({c.r, c.g, c.b}) - std::min({c.r, c.g, c.b}); }
int luma(const YuvView::Rgb &c) { return (c.r * 299 + c.g * 587 + c.b * 114) / 1000; }
bool isBlock(const YuvView::Rgb &c) { return saturation(c) > 70 || luma(c) > 140; }
bool isEmptyCell(const YuvView::Rgb &c)
{
    const int l = luma(c);
    return saturation(c) < 55 && l >= 28 && l <= 115;
}
bool closeTo(const YuvView::Rgb &c, int r, int g, int b) { return qAbs(c.r - r) + qAbs(c.g - g) + qAbs(c.b - b) < 90; }

/// A button is recognised by its fill colour on both sides of its label.
bool buttonAt(const YuvView &f, const Layout &l, double cy, int r, int g, int b)
{
    const double step = 4.0 * l.scale;
    return closeTo(f.average(l.canvas(-280, cy), 1, step), r, g, b) && closeTo(f.average(l.canvas(280, cy), 1, step), r, g, b);
}

Shape readSlot(const YuvView &f, const Layout &l, int slot)
{
    const QPointF c = l.slotCenter(slot);
    const double half = kSlotHalf * l.scale;
    const int step = std::max(1, qRound(l.scale * 1.5));
    const int x0 = std::max(0, qRound(c.x() - half));
    const int x1 = std::min(f.width - 1, qRound(c.x() + half));
    const int y0 = std::max(0, qRound(c.y() - half));
    const int y1 = std::min(f.height - 1, qRound(c.y() + half));
    int minX = x1 + 1, maxX = -1, minY = y1 + 1, maxY = -1, count = 0;
    for (int y = y0; y <= y1; y += step) {
        for (int x = x0; x <= x1; x += step) {
            if (!isBlock(f.rgbAt(x, y))) {
                continue;
            }
            ++count;
            minX = std::min(minX, x);
            maxX = std::max(maxX, x);
            minY = std::min(minY, y);
            maxY = std::max(maxY, y);
        }
    }
    // Less than half a hand cell of coloured pixels: an empty slot (or noise).
    const double cellArea = (kHandCell * l.scale) * (kHandCell * l.scale) / (step * step);
    if (count < cellArea * 0.5) {
        return Shape();
    }
    const double bw = (maxX - minX + step) / l.scale;
    const double bh = (maxY - minY + step) / l.scale;
    const int cols = qBound(1, qRound((bw - kHandCell) / kHandPitch) + 1, 5);
    const int rows = qBound(1, qRound((bh - kHandCell) / kHandPitch) + 1, 5);
    QVector<QPoint> cells;
    for (int r = 0; r < rows; ++r) {
        for (int cIdx = 0; cIdx < cols; ++cIdx) {
            const QPointF p(minX + (kHandCell / 2 + cIdx * kHandPitch) * l.scale, minY + (kHandCell / 2 + r * kHandPitch) * l.scale);
            if (isBlock(f.average(p, 1, kHandCell * l.scale * 0.2))) {
                cells.append(QPoint(cIdx, r));
            }
        }
    }
    return Shape::fromCells(cells);
}

} // namespace

Mask Shape::maskAt(int x, int y) const
{
    Mask m = 0;
    for (const QPoint &c : cells) {
        m |= bit(x + c.x(), y + c.y());
    }
    return m;
}

Shape Shape::fromCells(QVector<QPoint> cells)
{
    Shape s;
    if (cells.isEmpty()) {
        return s;
    }
    int minX = kSize, minY = kSize, maxX = 0, maxY = 0;
    for (const QPoint &c : cells) {
        minX = std::min(minX, c.x());
        minY = std::min(minY, c.y());
        maxX = std::max(maxX, c.x());
        maxY = std::max(maxY, c.y());
    }
    for (QPoint &c : cells) {
        c -= QPoint(minX, minY);
    }
    std::sort(cells.begin(), cells.end(), [](const QPoint &a, const QPoint &b) {
        return a.y() != b.y() ? a.y() < b.y() : a.x() < b.x();
    });
    s.cells = cells;
    s.width = maxX - minX + 1;
    s.height = maxY - minY + 1;
    return s;
}

Layout Layout::forFrame(const QSize &frame)
{
    Layout l;
    l.frame = frame;
    if (frame.isEmpty()) {
        return l;
    }
    // CanvasScaler: match width on screens at least as tall as 16:9, height otherwise.
    const bool tall = static_cast<qint64>(frame.height()) * 1080 >= static_cast<qint64>(frame.width()) * 1920;
    l.scale = tall ? frame.width() / 1080.0 : frame.height() / 1920.0;
    return l;
}

QPointF Layout::canvas(double cx, double cy) const
{
    return QPointF(frame.width() / 2.0 + cx * scale, frame.height() / 2.0 - cy * scale);
}

QPointF Layout::cellCenter(int x, int y) const
{
    return canvas(kBoardLeft + x * kPitch + kCell / 2, kBoardTop - y * kPitch - kCell / 2);
}

QPointF Layout::slotCenter(int slot) const
{
    return QPointF(frame.width() / 2.0 + (slot - 1) * kSlotSpacing * scale, frame.height() - kSlotFromBottom * scale);
}

QPointF Layout::dropCenter(const Shape &shape, int x, int y) const
{
    // While dragged the piece is drawn at the board pitch, centred on its own bounding box.
    const double cx = kBoardLeft + x * kPitch + shape.width * kPitch / 2 - (kPitch - kCell) / 2;
    const double cy = kBoardTop - y * kPitch - shape.height * kPitch / 2 + (kPitch - kCell) / 2;
    return canvas(cx, cy);
}

YuvView::Rgb YuvView::rgbAt(int x, int yy) const
{
    x = qBound(0, x, width - 1);
    yy = qBound(0, yy, height - 1);
    // BT.601 limited range, what Android encoders produce.
    const int c = y[yy * strideY + x] - 16;
    const int d = u[(yy / 2) * strideU + x / 2] - 128;
    const int e = v[(yy / 2) * strideV + x / 2] - 128;
    Rgb out;
    out.r = qBound(0, (298 * c + 409 * e + 128) >> 8, 255);
    out.g = qBound(0, (298 * c - 100 * d - 208 * e + 128) >> 8, 255);
    out.b = qBound(0, (298 * c + 516 * d + 128) >> 8, 255);
    return out;
}

YuvView::Rgb YuvView::average(QPointF center, int radius, double step) const
{
    Rgb sum;
    int n = 0;
    for (int j = -radius; j <= radius; ++j) {
        for (int i = -radius; i <= radius; ++i) {
            const Rgb c = rgbAt(qRound(center.x() + i * step), qRound(center.y() + j * step));
            sum.r += c.r;
            sum.g += c.g;
            sum.b += c.b;
            ++n;
        }
    }
    sum.r /= n;
    sum.g /= n;
    sum.b /= n;
    return sum;
}

int Scene::handCount() const
{
    return static_cast<int>(std::count_if(hand.begin(), hand.end(), [](const Shape &s) { return !s.isEmpty(); }));
}

Scene analyze(const YuvView &f, const Layout &l)
{
    Scene scene;
    if (!l.isValid() || f.width != l.frame.width() || f.height != l.frame.height()) {
        return scene;
    }
    bool boardVisible = true;
    Mask board = 0;
    int emptyLuma = 0;
    int emptyCount = 0;
    const double step = kCell * l.scale * 0.15;
    for (int y = 0; y < kSize && boardVisible; ++y) {
        for (int x = 0; x < kSize; ++x) {
            const YuvView::Rgb c = f.average(l.cellCenter(x, y), 2, step);
            if (isBlock(c)) {
                board |= bit(x, y);
            } else if (isEmptyCell(c)) {
                emptyLuma += luma(c);
                ++emptyCount;
            } else {
                boardVisible = false;    // dimmed by an overlay, or not the game screen
                break;
            }
        }
    }
    if (boardVisible) {
        // The board sits in a dark frame (20 units wide around the cells), darker than its empty cells;
        // a flat backdrop or a full-screen ad has none.
        const int frameMax = emptyCount > 0 ? std::min(emptyLuma / emptyCount - 10, 70) : 70;
        const double frameStep = 2.0 * l.scale;
        for (const QPointF &p : {l.canvas(kBoardLeft - 10, 150), l.canvas(-kBoardLeft + 10, 150),
                                 l.canvas(0, kBoardTop + 10), l.canvas(0, kBoardTop - 8 * kPitch + (kPitch - kCell) - 10)}) {
            if (luma(f.average(p, 1, frameStep)) > frameMax) {
                boardVisible = false;
                break;
            }
        }
    }
    if (boardVisible) {
        scene.kind = ScreenKind::Playing;
        scene.board = board;
        for (int i = 0; i < 3; ++i) {
            scene.hand[static_cast<size_t>(i)] = readSlot(f, l, i);
        }
        return scene;
    }
    if (buttonAt(f, l, 80, 0xFF, 0x8C, 0x1A)) {
        scene.kind = ScreenKind::GameOverFinal;       // PLAY AGAIN
    } else if (buttonAt(f, l, -330, 0x3C, 0xC8, 0x3C)) {
        scene.kind = ScreenKind::GameOverOffer;       // CONTINUE (rewarded ad) + countdown
    } else if (buttonAt(f, l, 40, 0x3C, 0xC8, 0x3C)) {
        scene.kind = ScreenKind::Menu;                // CLASSIC
    }
    return scene;
}

QString describe(const YuvView &f, const Layout &l)
{
    auto rgb = [&](const QPointF &p) {
        const YuvView::Rgb c = f.average(p, 1, 2.0 * l.scale);
        return QStringLiteral("%1,%2,%3").arg(c.r).arg(c.g).arg(c.b);
    };
    return QStringLiteral("frame %1x%2 scale %3; cell00 %4; cell33 %5; cell77 %6; frameL %7; slot1 %8; btn80 %9; btn-330 %10; btn40 %11")
        .arg(f.width).arg(f.height).arg(l.scale, 0, 'f', 3)
        .arg(rgb(l.cellCenter(0, 0)), rgb(l.cellCenter(3, 3)), rgb(l.cellCenter(7, 7)), rgb(l.canvas(kBoardLeft - 10, 150)),
             rgb(l.slotCenter(1)), rgb(l.canvas(-280, 80)), rgb(l.canvas(-280, -330)), rgb(l.canvas(-280, 40)));
}

Mask place(Mask board, Mask piece, int *lines)
{
    board |= piece;
    Mask clear = 0;
    int count = 0;
    for (int i = 0; i < kSize; ++i) {
        const Mask row = Mask(0xFF) << (8 * i);
        const Mask col = kCol0 << i;
        if ((board & row) == row) {
            clear |= row;
            ++count;
        }
        if ((board & col) == col) {
            clear |= col;
            ++count;
        }
    }
    if (lines) {
        *lines = count;
    }
    return board & ~clear;
}

Move bestMove(Mask board, const Hand &hand)
{
    Search search;
    search.hand = &hand;
    search.run(board, 0, 0, 0.0, Move());
    return search.best;
}

} // namespace bp
} // namespace farm
