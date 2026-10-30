#include <algorithm>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QtTest>

#include "automation/blockpuzzle.h"

using namespace farm;

namespace {

QString fixture(const QString &name)
{
    QDir dir(QCoreApplication::applicationDirPath());
    for (int i = 0; i < 8; ++i) {
        const QString path = dir.filePath(QStringLiteral("tests/fixtures/") + name);
        if (QFileInfo::exists(path)) {
            return path;
        }
        if (!dir.cdUp()) {
            break;
        }
    }
    return QString();
}

int filledCount(const PuzzleScene &scene)
{
    int n = 0;
    for (quint8 cell : scene.filled) {
        n += cell ? 1 : 0;
    }
    return n;
}

bool sameShape(const PuzzleShape &shape, const char *rows)
{
    const QStringList lines = QString::fromLatin1(rows).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    if (lines.size() != shape.rows) {
        return false;
    }
    for (int r = 0; r < lines.size(); ++r) {
        if (lines.at(r).size() != shape.cols) {
            return false;
        }
        for (int c = 0; c < shape.cols; ++c) {
            const bool on = lines.at(r).at(c) == QLatin1Char('#');
            if (on != shape.at(r, c)) {
                return false;
            }
        }
    }
    return true;
}

} // namespace

class TestBlockPuzzle : public QObject
{
    Q_OBJECT
private slots:
    void picksInstalledPackage()
    {
        const QStringList packages = {
            QStringLiteral("package:com.android.settings"),
            QStringLiteral("package:com.BlockPuzzle.BlockPuzzle"),
            QStringLiteral("package:com.block.juggle"),
        };
        QCOMPARE(BlockPuzzle::pickPackage(packages), QStringLiteral("com.BlockPuzzle.BlockPuzzle"));
        QCOMPARE(BlockPuzzle::pickPackage({ QStringLiteral("package:com.example.block") }), QString());
        QCOMPARE(BlockPuzzle::pickPackage({ QStringLiteral("com.games.block.puzzle.mania") }),
                 QStringLiteral("com.games.block.puzzle.mania"));
        QVERIFY(BlockPuzzle::pickPackage({ QStringLiteral("package:com.android.chrome") }).isEmpty());
    }

    void readsPhoneBoard()
    {
        const QImage image(fixture(QStringLiteral("blockpuzzle-board.png")));
        QVERIFY2(!image.isNull(), "missing tests/fixtures/blockpuzzle-board.png");
        const PuzzleScene scene = BlockPuzzle::read(image);
        QVERIFY2(scene.boardFound, qPrintable(BlockPuzzle::dump(scene)));
        QCOMPARE(scene.grid, 8);
        QCOMPARE(filledCount(scene), 3);
        int row = -1;
        int run = 0;
        int best = 0;
        for (int r = 0; r < 8; ++r) {
            int count = 0;
            for (int c = 0; c < 8; ++c) {
                count += scene.filled.at(r * 8 + c) ? 1 : 0;
            }
            if (count == 3) {
                row = r;
            }
            best = std::max(best, count);
        }
        QCOMPARE(best, 3);
        QVERIFY(row >= 0);
        run = 0;
        int streak = 0;
        for (int c = 0; c < 8; ++c) {
            if (scene.filled.at(row * 8 + c)) {
                ++streak;
                run = std::max(run, streak);
            } else {
                streak = 0;
            }
        }
        QCOMPARE(run, 3);
        QCOMPARE(scene.pieces.size(), 2);
        bool square = false;
        bool tee = false;
        for (const PuzzlePiece &piece : scene.pieces) {
            square = square || sameShape(piece.shape, "##\n##");
            tee = tee || sameShape(piece.shape, "###\n.#.");
            QVERIFY(piece.pitch > 20.0);
            QVERIFY(piece.originY > scene.rowCenter.last());
        }
        QVERIFY2(square, qPrintable(BlockPuzzle::dump(scene)));
        QVERIFY2(tee, qPrintable(BlockPuzzle::dump(scene)));
        QVERIFY(!scene.hasButton);
    }

    void findsClassicButton()
    {
        const QImage image(fixture(QStringLiteral("blockpuzzle-menu.png")));
        QVERIFY(!image.isNull());
        const PuzzleScene scene = BlockPuzzle::read(image);
        QVERIFY2(!scene.boardFound, qPrintable(BlockPuzzle::dump(scene)));
        QVERIFY2(scene.hasButton, qPrintable(BlockPuzzle::dump(scene)));
        QVERIFY(scene.button.x() > 200 && scene.button.x() < 520);
        QVERIFY(scene.button.y() > 600 && scene.button.y() < 950);
    }

    void prefersTheClear()
    {
        PuzzleScene scene;
        scene.boardFound = true;
        scene.grid = 8;
        scene.filled = QVector<quint8>(64, 0);
        for (int c = 0; c < 5; ++c) {
            scene.filled[c] = 1;
        }
        PuzzlePiece bar;
        bar.shape.rows = 1;
        bar.shape.cols = 3;
        bar.shape.cells = QVector<quint8>({ 1, 1, 1 });
        PuzzlePiece dot;
        dot.shape.rows = 1;
        dot.shape.cols = 1;
        dot.shape.cells = QVector<quint8>({ 1 });
        scene.pieces = { bar, dot };
        const PuzzleMove move = BlockPuzzle::choose(scene);
        QVERIFY(move.ok());
        QCOMPARE(move.piece, 0);
        QCOMPARE(move.row, 0);
        QCOMPARE(move.col, 5);
        QVERIFY(move.lines >= 1);
    }

    void reportsNoMove()
    {
        PuzzleScene scene;
        scene.boardFound = true;
        scene.grid = 8;
        scene.filled = QVector<quint8>(64, 1);
        scene.filled[63] = 0;
        PuzzlePiece square;
        square.shape.rows = 2;
        square.shape.cols = 2;
        square.shape.cells = QVector<quint8>({ 1, 1, 1, 1 });
        scene.pieces = { square };
        QVERIFY(!BlockPuzzle::choose(scene).ok());
    }

    void dragAimsAtTheCells()
    {
        PuzzleScene scene;
        scene.boardFound = true;
        scene.grid = 8;
        scene.filled = QVector<quint8>(64, 0);
        for (int i = 0; i < 8; ++i) {
            scene.colCenter.append(100.0 + i * 70.0);
            scene.rowCenter.append(400.0 + i * 70.0);
        }
        PuzzlePiece bar;
        bar.shape.rows = 1;
        bar.shape.cols = 3;
        bar.shape.cells = QVector<quint8>({ 1, 1, 1 });
        bar.pitch = 30;
        bar.originX = 50;
        bar.originY = 1400;
        scene.pieces = { bar };
        PuzzleMove move;
        move.piece = 0;
        move.row = 0;
        move.col = 1;
        const PuzzleDrag drag = BlockPuzzle::dragFor(scene, move);
        QVERIFY(drag.ok);
        QCOMPARE(drag.from, QPoint(80, 1400));
        QCOMPARE(drag.to, QPoint(240, 400));
    }
};

QTEST_GUILESS_MAIN(TestBlockPuzzle)
#include "tst_blockpuzzle.moc"
