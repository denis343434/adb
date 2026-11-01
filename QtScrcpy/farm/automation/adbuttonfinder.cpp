#include "adbuttonfinder.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <vector>

#include <QDir>
#include <QRect>
#include <QStringList>

namespace farm {
namespace {

constexpr int kWorkWidth = 540;      // screenshots are matched at this width
constexpr int kMinSide = 8;          // smallest glyph worth matching, in work pixels
constexpr int kGlyphMargin = 2;      // flat border kept around the trimmed glyph
constexpr int kFlatDelta = 30;       // grey difference a flat background may have
constexpr int kMinGlyphPixels = 12;  // fewer lit pixels than this is noise, not a button

// Skip buttons are generic enough to need a high bar; a Close cross is even
// easier to confuse with game art.
constexpr double kSkipThreshold = 0.85;
constexpr double kCloseThreshold = 0.90;

// Relative to the size of the crop, i.e. roughly 10..50 px of glyph on a phone.
constexpr double kScales[] = { 0.80, 0.95, 1.10, 1.30, 1.50, 1.75, 2.00, 2.30, 2.65, 3.05, 3.50, 4.00 };
constexpr int kScaleCount = static_cast<int>(sizeof(kScales) / sizeof(kScales[0]));

struct Band
{
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;
};

struct GrayImage
{
    int w = 0;
    int h = 0;
    std::vector<float> px;
};

struct Needle
{
    int w = 0;
    int h = 0;
    std::vector<float> centered;
    double norm = 0.0;
};

struct Integral
{
    int w = 0;
    int h = 0;
    std::vector<double> sum;
    std::vector<double> sq;
};

GrayImage toGray(const QImage &image)
{
    const QImage gray = image.convertToFormat(QImage::Format_Grayscale8);
    GrayImage out;
    out.w = gray.width();
    out.h = gray.height();
    out.px.resize(static_cast<std::size_t>(out.w) * static_cast<std::size_t>(out.h));
    for (int y = 0; y < out.h; ++y) {
        const uchar *line = gray.constScanLine(y);
        for (int x = 0; x < out.w; ++x) {
            out.px[static_cast<std::size_t>(y) * static_cast<std::size_t>(out.w) + static_cast<std::size_t>(x)] = line[x];
        }
    }
    return out;
}

Integral integrate(const GrayImage &gray)
{
    Integral out;
    out.w = gray.w;
    out.h = gray.h;
    const int stride = gray.w + 1;
    out.sum.assign(static_cast<std::size_t>(stride) * static_cast<std::size_t>(gray.h + 1), 0.0);
    out.sq.assign(out.sum.size(), 0.0);
    for (int y = 1; y <= gray.h; ++y) {
        double rowSum = 0.0;
        double rowSq = 0.0;
        for (int x = 1; x <= gray.w; ++x) {
            const double value = gray.px[static_cast<std::size_t>(y - 1) * static_cast<std::size_t>(gray.w)
                                         + static_cast<std::size_t>(x - 1)];
            rowSum += value;
            rowSq += value * value;
            const std::size_t at = static_cast<std::size_t>(y) * static_cast<std::size_t>(stride) + static_cast<std::size_t>(x);
            const std::size_t above = static_cast<std::size_t>(y - 1) * static_cast<std::size_t>(stride) + static_cast<std::size_t>(x);
            out.sum[at] = out.sum[above] + rowSum;
            out.sq[at] = out.sq[above] + rowSq;
        }
    }
    return out;
}

double rectSum(const std::vector<double> &table, int stride, int x, int y, int w, int h)
{
    const std::size_t s = static_cast<std::size_t>(stride);
    const std::size_t top = static_cast<std::size_t>(y) * s;
    const std::size_t bottom = static_cast<std::size_t>(y + h) * s;
    const std::size_t left = static_cast<std::size_t>(x);
    const std::size_t right = static_cast<std::size_t>(x + w);
    return table[bottom + right] - table[top + right] - table[bottom + left] + table[top + left];
}

double nccAt(const GrayImage &haystack, const Integral &integral, const Needle &needle, int x, int y)
{
    if (x < 0 || y < 0 || x + needle.w > haystack.w || y + needle.h > haystack.h || needle.norm <= 1e-6) {
        return 0.0;
    }
    const double count = static_cast<double>(needle.w) * static_cast<double>(needle.h);
    const double sum = rectSum(integral.sum, integral.w + 1, x, y, needle.w, needle.h);
    const double sq = rectSum(integral.sq, integral.w + 1, x, y, needle.w, needle.h);
    const double mean = sum / count;
    const double variance = sq - count * mean * mean;
    if (variance <= 1e-6) {
        return 0.0;
    }
    double dot = 0.0;
    for (int row = 0; row < needle.h; ++row) {
        const float *hayRow = &haystack.px[static_cast<std::size_t>(y + row) * static_cast<std::size_t>(haystack.w)
                                          + static_cast<std::size_t>(x)];
        const float *needleRow = &needle.centered[static_cast<std::size_t>(row) * static_cast<std::size_t>(needle.w)];
        for (int col = 0; col < needle.w; ++col) {
            dot += static_cast<double>(hayRow[col]) * needleRow[col];
        }
    }
    return dot / (std::sqrt(variance) * needle.norm);
}

Needle makeNeedle(const QImage &crop, int w, int h)
{
    const GrayImage gray = toGray(crop.scaled(w, h, Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
    Needle needle;
    needle.w = gray.w;
    needle.h = gray.h;
    double sum = 0.0;
    for (float value : gray.px) {
        sum += value;
    }
    const double count = static_cast<double>(gray.px.size());
    const double mean = count > 0.0 ? sum / count : 0.0;
    needle.centered.resize(gray.px.size());
    double squared = 0.0;
    for (std::size_t i = 0; i < gray.px.size(); ++i) {
        const double centered = static_cast<double>(gray.px[i]) - mean;
        needle.centered[i] = static_cast<float>(centered);
        squared += centered * centered;
    }
    needle.norm = std::sqrt(squared);
    return needle;
}

// Keeps the glyph and a little of the flat background around it. A crop whose
// border is not one flat colour is a photo fragment, not a button on ad chrome,
// and is rejected — such a crop matches the game's own art far more often than
// an ad button.
QImage cropGlyph(const QImage &source)
{
    const QImage grayImage = source.convertToFormat(QImage::Format_Grayscale8);
    const int w = grayImage.width();
    const int h = grayImage.height();
    if (w < 6 || h < 6) {
        return QImage();
    }

    std::vector<int> border;
    border.reserve(static_cast<std::size_t>(2 * (w + h)));
    for (int x = 0; x < w; ++x) {
        border.push_back(grayImage.constScanLine(0)[x]);
        border.push_back(grayImage.constScanLine(h - 1)[x]);
    }
    for (int y = 0; y < h; ++y) {
        border.push_back(grayImage.constScanLine(y)[0]);
        border.push_back(grayImage.constScanLine(y)[w - 1]);
    }
    if (border.empty()) {
        return QImage();
    }
    std::nth_element(border.begin(), border.begin() + static_cast<std::ptrdiff_t>(border.size() / 2), border.end());
    const int background = border[border.size() / 2];

    int flat = 0;
    for (int value : border) {
        if (std::abs(value - background) <= kFlatDelta) {
            ++flat;
        }
    }
    if (static_cast<long long>(flat) * 5 < static_cast<long long>(border.size()) * 4) {
        return QImage();
    }

    int minX = w;
    int minY = h;
    int maxX = -1;
    int maxY = -1;
    int lit = 0;
    for (int y = 0; y < h; ++y) {
        const uchar *line = grayImage.constScanLine(y);
        for (int x = 0; x < w; ++x) {
            if (std::abs(static_cast<int>(line[x]) - background) <= kFlatDelta) {
                continue;
            }
            minX = std::min(minX, x);
            maxX = std::max(maxX, x);
            minY = std::min(minY, y);
            maxY = std::max(maxY, y);
            ++lit;
        }
    }
    if (lit < kMinGlyphPixels || maxX < minX || maxY < minY) {
        return QImage();
    }
    const int left = std::max(0, minX - kGlyphMargin);
    const int top = std::max(0, minY - kGlyphMargin);
    const int right = std::min(w, maxX + 1 + kGlyphMargin);
    const int bottom = std::min(h, maxY + 1 + kGlyphMargin);
    return source.copy(QRect(left, top, right - left, bottom - top));
}

QVector<Band> bandsFor(int w, int h)
{
    QVector<Band> bands;
    // Top-right corner: the skip counter and the ">|" / "x" controls.
    bands.append(Band{ w * 68 / 100, 0, w, h * 32 / 100 });
    // Bottom-right strip, from below the board down to just above the permanent
    // in-game banner (bottom 15%): below that the phone's own system overlays
    // (navigation, accessibility bubble) have icons that look like a Close.
    bands.append(Band{ w * 60 / 100, h * 40 / 100, w, h * 85 / 100 });
    return bands;
}

// Fills the hit's centre and its box (in screenshot pixels). The autoplay taps a
// random point inside the box so repeated taps do not land on one pixel.
void setHitBox(AdButtonHit *hit, double centerX, double centerY, int workW, int workH, double factor)
{
    hit->point = QPoint(static_cast<int>(std::lround(centerX)), static_cast<int>(std::lround(centerY)));
    const int boxW = std::max(1, static_cast<int>(std::lround(static_cast<double>(workW) / factor)));
    const int boxH = std::max(1, static_cast<int>(std::lround(static_cast<double>(workH) / factor)));
    hit->rect = QRect(hit->point.x() - boxW / 2, hit->point.y() - boxH / 2, boxW, boxH);
}

} // namespace

AdButtonFinder &AdButtonFinder::instance()
{
    static AdButtonFinder finder;
    return finder;
}

int AdButtonFinder::loadFromDirectory(const QString &dir)
{
    m_templates.clear();
    QDir folder(dir);
    if (!folder.exists()) {
        return 0;
    }
    const QStringList files = folder.entryList(QStringList{ QStringLiteral("*.png"), QStringLiteral("*.jpg"), QStringLiteral("*.jpeg") },
                                               QDir::Files, QDir::Name);
    for (const QString &name : files) {
        const QString lower = name.toLower();
        const bool close = lower.startsWith(QLatin1String("close"));
        if (!close && !lower.startsWith(QLatin1String("skip"))) {
            continue;
        }
        const QImage image(folder.filePath(name));
        if (image.isNull()) {
            continue;
        }
        const QImage cropped = cropGlyph(image);
        if (cropped.isNull()) {
            continue;
        }
        Crop crop;
        crop.name = name;
        crop.image = cropped;
        crop.close = close;
        m_templates.append(crop);
    }
    return m_templates.size();
}

AdButtonHit AdButtonFinder::find(const QImage &screenshot, double *candidateScore, QString *candidateName) const
{
    AdButtonHit best;
    if (m_templates.isEmpty() || screenshot.isNull() || screenshot.width() < 160) {
        return best;
    }

    const double factor = screenshot.width() > kWorkWidth
                              ? static_cast<double>(kWorkWidth) / static_cast<double>(screenshot.width())
                              : 1.0;
    const QImage work = factor < 0.999
                            ? screenshot.scaled(std::max(1, static_cast<int>(std::lround(screenshot.width() * factor))),
                                                std::max(1, static_cast<int>(std::lround(screenshot.height() * factor))),
                                                Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                            : screenshot;

    const GrayImage gray = toGray(work);
    if (gray.w < 64 || gray.h < 64) {
        return best;
    }
    const Integral integral = integrate(gray);
    const QVector<Band> bands = bandsFor(gray.w, gray.h);

    struct Candidate
    {
        double score = -1.0;
        const Crop *crop = nullptr;
        double scale = 1.0;
        int x = 0;
        int y = 0;
    };
    const int templateCount = static_cast<int>(m_templates.size());
    std::vector<Candidate> tops(static_cast<std::size_t>(templateCount));
    Candidate top;

    for (int t = 0; t < templateCount; ++t) {
        const Crop &crop = m_templates.at(t);
        Candidate &cropTop = tops[static_cast<std::size_t>(t)];
        cropTop.crop = &crop;
        const double threshold = crop.close ? kCloseThreshold : kSkipThreshold;
        for (int i = 0; i < kScaleCount; ++i) {
            const double scale = kScales[i] * factor;
            const int tw = static_cast<int>(std::lround(static_cast<double>(crop.image.width()) * scale));
            const int th = static_cast<int>(std::lround(static_cast<double>(crop.image.height()) * scale));
            if (tw < kMinSide || th < kMinSide || tw >= gray.w || th >= gray.h) {
                continue;
            }
            const Needle needle = makeNeedle(crop.image, tw, th);
            if (needle.norm <= 1e-6 || needle.w != tw || needle.h != th) {
                continue;
            }
            for (const Band &band : bands) {
                const int xEnd = std::min(band.x1, gray.w);
                const int yEnd = std::min(band.y1, gray.h);
                if (xEnd - band.x0 < tw || yEnd - band.y0 < th) {
                    continue;
                }
                const int step = std::max(2, std::min(tw, th) / 4);
                int bestX = band.x0;
                int bestY = band.y0;
                double bestScore = -1.0;
                for (int y = band.y0; y + th <= yEnd; y += step) {
                    for (int x = band.x0; x + tw <= xEnd; x += step) {
                        const double score = nccAt(gray, integral, needle, x, y);
                        if (score > bestScore) {
                            bestScore = score;
                            bestX = x;
                            bestY = y;
                        }
                    }
                }
                if (bestScore <= 0.0) {
                    continue;
                }
                const int refineX0 = std::max(band.x0, bestX - step);
                const int refineX1 = std::min(xEnd - tw, bestX + step);
                const int refineY0 = std::max(band.y0, bestY - step);
                const int refineY1 = std::min(yEnd - th, bestY + step);
                for (int y = refineY0; y <= refineY1; ++y) {
                    for (int x = refineX0; x <= refineX1; ++x) {
                        const double score = nccAt(gray, integral, needle, x, y);
                        if (score > bestScore) {
                            bestScore = score;
                            bestX = x;
                            bestY = y;
                        }
                    }
                }
                if (bestScore > cropTop.score) {
                    cropTop.score = bestScore;
                    cropTop.scale = scale;
                    cropTop.x = bestX;
                    cropTop.y = bestY;
                }
                if (bestScore > top.score) {
                    top = cropTop;
                }
                if (bestScore < threshold || bestScore <= best.score) {
                    continue;
                }
                best.score = bestScore;
                best.name = crop.name;
                best.close = crop.close;
                setHitBox(&best, (bestX + tw / 2.0) / factor, (bestY + th / 2.0) / factor, tw, th, factor);
            }
        }
    }

    // The scale ladder jumps by ~15%, and a small glyph is very sensitive to that:
    // landing between two steps can cost 0.1 of the correlation. When nothing
    // passed, every crop's own best candidate is re-matched at finer scales
    // around its size and position before the frame is given up on.
    if (best.score <= 0.0) {
        const double multipliers[] = { 0.84, 0.88, 0.92, 0.96, 1.04, 1.09, 1.14, 1.19 };
        for (const Candidate &candidate : tops) {
            if (candidate.crop == nullptr || candidate.score <= 0.4) {
                continue;
            }
            const double threshold = candidate.crop->close ? kCloseThreshold : kSkipThreshold;
            for (double multiplier : multipliers) {
                const double scale = candidate.scale * multiplier;
                const int tw = static_cast<int>(std::lround(static_cast<double>(candidate.crop->image.width()) * scale));
                const int th = static_cast<int>(std::lround(static_cast<double>(candidate.crop->image.height()) * scale));
                if (tw < kMinSide || th < kMinSide || tw >= gray.w || th >= gray.h) {
                    continue;
                }
                const Needle needle = makeNeedle(candidate.crop->image, tw, th);
                if (needle.norm <= 1e-6) {
                    continue;
                }
                const int radius = 6;
                const int x0 = std::max(0, candidate.x - radius);
                const int x1 = std::min(gray.w - tw, candidate.x + radius);
                const int y0 = std::max(0, candidate.y - radius);
                const int y1 = std::min(gray.h - th, candidate.y + radius);
                for (int y = y0; y <= y1; ++y) {
                    for (int x = x0; x <= x1; ++x) {
                        const double score = nccAt(gray, integral, needle, x, y);
                        if (score > top.score) {
                            top.score = score;
                            top.crop = candidate.crop;
                            top.scale = scale;
                            top.x = x;
                            top.y = y;
                        }
                        if (score < threshold || score <= best.score) {
                            continue;
                        }
                        best.score = score;
                        best.name = candidate.crop->name;
                        best.close = candidate.crop->close;
                        setHitBox(&best, (x + tw / 2.0) / factor, (y + th / 2.0) / factor, tw, th, factor);
                    }
                }
            }
        }
    }

    if (candidateScore != nullptr) {
        *candidateScore = best.ok() ? best.score : std::max(0.0, top.score);
    }
    if (candidateName != nullptr) {
        if (best.ok()) {
            *candidateName = best.name;
        } else if (top.crop != nullptr) {
            *candidateName = top.crop->name;
        } else {
            candidateName->clear();
        }
    }
    return best;
}

} // namespace farm
