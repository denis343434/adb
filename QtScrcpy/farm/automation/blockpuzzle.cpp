#include "blockpuzzle.h"

#include <algorithm>
#include <cmath>

#include <QHash>
#include <QStringList>

namespace farm {

namespace {

struct Pix
{
    int w = 0;
    int h = 0;
    int scale = 1;
    QVector<int> lum;
    QVector<QRgb> rgb;

    int lumaAt(int x, int y) const { return lum[y * w + x]; }
    QRgb colorAt(int x, int y) const { return rgb[y * w + x]; }
};

int lumaOf(QRgb c)
{
    return (qRed(c) * 30 + qGreen(c) * 59 + qBlue(c) * 11) / 100;
}

int distRgb(QRgb a, QRgb b)
{
    return std::abs(qRed(a) - qRed(b)) + std::abs(qGreen(a) - qGreen(b)) + std::abs(qBlue(a) - qBlue(b));
}

Pix prepare(const QImage &src)
{
    const QImage img = src.convertToFormat(QImage::Format_RGB32);
    int scale = 1;
    while (scale < 6 && img.width() / (scale + 1) >= 340) {
        ++scale;
    }
    Pix p;
    p.scale = scale;
    p.w = std::max(1, img.width() / scale);
    p.h = std::max(1, img.height() / scale);
    p.lum.resize(p.w * p.h);
    p.rgb.resize(p.w * p.h);
    for (int y = 0; y < p.h; ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(img.constScanLine(std::min(img.height() - 1, y * scale)));
        for (int x = 0; x < p.w; ++x) {
            const QRgb c = line[std::min(img.width() - 1, x * scale)];
            p.rgb[y * p.w + x] = c;
            p.lum[y * p.w + x] = lumaOf(c);
        }
    }
    return p;
}

double toFull(double down, int scale)
{
    return down * static_cast<double>(scale) + (scale - 1) * 0.5;
}

int medianOf(QVector<int> values)
{
    if (values.isEmpty()) {
        return 0;
    }
    std::sort(values.begin(), values.end());
    return values.at(values.size() / 2);
}

struct Axis
{
    bool ok = false;
    int n = 0;
    QVector<double> center;
    double pitch = 0;
    int contrast = 0;
};

// Counts the columns (rows) where the luma step is strong instead of summing its
// magnitude: a grid line runs across the whole board, while text and icons only
// cover a few pixels. Summing lets an ad banner or a big title outvote the board;
// counting (or not) each column makes the board's long, regular lines win, which
// is what the period search needs — also on a board that is still empty.
constexpr int kEdgeStep = 14;

QVector<int> rowEnergy(const Pix &p)
{
    QVector<int> energy(p.h, 0);
    const int x0 = p.w / 10;
    const int x1 = p.w - p.w / 10;
    for (int y = 0; y < p.h - 1; ++y) {
        int count = 0;
        for (int x = x0; x < x1; ++x) {
            if (std::abs(p.lumaAt(x, y) - p.lumaAt(x, y + 1)) > kEdgeStep) {
                ++count;
            }
        }
        energy[y] = count;
    }
    return energy;
}

QVector<int> colEnergy(const Pix &p, int y0, int y1)
{
    y0 = std::max(0, y0);
    y1 = std::min(p.h - 1, y1);
    QVector<int> energy(p.w, 0);
    if (y1 <= y0) {
        return energy;
    }
    for (int x = 0; x < p.w - 1; ++x) {
        int count = 0;
        for (int y = y0; y < y1; ++y) {
            if (std::abs(p.lumaAt(x, y) - p.lumaAt(x + 1, y)) > kEdgeStep) {
                ++count;
            }
        }
        energy[x] = count;
    }
    return energy;
}

struct PeriodScore
{
    int period = 0;
    qint64 score = 0;
};

QVector<PeriodScore> rankPeriods(const QVector<int> &energy, int minP, int maxP)
{
    QVector<PeriodScore> ranked;
    minP = std::max(4, minP);
    maxP = std::min(maxP, static_cast<int>(energy.size()) / 3);
    if (minP > maxP) {
        return ranked;
    }
    for (int period = minP; period <= maxP; ++period) {
        qint64 score = 0;
        const int n = static_cast<int>(energy.size()) - period;
        for (int i = 0; i < n; ++i) {
            score += static_cast<qint64>(energy.at(i)) * static_cast<qint64>(energy.at(i + period));
        }
        ranked.append(PeriodScore{ period, score });
    }
    std::sort(ranked.begin(), ranked.end(), [](const PeriodScore &a, const PeriodScore &b) { return a.score > b.score; });
    if (ranked.size() > 5) {
        ranked.resize(5);
    }
    return ranked;
}

int bestPhase(const QVector<int> &energy, int period)
{
    int chosen = 0;
    qint64 best = -1;
    for (int phase = 0; phase < period; ++phase) {
        qint64 sum = 0;
        for (int y = phase; y < energy.size(); y += period) {
            sum += energy.at(y);
        }
        if (sum > best) {
            best = sum;
            chosen = phase;
        }
    }
    return chosen;
}

QVector<int> strongRun(const QVector<int> &energy, int period)
{
    const int phase = bestPhase(energy, period);
    QVector<int> pos;
    for (int y = phase; y < energy.size(); y += period) {
        pos.append(y);
    }
    // A bright piece edge is much stronger than an empty-cell gap, so a threshold
    // against the loudest line drops the rest of the board once a few cells are filled.
    // The grid is the window of eight phase lines with the greatest total energy.
    const int want = 8;
    QVector<int> run;
    if (pos.size() < want) {
        return run;
    }
    int bestA = 0;
    qint64 bestSum = -1;
    for (int a = 0; a + want <= pos.size(); ++a) {
        qint64 sum = 0;
        for (int k = 0; k < want; ++k) {
            sum += energy.at(pos.at(a + k));
        }
        if (sum > bestSum) {
            bestSum = sum;
            bestA = a;
        }
    }
    for (int k = 0; k < want; ++k) {
        run.append(pos.at(bestA + k));
    }
    return run;
}

int probeLuma(const Pix &p, double coord, int probe, bool rows)
{
    const int c = std::clamp(static_cast<int>(std::lround(coord)), 0, (rows ? p.h : p.w) - 1);
    const int q = std::clamp(probe, 0, (rows ? p.w : p.h) - 1);
    return rows ? p.lumaAt(q, c) : p.lumaAt(c, q);
}

Axis interpret(const Pix &p, const QVector<int> &lines, bool rows)
{
    Axis best;
    if (lines.size() < 6) {
        return best;
    }
    QVector<int> diffs;
    for (int i = 1; i < lines.size(); ++i) {
        diffs.append(lines.at(i) - lines.at(i - 1));
    }
    const double pitch = static_cast<double>(medianOf(diffs));
    if (pitch < 4.0) {
        return best;
    }

    auto consider = [&](int n, const QVector<double> &center) {
        if (n != 8 && n != 10) {
            return;
        }
        if (static_cast<int>(center.size()) != n) {
            return;
        }
        const double span = pitch * n;
        // The board is square and almost as wide as the screen, so compare with the short side.
        const int side = std::min(p.w, p.h);
        if (span < side * 0.45 || span > side * 0.98) {
            return;
        }
        const int axisLength = rows ? p.h : p.w;
        if (center.first() < 1.0 || center.last() > axisLength - 2.0) {
            return;
        }
        // A block-puzzle board sits above the piece tray and the banner: its centre
        // is in the upper half of the screen. Without this, the tray pieces and the
        // ad banner below form their own regular lattice and win on contrast.
        const double mid = (center.first() + center.last()) * 0.5;
        if (rows) {
            if (mid < p.h * 0.16 || mid > p.h * 0.62) {
                return;
            }
        } else if (std::abs(mid - p.w * 0.5) > p.w * 0.12) {
            // Boards are horizontally centred; the tray is not.
            return;
        }
        // Rounded cells hide the gap at their middle, and an even grid's midline is
        // itself a gap, so contrast is measured across the axis. A real board also has
        // a dark frame just outside the first and last cell; the mirror alignment
        // (cells on the other side of the same lines) runs off into the page.
        auto framed = [&](double origin, int direction, int at, int cellLuma) {
            const int near = std::max(2, static_cast<int>(std::lround(pitch * 0.25)));
            const int far = std::max(near + 1, static_cast<int>(std::lround(pitch * 0.85)));
            int darkest = 255;
            for (int d = near; d <= far; ++d) {
                darkest = std::min(darkest, probeLuma(p, origin + direction * static_cast<double>(d), at, rows));
            }
            return darkest + 12 < cellLuma;
        };
        const int cross = rows ? p.w : p.h;
        const int step = std::max(2, static_cast<int>(std::lround(pitch / 6.0)));
        const int from = cross / 12;
        const int to = std::max(from + 1, cross - from);
        int contrast = -100000;
        for (int at = from; at < to; at += step) {
            int centerLuma = 0;
            int lineLuma = 0;
            for (int i = 0; i < n; ++i) {
                centerLuma += probeLuma(p, center.at(i), at, rows);
            }
            for (int line : lines) {
                lineLuma += probeLuma(p, static_cast<double>(line), at, rows);
            }
            // A blue-theme cell is brighter than the gaps between cells, but on the
            // dark theme the strongest edges are the bright bottom bevels of the
            // cells, so the cell centres between them are darker than the lines.
            // Either polarity is a real board; the flatness and frame checks below
            // still reject menus and popup grids.
            const int sample = std::abs(centerLuma / n - lineLuma / static_cast<int>(lines.size()));
            if (sample < 8) {
                continue;
            }
            if (!framed(center.first(), -1, at, probeLuma(p, center.first(), at, rows))
                || !framed(center.last(), 1, at, probeLuma(p, center.last(), at, rows))) {
                continue;
            }
            contrast = std::max(contrast, sample);
        }
        if (contrast < 8) {
            return;
        }
        if (!best.ok || contrast > best.contrast) {
            best.ok = true;
            best.n = n;
            best.center = center;
            best.pitch = pitch;
            best.contrast = contrast;
        }
    };

    // Lines are every boundary, including the outer frame: cells sit between them.
    {
        const int n = static_cast<int>(lines.size()) - 1;
        QVector<double> center;
        center.reserve(n);
        for (int i = 0; i < n; ++i) {
            center.append((lines.at(i) + lines.at(i + 1)) * 0.5);
        }
        consider(n, center);
    }
    // Lines are only the gaps between cells: one cell hangs off each end.
    {
        const int n = static_cast<int>(lines.size()) + 1;
        QVector<double> center;
        center.reserve(n);
        const double first = static_cast<double>(lines.first()) - pitch * 0.5;
        for (int i = 0; i < n; ++i) {
            center.append(first + pitch * i);
        }
        consider(n, center);
    }
    // One outer border is missing, so each line is the trailing (or leading) edge of a cell.
    {
        const int n = static_cast<int>(lines.size());
        QVector<double> above;
        QVector<double> below;
        above.reserve(n);
        below.reserve(n);
        for (int line : lines) {
            above.append(line - pitch * 0.5);
            below.append(line + pitch * 0.5);
        }
        consider(n, above);
        consider(n, below);
    }
    return best;
}

Axis fitAxis(const Pix &p, const QVector<int> &energy, bool rows, QString *note, const char *label)
{
    const int side = std::min(p.w, p.h);
    const int minP = std::max(8, side / 20);
    const int maxP = std::max(minP + 1, side / 6);
    Axis best;
    const QVector<PeriodScore> periods = rankPeriods(energy, minP, maxP);
    if (note != nullptr) {
        *note += QStringLiteral("%1 side %2 period %3..%4\n").arg(QString::fromLatin1(label)).arg(side).arg(minP).arg(maxP);
    }
    for (const PeriodScore &candidate : periods) {
        if (candidate.score <= 0) {
            continue;
        }
        const QVector<int> run = strongRun(energy, candidate.period);
        const Axis axis = interpret(p, run, rows);
        if (note != nullptr) {
            *note += QStringLiteral("  p=%1 score=%2 run=%3 n=%4 contrast=%5 pitch=%6\n")
                         .arg(candidate.period)
                         .arg(candidate.score)
                         .arg(run.size())
                         .arg(axis.n)
                         .arg(axis.contrast)
                         .arg(axis.pitch);
        }
        // Take the strongest periodic signal that validates. Picking the highest
        // contrast instead lets a near-miss period (a slightly wrong spacing that
        // still finds eight lines) win on a noisy screen.
        if (axis.ok) {
            best = axis;
            break;
        }
    }
    return best;
}

QRgb averagePatch(const QImage &img, int cx, int cy, int radius)
{
    long red = 0;
    long green = 0;
    long blue = 0;
    int count = 0;
    for (int y = cy - radius; y <= cy + radius; ++y) {
        if (y < 0 || y >= img.height()) {
            continue;
        }
        const QRgb *line = reinterpret_cast<const QRgb *>(img.constScanLine(y));
        for (int x = cx - radius; x <= cx + radius; ++x) {
            if (x < 0 || x >= img.width()) {
                continue;
            }
            const QRgb c = line[x];
            red += qRed(c);
            green += qGreen(c);
            blue += qBlue(c);
            ++count;
        }
    }
    if (count == 0) {
        return qRgb(0, 0, 0);
    }
    return qRgb(static_cast<int>(red / count), static_cast<int>(green / count), static_cast<int>(blue / count));
}

/// Standard deviation of the luma in a square patch — flat blocks stay low,
/// text and icons push it high.
double patchSpread(const QImage &img, int cx, int cy, int radius)
{
    long sum = 0;
    long sumSq = 0;
    int count = 0;
    for (int y = cy - radius; y <= cy + radius; ++y) {
        if (y < 0 || y >= img.height()) {
            continue;
        }
        const QRgb *line = reinterpret_cast<const QRgb *>(img.constScanLine(y));
        for (int x = cx - radius; x <= cx + radius; ++x) {
            if (x < 0 || x >= img.width()) {
                continue;
            }
            const int luma = lumaOf(line[x]);
            sum += luma;
            sumSq += luma * luma;
            ++count;
        }
    }
    if (count == 0) {
        return 0.0;
    }
    const double mean = static_cast<double>(sum) / count;
    return std::sqrt(std::max(0.0, static_cast<double>(sumSq) / count - mean * mean));
}

struct Blob
{
    int minX = 0;
    int minY = 0;
    int maxX = 0;
    int maxY = 0;
    int area = 0;
    long sumX = 0;
    long sumY = 0;

    double cx() const { return area > 0 ? static_cast<double>(sumX) / area : 0.0; }
    double cy() const { return area > 0 ? static_cast<double>(sumY) / area : 0.0; }
    int width() const { return maxX - minX + 1; }
    int height() const { return maxY - minY + 1; }
};

QVector<Blob> components(const QVector<char> &mask, int w, int h)
{
    QVector<char> seen(w * h, 0);
    QVector<Blob> blobs;
    QVector<int> stack;
    for (int start = 0; start < w * h; ++start) {
        if (!mask.at(start) || seen.at(start)) {
            continue;
        }
        Blob blob;
        blob.minX = w;
        blob.minY = h;
        stack.append(start);
        seen[start] = 1;
        while (!stack.isEmpty()) {
            const int index = stack.takeLast();
            const int x = index % w;
            const int y = index / w;
            ++blob.area;
            blob.sumX += x;
            blob.sumY += y;
            blob.minX = std::min(blob.minX, x);
            blob.minY = std::min(blob.minY, y);
            blob.maxX = std::max(blob.maxX, x);
            blob.maxY = std::max(blob.maxY, y);
            const int neighbors[4] = { index - 1, index + 1, index - w, index + w };
            for (int next : neighbors) {
                if (next < 0 || next >= w * h || seen.at(next) || !mask.at(next)) {
                    continue;
                }
                const int nx = next % w;
                const int ny = next / w;
                if (std::abs(nx - x) + std::abs(ny - y) != 1) {
                    continue;
                }
                seen[next] = 1;
                stack.append(next);
            }
        }
        if (blob.area >= 8) {
            blobs.append(blob);
        }
    }
    return blobs;
}

bool isGreen(QRgb c)
{
    const int red = qRed(c);
    const int green = qGreen(c);
    const int blue = qBlue(c);
    return green > 140 && green > red + 35 && green > blue + 35 && red < 190;
}

// ── Game buttons ──────────────────────────────────────────────────────────

// Large green banner button (Play Again / Claim / Resume) — wide and centered.
// Called even while the board is visible (these can overlay the game).
QPoint findGreenButton(const Pix &p)
{
    QVector<char> mask(p.w * p.h, 0);
    for (int i = 0; i < p.rgb.size(); ++i) {
        mask[i] = isGreen(p.rgb.at(i)) ? 1 : 0;
    }
    const QVector<Blob> blobs = components(mask, p.w, p.h);
    const Blob *best = nullptr;
    int bestArea = 0;
    for (const Blob &blob : blobs) {
        // Must span at least 26% of screen width — rules out icons and game pieces.
        if (blob.width() < p.w * 26 / 100) {
            continue;
        }
        if (blob.height() < p.h / 40 || blob.height() > p.h * 14 / 100) {
            continue;
        }
        const double cx = blob.cx();
        const double cy = blob.cy();
        if (std::abs(cx - p.w * 0.5) > p.w * 0.24) {
            continue;
        }
        if (cy < p.h * 0.18 || cy > p.h * 0.86) {
            continue;
        }
        if (blob.area > bestArea) {
            bestArea = blob.area;
            best = &blob;
        }
    }
    if (best == nullptr) {
        return QPoint();
    }
    return QPoint(static_cast<int>(std::lround(toFull(best->cx(), p.scale))),
                  static_cast<int>(std::lround(toFull(best->cy(), p.scale))));
}


QVector<PuzzlePiece> findPieces(const Pix &p, const Axis &rows, const Axis &cols)
{
    QVector<PuzzlePiece> pieces;
    const double pitch = (rows.pitch + cols.pitch) * 0.5;
    const double boardBottom = rows.center.last() + pitch * 0.65;
    // The tray sits well below the grid (about five cells down) and above the ad banner.
    const double regionTop = boardBottom + pitch * 0.15;
    const double regionBottom = std::min(p.h * 0.93, boardBottom + pitch * 6.5);
    if (regionBottom <= regionTop + 4.0) {
        return pieces;
    }
    const int y0 = std::clamp(static_cast<int>(regionTop), 0, p.h - 1);
    const int y1 = std::clamp(static_cast<int>(regionBottom), y0 + 1, p.h - 1);

    int hist[256][3] = {};
    int samples = 0;
    for (int y = y0; y <= y1; y += 2) {
        for (int x = 2; x < p.w - 2; x += 2) {
            const QRgb c = p.colorAt(x, y);
            ++hist[qRed(c)][0];
            ++hist[qGreen(c)][1];
            ++hist[qBlue(c)][2];
            ++samples;
        }
    }
    if (samples == 0) {
        return pieces;
    }
    int channel[3] = {};
    for (int k = 0; k < 3; ++k) {
        int seen = 0;
        const int mid = samples / 2;
        for (int value = 0; value < 256; ++value) {
            seen += hist[value][k];
            if (seen >= mid) {
                channel[k] = value;
                break;
            }
        }
    }
    const QRgb background = qRgb(channel[0], channel[1], channel[2]);

    QVector<char> mask(p.w * p.h, 0);
    for (int y = y0; y <= y1; ++y) {
        for (int x = 1; x < p.w - 1; ++x) {
            if (distRgb(p.colorAt(x, y), background) > 70) {
                mask[y * p.w + x] = 1;
            }
        }
    }
    const int minSide = std::max(4, static_cast<int>(pitch * 0.22));
    const int maxSide = std::max(minSide + 2, static_cast<int>(pitch * 0.8));
    // A tray piece stands on the game background. The permanent ad banner is a
    // bright band across the bottom, and its ">" arrow is exactly one cell: read
    // as a piece, it makes the bot drag from inside the banner, which never picks
    // anything up, and the game looks frozen while every nudge is repeated.
    const auto insideBrightBand = [&](const Blob &blob) {
        const int y = std::clamp(static_cast<int>(std::lround(blob.cy())), 0, p.h - 1);
        QVector<int> row;
        row.reserve(p.w / 2);
        int bright = 0;
        for (int x = 2; x < p.w - 2; x += 2) {
            const int luma = p.lumaAt(x, y);
            row.append(luma);
            bright += luma > 170 ? 1 : 0;
        }
        if (row.isEmpty()) {
            return false;
        }
        return medianOf(row) > 150 && bright * 2 > row.size();
    };
    QVector<Blob> blocks;
    for (const Blob &blob : components(mask, p.w, p.h)) {
        if (blob.width() < minSide || blob.height() < minSide || blob.width() > maxSide || blob.height() > maxSide) {
            continue;
        }
        const double aspect = static_cast<double>(blob.width()) / std::max(1, blob.height());
        if (aspect < 0.55 || aspect > 1.8) {
            continue;
        }
        const double fill = static_cast<double>(blob.area) / (blob.width() * blob.height());
        if (fill < 0.35 || blob.cy() < regionTop || blob.cy() > regionBottom) {
            continue;
        }
        if (insideBrightBand(blob)) {
            continue;
        }
        blocks.append(blob);
    }
    if (blocks.isEmpty()) {
        return pieces;
    }
    std::sort(blocks.begin(), blocks.end(), [](const Blob &a, const Blob &b) { return a.cx() < b.cx(); });
    QVector<int> widths;
    for (const Blob &blob : blocks) {
        widths.append(blob.width());
    }
    const double splitGap = std::max(6.0, medianOf(widths) * 1.35);

    QVector<QVector<Blob>> groups;
    QVector<QRgb> groupColor;
    for (const Blob &blob : blocks) {
        const int bx = std::clamp(static_cast<int>(std::lround(blob.cx())), 0, p.w - 1);
        const int by = std::clamp(static_cast<int>(std::lround(blob.cy())), 0, p.h - 1);
        const QRgb color = p.colorAt(bx, by);
        bool startGroup = groups.isEmpty() || blob.cx() - groups.last().last().cx() > splitGap;
        if (!startGroup && distRgb(color, groupColor.last()) > 60) {
            // Every tray piece has its own colour: a colour change means the next
            // piece starts, even when two pieces sit close together. Grouping by
            // position alone merged neighbours into one bogus shape, and the drag
            // then started between the pieces and never picked one up.
            startGroup = true;
        }
        if (startGroup) {
            groups.append(QVector<Blob>());
            groupColor.append(color);
        }
        groups.last().append(blob);
    }

    for (const QVector<Blob> &group : groups) {
        if (group.isEmpty() || group.size() > 9) {
            continue;
        }
        QVector<double> gaps;
        double minX = group.first().cx();
        double minY = group.first().cy();
        for (const Blob &blob : group) {
            minX = std::min(minX, blob.cx());
            minY = std::min(minY, blob.cy());
        }
        for (int i = 0; i < group.size(); ++i) {
            for (int j = i + 1; j < group.size(); ++j) {
                const double dx = std::abs(group.at(i).cx() - group.at(j).cx());
                const double dy = std::abs(group.at(i).cy() - group.at(j).cy());
                const double d = std::max(dx, dy);
                const double other = std::min(dx, dy);
                if (d > group.at(i).width() * 0.6 && d < group.at(i).width() * 2.4 && other < d * 0.45) {
                    gaps.append(d);
                }
            }
        }
        double blockPitch = gaps.isEmpty() ? static_cast<double>(medianOf(widths)) : medianOf([&]() {
            QVector<int> asInt;
            for (double g : gaps) {
                asInt.append(static_cast<int>(std::lround(g)));
            }
            return asInt;
        }());
        if (blockPitch < 3.0) {
            continue;
        }
        int maxCol = 0;
        int maxRow = 0;
        QVector<QPoint> cells;
        for (const Blob &blob : group) {
            const int col = std::clamp(static_cast<int>(std::lround((blob.cx() - minX) / blockPitch)), 0, 5);
            const int row = std::clamp(static_cast<int>(std::lround((blob.cy() - minY) / blockPitch)), 0, 5);
            maxCol = std::max(maxCol, col);
            maxRow = std::max(maxRow, row);
            cells.append(QPoint(col, row));
        }
        if (maxCol > 4 || maxRow > 4) {
            continue;
        }
        PuzzlePiece piece;
        piece.shape.cols = maxCol + 1;
        piece.shape.rows = maxRow + 1;
        piece.shape.cells = QVector<quint8>(piece.shape.rows * piece.shape.cols, 0);
        for (const QPoint &cell : cells) {
            piece.shape.cells[cell.y() * piece.shape.cols + cell.x()] = 1;
        }
        piece.pitch = blockPitch * p.scale;
        piece.originX = toFull(minX, p.scale);
        piece.originY = toFull(minY, p.scale);
        if (piece.shape.count() >= 1 && piece.shape.count() <= 9) {
            pieces.append(piece);
        }
    }
    return pieces;
}

bool fits(const QVector<quint8> &board, int n, const PuzzleShape &shape, int row, int col)
{
    if (row < 0 || col < 0 || row + shape.rows > n || col + shape.cols > n) {
        return false;
    }
    for (int r = 0; r < shape.rows; ++r) {
        for (int c = 0; c < shape.cols; ++c) {
            if (shape.at(r, c) && board.at((row + r) * n + col + c)) {
                return false;
            }
        }
    }
    return true;
}

int place(QVector<quint8> *board, int n, const PuzzleShape &shape, int row, int col)
{
    for (int r = 0; r < shape.rows; ++r) {
        for (int c = 0; c < shape.cols; ++c) {
            if (shape.at(r, c)) {
                (*board)[(row + r) * n + col + c] = 1;
            }
        }
    }
    QVector<int> fullRows;
    QVector<int> fullCols;
    for (int r = 0; r < n; ++r) {
        bool full = true;
        for (int c = 0; c < n; ++c) {
            if (!board->at(r * n + c)) {
                full = false;
                break;
            }
        }
        if (full) {
            fullRows.append(r);
        }
    }
    for (int c = 0; c < n; ++c) {
        bool full = true;
        for (int r = 0; r < n; ++r) {
            if (!board->at(r * n + c)) {
                full = false;
                break;
            }
        }
        if (full) {
            fullCols.append(c);
        }
    }
    for (int r : fullRows) {
        for (int c = 0; c < n; ++c) {
            (*board)[r * n + c] = 0;
        }
    }
    for (int c : fullCols) {
        for (int r = 0; r < n; ++r) {
            (*board)[r * n + c] = 0;
        }
    }
    return static_cast<int>(fullRows.size() + fullCols.size());
}

PuzzleShape solidShape(int rows, int cols)
{
    PuzzleShape shape;
    shape.rows = rows;
    shape.cols = cols;
    shape.cells = QVector<quint8>(rows * cols, 1);
    return shape;
}

bool shapeFitsAnywhere(const QVector<quint8> &board, int n, const PuzzleShape &shape)
{
    if (shape.rows > n || shape.cols > n) {
        return false;
    }
    for (int r = 0; r <= n - shape.rows; ++r) {
        for (int c = 0; c <= n - shape.cols; ++c) {
            if (fits(board, n, shape, r, c)) {
                return true;
            }
        }
    }
    return false;
}

// Block Puzzle's GreedyBot.Evaluate, classic mode (no gems or stones).
// `board` is the grid after the piece is placed and completed lines are removed.
double greedyValue(const QVector<quint8> &board, int n, int lines)
{
    int filled = 0;
    int holes = 0;
    int edges = 0;
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            const bool here = board.at(y * n + x) != 0;
            filled += here ? 1 : 0;
            if (x + 1 < n && here != (board.at(y * n + x + 1) != 0)) {
                ++edges;
            }
            if (y + 1 < n && here != (board.at((y + 1) * n + x) != 0)) {
                ++edges;
            }
            if (here) {
                continue;
            }
            const bool up = y == 0 || board.at((y - 1) * n + x) != 0;
            const bool down = y + 1 >= n || board.at((y + 1) * n + x) != 0;
            const bool left = x == 0 || board.at(y * n + x - 1) != 0;
            const bool right = x + 1 >= n || board.at(y * n + x + 1) != 0;
            if (up && down && left && right) {
                ++holes;
            }
        }
    }

    double value = lines * 20.0;
    if (filled == 0) {
        value += 100.0;
    }
    value -= holes * 6.0 + edges * 0.8 + filled * 0.3;
    static const PuzzleShape kSquare3 = solidShape(3, 3);
    static const PuzzleShape kLine5H = solidShape(1, 5);
    static const PuzzleShape kLine5V = solidShape(5, 1);
    if (shapeFitsAnywhere(board, n, kSquare3)) {
        value += 8.0;
    }
    if (shapeFitsAnywhere(board, n, kLine5H)) {
        value += 3.0;
    }
    if (shapeFitsAnywhere(board, n, kLine5V)) {
        value += 3.0;
    }
    return value;
}

bool skipped(const PuzzleMove &move, const QVector<PuzzleMove> &skip)
{
    for (const PuzzleMove &old : skip) {
        if (old.piece == move.piece && old.row == move.row && old.col == move.col) {
            return true;
        }
    }
    return false;
}

} // namespace

bool PuzzleShape::at(int r, int c) const
{
    if (r < 0 || c < 0 || r >= rows || c >= cols) {
        return false;
    }
    return cells.at(r * cols + c) != 0;
}

int PuzzleShape::count() const
{
    int n = 0;
    for (quint8 cell : cells) {
        n += cell ? 1 : 0;
    }
    return n;
}

QString BlockPuzzle::pickPackage(const QStringList &packages)
{
    static const char *kKnown[] = {
        "com.BlockPuzzle.BlockPuzzle",
        "com.block.juggle",
        "com.blockpuzzle.comboblastmaster.freegame",
        "com.wood.block.puzzle.game.jewel.gem.blockpuzzle",
        "com.puzzle.block.mania",
        "com.block.puzzle.game.hippo.uc",
        "com.tripledot.woodoku",
        "com.easybrain.block.puzzle.games",
    };
    QStringList names;
    for (QString raw : packages) {
        raw = raw.trimmed();
        if (raw.startsWith(QLatin1String("package:"))) {
            raw = raw.mid(8).trimmed();
        }
        if (!raw.isEmpty()) {
            names.append(raw);
        }
    }
    for (const char *known : kKnown) {
        const QString key = QString::fromLatin1(known);
        if (names.contains(key)) {
            return key;
        }
    }
    QString best;
    int bestScore = 0;
    for (const QString &name : names) {
        const QString lower = name.toLower();
        int score = 0;
        if (lower.contains(QLatin1String("blockpuzzle"))) {
            score += 12;
        }
        if (lower.contains(QLatin1String("block"))) {
            score += 5;
        }
        if (lower.contains(QLatin1String("puzzle"))) {
            score += 5;
        }
        if (lower.contains(QLatin1String("juggle")) || lower.contains(QLatin1String("woodoku"))) {
            score += 6;
        }
        if (score > bestScore) {
            bestScore = score;
            best = name;
        }
    }
    return bestScore >= 8 ? best : QString();
}

PuzzleScene BlockPuzzle::read(const QImage &screen)
{
    PuzzleScene scene;
    if (screen.isNull() || screen.width() < 80 || screen.height() < 80) {
        return scene;
    }
    const QImage full = screen.convertToFormat(QImage::Format_RGB32);
    const Pix pix = prepare(full);

    // Green buttons (Play Again, Claim, Resume) can appear as overlays on top of
    // the game board, so we always check for them.
    const QPoint greenBtn = findGreenButton(pix);
    if (!greenBtn.isNull()) {
        scene.hasButton = true;
        scene.button = greenBtn;
    }

    const Axis rows = fitAxis(pix, rowEnergy(pix), true, &scene.note, "rows");
    if (rows.ok) {
        const int y0 = static_cast<int>(std::floor(rows.center.first() - rows.pitch));
        const int y1 = static_cast<int>(std::ceil(rows.center.last() + rows.pitch));
        const Axis cols = fitAxis(pix, colEnergy(pix, y0, y1), false, &scene.note, "cols");
        if (cols.ok && cols.n == rows.n
            && std::abs(cols.pitch - rows.pitch) <= std::max(cols.pitch, rows.pitch) * 0.06) {
            QVector<double> colCenter;
            QVector<double> rowCenter;
            colCenter.reserve(rows.n);
            rowCenter.reserve(rows.n);
            for (double x : cols.center) {
                colCenter.append(toFull(x, pix.scale));
            }
            for (double y : rows.center) {
                rowCenter.append(toFull(y, pix.scale));
            }

            const int radius = std::max(2, static_cast<int>(std::lround(toFull(rows.pitch, pix.scale) * 0.12)));
            QVector<QRgb> samples(rows.n * rows.n);
            QHash<int, int> buckets;
            QHash<int, QVector<int>> members;
            for (int r = 0; r < rows.n; ++r) {
                for (int c = 0; c < rows.n; ++c) {
                    const int index = r * rows.n + c;
                    samples[index] = averagePatch(full, static_cast<int>(std::lround(colCenter.at(c))),
                                                   static_cast<int>(std::lround(rowCenter.at(r))), radius);
                    const QRgb color = samples.at(index);
                    const int key = ((qRed(color) >> 4) << 8) | ((qGreen(color) >> 4) << 4) | (qBlue(color) >> 4);
                    buckets[key] += 1;
                    members[key].append(index);
                }
            }

            // Board cells are flat blocks. A popup full of cards and text also lines
            // up in rows and columns, but its cells carry icons and labels — the patch
            // spread gives that away, which keeps the reader from "playing" a menu.
            const int flatRadius = std::max(3, static_cast<int>(std::lround(toFull(rows.pitch, pix.scale) * 0.18)));
            QVector<double> spreads;
            spreads.reserve(samples.size());
            for (int i = 0; i < samples.size(); ++i) {
                const int r = i / rows.n;
                const int c = i % rows.n;
                spreads.append(patchSpread(full, static_cast<int>(std::lround(colCenter.at(c))),
                                           static_cast<int>(std::lround(rowCenter.at(r))), flatRadius));
            }
            std::sort(spreads.begin(), spreads.end());
            const double spread75 = spreads.at(spreads.size() * 3 / 4);
            if (spread75 > 15.0) {
                scene.note += QStringLiteral("cells not flat: p75 spread %1\n").arg(spread75);
                return scene;
            }

            scene.boardFound = true;
            scene.grid = rows.n;
            scene.filled = QVector<quint8>(scene.grid * scene.grid, 0);
            scene.colCenter = colCenter;
            scene.rowCenter = rowCenter;

            int emptyKey = 0;
            int emptyCount = -1;
            for (auto it = buckets.cbegin(); it != buckets.cend(); ++it) {
                if (it.value() > emptyCount) {
                    emptyCount = it.value();
                    emptyKey = it.key();
                }
            }
            long red = 0;
            long green = 0;
            long blue = 0;
            const QVector<int> emptyCells = members.value(emptyKey);
            for (int index : emptyCells) {
                red += qRed(samples.at(index));
                green += qGreen(samples.at(index));
                blue += qBlue(samples.at(index));
            }
            const int emptyN = std::max(1, static_cast<int>(emptyCells.size()));
            const QRgb emptyColor = qRgb(static_cast<int>(red / emptyN), static_cast<int>(green / emptyN), static_cast<int>(blue / emptyN));
            for (int i = 0; i < samples.size(); ++i) {
                scene.filled[i] = distRgb(samples.at(i), emptyColor) > 75 ? 1 : 0;
            }

            scene.pieces = findPieces(pix, rows, cols);
            scene.note.clear();
        } // cols ok
    } // rows ok

    return scene;
}

PuzzleMove BlockPuzzle::choose(const PuzzleScene &scene, const QVector<PuzzleMove> &skip)
{
    PuzzleMove best;
    if (!scene.boardFound || scene.grid < 8 || scene.pieces.isEmpty()) {
        return best;
    }
    const int n = scene.grid;
    double bestValue = 0;
    bool have = false;
    for (int piece = 0; piece < scene.pieces.size(); ++piece) {
        const PuzzleShape &shape = scene.pieces.at(piece).shape;
        if (shape.count() <= 0) {
            continue;
        }
        for (int row = 0; row <= n - shape.rows; ++row) {
            for (int col = 0; col <= n - shape.cols; ++col) {
                PuzzleMove move;
                move.piece = piece;
                move.row = row;
                move.col = col;
                if (skipped(move, skip) || !fits(scene.filled, n, shape, row, col)) {
                    continue;
                }
                QVector<quint8> next = scene.filled;
                move.lines = place(&next, n, shape, row, col);
                const double value = greedyValue(next, n, move.lines);
                move.score = static_cast<int>(std::lround(value * 10.0));
                // Strictly less: pick the worst placement so the board fills up faster.
                if (!have || value < bestValue) {
                    have = true;
                    bestValue = value;
                    best = move;
                }
            }
        }
    }
    return best;
}

PuzzleDrag BlockPuzzle::dragFor(const PuzzleScene &scene, const PuzzleMove &move)
{
    PuzzleDrag drag;
    if (!move.ok() || move.piece < 0 || move.piece >= scene.pieces.size() || !scene.boardFound) {
        return drag;
    }
    const PuzzlePiece &piece = scene.pieces.at(move.piece);
    // Grab a real block, not the shape's centre: for shapes whose top-left cell
    // is empty (e.g. "..# / ###") the centre falls into a gap and the touch does
    // not pick the piece up at all. The translation is the same either way.
    double sumC = 0;
    int cells = 0;
    for (int r = 0; r < piece.shape.rows; ++r) {
        for (int c = 0; c < piece.shape.cols; ++c) {
            if (piece.shape.at(r, c)) {
                sumC += c;
                ++cells;
            }
        }
    }
    if (cells == 0) {
        return drag;
    }
    const double midC = sumC / cells;
    // Prefer the topmost block of the piece: the permanent in-game ad banner sits
    // at the bottom of the screen and can cover the lower rows of the tray, and a
    // touch under the banner goes to the ad instead of the piece. The drag vector
    // is unchanged by which block is grabbed, only the gesture moves up.
    int pickR = -1;
    int pickC = -1;
    double pickDistance = 0;
    for (int r = 0; r < piece.shape.rows; ++r) {
        for (int c = 0; c < piece.shape.cols; ++c) {
            if (!piece.shape.at(r, c)) {
                continue;
            }
            if (pickR >= 0 && r != pickR) {
                continue;
            }
            const double distance = std::abs(c - midC);
            if (pickR < 0 || r < pickR || distance < pickDistance) {
                pickR = r;
                pickC = c;
                pickDistance = distance;
            }
        }
    }
    const int boardRow = move.row + pickR;
    const int boardCol = move.col + pickC;
    if (boardRow < 0 || boardCol < 0 || boardRow >= scene.grid || boardCol >= scene.grid) {
        return PuzzleDrag();
    }
    drag.from = QPoint(static_cast<int>(std::lround(piece.originX + pickC * piece.pitch)),
                       static_cast<int>(std::lround(piece.originY + pickR * piece.pitch)));
    drag.to = QPoint(static_cast<int>(std::lround(scene.colCenter.at(boardCol))),
                     static_cast<int>(std::lround(scene.rowCenter.at(boardRow))));
    drag.ok = true;
    return drag;
}

QString BlockPuzzle::dump(const PuzzleScene &scene)
{
    QString text;
    if (!scene.note.isEmpty()) {
        text += scene.note;
    }
    if (!scene.boardFound) {
        text += QStringLiteral("no board");
    } else {
        text = QStringLiteral("grid %1 filled").arg(scene.grid);
        int filled = 0;
        for (quint8 cell : scene.filled) {
            filled += cell ? 1 : 0;
        }
        text += QStringLiteral(" %1\n").arg(filled);
        for (int r = 0; r < scene.grid; ++r) {
            for (int c = 0; c < scene.grid; ++c) {
                text += scene.filled.at(r * scene.grid + c) ? QLatin1Char('#') : QLatin1Char('.');
            }
            text += QLatin1Char('\n');
        }
    }
    for (int i = 0; i < scene.pieces.size(); ++i) {
        const PuzzlePiece &piece = scene.pieces.at(i);
        text += QStringLiteral("piece %1 %2x%3 @ %4,%5\n")
                    .arg(i)
                    .arg(piece.shape.cols)
                    .arg(piece.shape.rows)
                    .arg(static_cast<int>(piece.originX))
                    .arg(static_cast<int>(piece.originY));
        for (int r = 0; r < piece.shape.rows; ++r) {
            for (int c = 0; c < piece.shape.cols; ++c) {
                text += piece.shape.at(r, c) ? QLatin1Char('#') : QLatin1Char('.');
            }
            text += QLatin1Char('\n');
        }
    }
    if (scene.hasButton) {
        text += QStringLiteral("button %1,%2\n").arg(scene.button.x()).arg(scene.button.y());
    }
    return text;
}

} // namespace farm
