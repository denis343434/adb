#include <QtTest>

#include <vector>

#include "automation/blockpuzzle.h"

using namespace farm::bp;

namespace {

/// A synthetic I420 frame painted in RGB (converted with the same BT.601 limited range the decoder delivers).
struct Frame
{
    int w, h;
    std::vector<uint8_t> y, u, v;

    Frame(int width, int height, int r, int g, int b) : w(width), h(height), y(size_t(width) * height), u(size_t(width / 2) * (height / 2)), v(u.size())
    {
        fill(QRectF(0, 0, w, h), r, g, b);
    }

    void fill(const QRectF &rect, int r, int g, int b)
    {
        const int yy = qBound(0, ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16, 255);
        const int uu = qBound(0, ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128, 255);
        const int vv = qBound(0, ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128, 255);
        const QRect px = rect.toAlignedRect() & QRect(0, 0, w, h);
        for (int j = px.top(); j <= px.bottom(); ++j) {
            for (int i = px.left(); i <= px.right(); ++i) {
                y[size_t(j) * w + i] = uint8_t(yy);
                u[size_t(j / 2) * (w / 2) + i / 2] = uint8_t(uu);
                v[size_t(j / 2) * (w / 2) + i / 2] = uint8_t(vv);
            }
        }
    }

    YuvView view() const
    {
        YuvView f;
        f.width = w;
        f.height = h;
        f.y = y.data();
        f.u = u.data();
        f.v = v.data();
        f.strideY = w;
        f.strideU = w / 2;
        f.strideV = w / 2;
        return f;
    }
};

Shape shape(std::initializer_list<QPoint> cells) { return Shape::fromCells(QVector<QPoint>(cells)); }

/// Paints the game screen: background, board cells (filled ones orange) and the hand pieces.
Frame paintGame(const Layout &l, Mask board, const Hand &hand)
{
    Frame f(l.frame.width(), l.frame.height(), 0x1B, 0x21, 0x30);
    const QPointF frameCentre = l.canvas(0, 150);
    f.fill(QRectF(frameCentre.x() - 460 * l.scale, frameCentre.y() - 460 * l.scale, 920 * l.scale, 920 * l.scale), 0x12, 0x17, 0x22);
    const double cell = 103 * l.scale;
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            const QPointF c = l.cellCenter(x, y);
            const bool filled = board & bit(x, y);
            f.fill(QRectF(c.x() - cell / 2, c.y() - cell / 2, cell, cell), filled ? 255 : 0x2A, filled ? 140 : 0x33, filled ? 35 : 0x48);
        }
    }
    const double pitch = 52 * l.scale;
    const double size = 52 * 0.92 * l.scale;
    for (int s = 0; s < 3; ++s) {
        const Shape &piece = hand[size_t(s)];
        if (piece.isEmpty()) {
            continue;
        }
        const QPointF centre = l.slotCenter(s);
        const QPointF origin = centre - QPointF(piece.width * pitch / 2, piece.height * pitch / 2);
        for (const QPoint &c : piece.cells) {
            f.fill(QRectF(origin.x() + c.x() * pitch + (pitch - size) / 2, origin.y() + c.y() * pitch + (pitch - size) / 2, size, size), 31, 187, 225);
        }
    }
    return f;
}

} // namespace

class TstBlockPuzzle : public QObject
{
    Q_OBJECT
private slots:
    void placeClearsRowsAndColumns()
    {
        Mask board = 0;
        for (int x = 0; x < 7; ++x) board |= bit(x, 3);
        for (int y = 0; y < 7; ++y) board |= bit(7, y);
        int lines = 0;
        const Mask after = place(board, bit(7, 3) | bit(7, 7), &lines);
        QCOMPARE(lines, 2);
        QCOMPARE(after, Mask(0));
    }

    void shapeIsNormalised()
    {
        const Shape s = shape({QPoint(3, 2), QPoint(2, 2), QPoint(2, 3)});
        QCOMPARE(s.width, 2);
        QCOMPARE(s.height, 2);
        QCOMPARE(s.maskAt(0, 0), bit(0, 0) | bit(1, 0) | bit(0, 1));
    }

    void bestMoveCompletesALine()
    {
        Mask board = 0;
        for (int x = 0; x < 6; ++x) board |= bit(x, 7);
        Hand hand;
        hand[1] = shape({QPoint(0, 0), QPoint(1, 0)});    // the 1x2 finishes the bottom row
        const Move m = bestMove(board, hand);
        QVERIFY(m.isValid());
        QCOMPARE(m.slot, 1);
        QCOMPARE(m.x, 6);
        QCOMPARE(m.y, 7);
        QCOMPARE(m.after, Mask(0));
    }

    void bestMoveNoneWhenNothingFits()
    {
        Hand hand;
        hand[0] = shape({QPoint(0, 0), QPoint(1, 0), QPoint(0, 1), QPoint(1, 1)});
        Mask board = ~Mask(0);
        board &= ~bit(0, 0);    // a single hole
        QVERIFY(!bestMove(board, hand).isValid());
    }

    void layoutMatchesThePhone()
    {
        // 720x1600 phone: board cells span x 67..653, first row starts at y 407; hand slots at y 1383.
        const Layout l = Layout::forFrame(QSize(720, 1600));
        QCOMPARE(qRound(l.cellCenter(0, 0).x()), 101);
        QCOMPARE(qRound(l.cellCenter(0, 0).y()), 441);
        QCOMPARE(qRound(l.cellCenter(7, 7).x()), 619);
        QCOMPARE(qRound(l.slotCenter(0).x()), 140);
        QCOMPARE(qRound(l.slotCenter(2).x()), 580);
        QCOMPARE(qRound(l.slotCenter(1).y()), 1383);
        // A single cell dropped at (0, 0) is centred on that cell.
        const QPointF drop = l.dropCenter(shape({QPoint(0, 0)}), 0, 0);
        QVERIFY(qAbs(drop.x() - l.cellCenter(0, 0).x()) < 0.01);
        QVERIFY(qAbs(drop.y() - l.cellCenter(0, 0).y()) < 0.01);
    }

    void analyzeReadsBoardAndHand_data()
    {
        QTest::addColumn<QSize>("frame");
        QTest::newRow("720x1600") << QSize(720, 1600);
        QTest::newRow("1080x2400") << QSize(1080, 2400);
        QTest::newRow("1080x1920") << QSize(1080, 1920);
        QTest::newRow("tablet 1200x1920") << QSize(1200, 1920);
    }

    void analyzeReadsBoardAndHand()
    {
        QFETCH(QSize, frame);
        const Layout l = Layout::forFrame(frame);
        const Mask board = bit(0, 0) | bit(3, 0) | bit(7, 7) | bit(4, 5) | bit(5, 5);
        Hand hand;
        hand[0] = shape({QPoint(0, 0), QPoint(1, 0)});
        hand[1] = shape({QPoint(0, 0), QPoint(0, 1), QPoint(0, 2), QPoint(0, 3), QPoint(0, 4)});
        hand[2] = shape({QPoint(0, 0), QPoint(1, 0), QPoint(2, 0), QPoint(0, 1), QPoint(1, 1), QPoint(2, 1), QPoint(0, 2), QPoint(1, 2), QPoint(2, 2)});
        const Frame f = paintGame(l, board, hand);
        const Scene scene = analyze(f.view(), l);
        QCOMPARE(int(scene.kind), int(ScreenKind::Playing));
        QCOMPARE(scene.board, board);
        QVERIFY(scene.hand == hand);
    }

    void analyzeEmptySlotAndLShape()
    {
        const Layout l = Layout::forFrame(QSize(720, 1600));
        Hand hand;
        hand[2] = shape({QPoint(0, 0), QPoint(0, 1), QPoint(1, 1), QPoint(2, 1)});
        const Frame f = paintGame(l, 0, hand);
        const Scene scene = analyze(f.view(), l);
        QCOMPARE(int(scene.kind), int(ScreenKind::Playing));
        QVERIFY(scene.hand[0].isEmpty());
        QVERIFY(scene.hand[1].isEmpty());
        QVERIFY(scene.hand[2] == hand[2]);
    }

    void analyzeGameOverScreens()
    {
        const Layout l = Layout::forFrame(QSize(720, 1600));
        auto button = [&](Frame &f, double cy, int r, int g, int b) {
            const QPointF c = l.canvas(0, cy);
            f.fill(QRectF(c.x() - 320 * l.scale, c.y() - 85 * l.scale, 640 * l.scale, 170 * l.scale), r, g, b);
        };
        Frame offer(720, 1600, 10, 13, 20);    // dark overlay over the board
        button(offer, -330, 0x3C, 0xC8, 0x3C);
        QCOMPARE(int(analyze(offer.view(), l).kind), int(ScreenKind::GameOverOffer));

        Frame final(720, 1600, 10, 13, 20);
        button(final, 80, 0xFF, 0x8C, 0x1A);
        QCOMPARE(int(analyze(final.view(), l).kind), int(ScreenKind::GameOverFinal));

        Frame menu(720, 1600, 0x1B, 0x21, 0x30);
        button(menu, 40, 0x3C, 0xC8, 0x3C);
        QCOMPARE(int(analyze(menu.view(), l).kind), int(ScreenKind::Menu));

        Frame other(720, 1600, 240, 240, 240);
        QCOMPARE(int(analyze(other.view(), l).kind), int(ScreenKind::Unknown));
    }
};

QTEST_GUILESS_MAIN(TstBlockPuzzle)
#include "tst_blockpuzzle.moc"
