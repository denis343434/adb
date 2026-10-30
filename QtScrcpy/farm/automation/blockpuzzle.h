#ifndef FARM_AUTOMATION_BLOCKPUZZLE_H
#define FARM_AUTOMATION_BLOCKPUZZLE_H

#include <QImage>
#include <QPoint>
#include <QString>
#include <QStringList>
#include <QVector>

namespace farm {

/**
 * One polyomino, row-major, origin at the top-left occupied bounding box.
 * `cells[r * cols + c]` is 1 when that square belongs to the piece.
 */
struct PuzzleShape
{
    int rows = 0;
    int cols = 0;
    QVector<quint8> cells;

    bool at(int r, int c) const;
    int count() const;
};

/// A piece sitting in the tray under the board, in screenshot pixels.
struct PuzzlePiece
{
    PuzzleShape shape;
    double pitch = 0;     // pixels between block centers
    double originX = 0;   // screenshot x of the center of shape cell (0,0)
    double originY = 0;
};

struct PuzzleScene
{
    bool boardFound = false;
    int grid = 0;                         // 8 or 10
    QVector<quint8> filled;               // grid * grid, row-major, 1 = occupied
    QVector<double> colCenter;            // screenshot x of each column center
    QVector<double> rowCenter;            // screenshot y of each row center
    QVector<PuzzlePiece> pieces;
    bool hasButton = false;               // a big green menu button (Play / Claim / Resume)
    QPoint button;
    QString note;
};

struct PuzzleMove
{
    int piece = -1;
    int row = 0;
    int col = 0;                          // top-left of the shape's bounding box
    int lines = 0;
    int score = 0;
    bool ok() const { return piece >= 0; }
};

struct PuzzleDrag
{
    QPoint from;
    QPoint to;
    bool ok = false;
};

/**
 * Reads a Block Puzzle / 1010-style screen and picks a placement.
 * The reader is tuned for the wood-free "Block Puzzle" Unity game
 * (package com.BlockPuzzle.BlockPuzzle): an 8×8 blue grid, three trays
 * of pieces underneath, and green menu buttons (Classic, Claim, Resume).
 * Pure functions — no ADB — so the unit tests can feed screenshots.
 */
class BlockPuzzle
{
public:
    /// Prefer a known Block Puzzle package, else the best block/puzzle name match.
    static QString pickPackage(const QStringList &packages);

    static PuzzleScene read(const QImage &screen);
    /// `skip` placements are ignored (the previous drag did not land).
    static PuzzleMove choose(const PuzzleScene &scene, const QVector<PuzzleMove> &skip = {});
    static PuzzleDrag dragFor(const PuzzleScene &scene, const PuzzleMove &move);

    static QString dump(const PuzzleScene &scene);
};

} // namespace farm

#endif // FARM_AUTOMATION_BLOCKPUZZLE_H
