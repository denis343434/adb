#ifndef FARM_AUTOMATION_ADBUTTONFINDER_H
#define FARM_AUTOMATION_ADBUTTONFINDER_H

#include <QImage>
#include <QPoint>
#include <QRect>
#include <QString>
#include <QVector>

namespace farm {

/// One ad control found on a screenshot.
struct AdButtonHit
{
    QPoint point;         ///< centre of the control, in screenshot pixels
    QRect rect;           ///< box of the control, in screenshot pixels
    double score = 0.0;   ///< normalised cross-correlation; 0 when nothing matched
    QString name;         ///< crop that matched, e.g. "skip_next.png"
    bool close = false;   ///< a Close cross rather than a Skip glyph

    bool ok() const { return score > 0.0 && !point.isNull(); }
};

/**
 * Finds the Skip (">|") and Close ("x") controls of a full-screen ad.
 *
 * Crops live in `skip_ads/buttons` next to the executable and are classified by
 * name (`skip*.png`, `close*.png`). Each crop is trimmed to its glyph; a crop
 * whose "glyph" covers almost the whole picture has no flat ad chrome around
 * it (it is a photo fragment) and is ignored, because such a crop matches the
 * game's own art far more often than an ad button.
 *
 * Matching is normalised cross-correlation at a ladder of scales, restricted to
 * the two corners where ad chrome lives: the top-right corner (the skip counter
 * and its ">|" / "x" control) and the bottom-right strip above the permanent
 * in-game banner (the Close cross of an end card). A match is reported only when
 * the window around it is flat chrome, like the crop itself — a photo fragment,
 * a window frame or a run of text correlates with a small glyph by chance, and a
 * tap on the creative click-throughs to the advertiser's page. The coarse pass
 * walks the zone in steps and the best peak is then refined pixel by pixel, which
 * keeps one screenshot at a few dozen milliseconds on the automation lane.
 */
class AdButtonFinder
{
public:
    static AdButtonFinder &instance();

    /// Reads every usable crop of `dir`; returns how many are now loaded.
    int loadFromDirectory(const QString &dir);
    bool available() const { return !m_templates.isEmpty(); }
    int templateCount() const { return m_templates.size(); }

    /// Best control on the screen; `score` is 0 when nothing matched.
    /// When `candidateScore` / `candidateName` are given they receive the best
    /// match found even if it stayed below the threshold: the autoplay logs that
    /// when an ad's control is not recognised, so a new crop or a threshold
    /// change can be decided from the log alone.
    AdButtonHit find(const QImage &screenshot, double *candidateScore = nullptr, QString *candidateName = nullptr) const;

private:
    struct Crop
    {
        QString name;
        QImage image;
        bool close = false;
    };

    AdButtonFinder() = default;

    QVector<Crop> m_templates;
};

} // namespace farm

#endif // FARM_AUTOMATION_ADBUTTONFINDER_H
