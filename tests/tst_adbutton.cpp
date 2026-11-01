#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QImage>
#include <QPainter>
#include <QtTest>

#include "automation/adbuttonfinder.h"

using namespace farm;

namespace {

/// Walks up from the test binary to the repository root and returns `relative`.
QString findUp(const QString &relative)
{
    QDir dir(QCoreApplication::applicationDirPath());
    for (int i = 0; i < 8; ++i) {
        const QString path = dir.filePath(relative);
        if (QFileInfo::exists(path)) {
            return path;
        }
        if (!dir.cdUp()) {
            break;
        }
    }
    return QString();
}

QString fixture(const QString &name)
{
    return findUp(QStringLiteral("tests/fixtures/") + name);
}

QString crops()
{
    return findUp(QStringLiteral("QtScrcpy/farm/automation/skip_ads/buttons"));
}

QImage darkCanvas()
{
    QImage canvas(720, 1600, QImage::Format_RGB32);
    canvas.fill(QColor(10, 10, 10));
    return canvas;
}

} // namespace

class TestAdButton : public QObject
{
    Q_OBJECT
private slots:
    void loadsButtonCrops()
    {
        const QString dir = crops();
        QVERIFY2(!dir.isEmpty(), "skip_ads/buttons not found");
        const int count = AdButtonFinder::instance().loadFromDirectory(dir);
        // Every crop with a flat background around the button is used;
        // skip_dark.png is a green photo fragment and must be dropped, otherwise
        // it matches the game board.
        QCOMPARE(count, 7);
    }

    void findsSkipOnRealAd()
    {
        QVERIFY(AdButtonFinder::instance().loadFromDirectory(crops()) > 0);
        const QImage ad(fixture(QStringLiteral("ad-skip-720.png")));
        QVERIFY2(!ad.isNull(), "missing tests/fixtures/ad-skip-720.png");

        QElapsedTimer timer;
        timer.start();
        const AdButtonHit hit = AdButtonFinder::instance().find(ad);
        const qint64 elapsed = timer.elapsed();

        QVERIFY2(hit.ok(), "no Skip button found on the aclinicmed.ru ad screen");
        QVERIFY(!hit.close);
        QVERIFY2(hit.rect.isValid() && hit.rect.contains(hit.point), "the hit box must contain its centre");
        QVERIFY(hit.name.startsWith(QStringLiteral("skip")));
        QVERIFY2(hit.score > 0.85, qPrintable(QStringLiteral("score %1").arg(hit.score)));
        // The ">|" glyph sits in the top-right corner of the creative.
        QVERIFY2(qAbs(hit.point.x() - 670) <= 30, qPrintable(QStringLiteral("x = %1").arg(hit.point.x())));
        QVERIFY2(qAbs(hit.point.y() - 128) <= 30, qPrintable(QStringLiteral("y = %1").arg(hit.point.y())));
        QVERIFY2(elapsed < 2000, qPrintable(QStringLiteral("one screenshot took %1 ms").arg(elapsed)));
    }

    void findsDarkSkipOnSyntheticAd()
    {
        QVERIFY(AdButtonFinder::instance().loadFromDirectory(crops()) > 0);
        const QImage crop(QStringLiteral("%1/skip_next.png").arg(crops()));
        QVERIFY(!crop.isNull());

        QImage canvas = darkCanvas();
        {
            QPainter painter(&canvas);
            painter.drawImage(QPoint(600, 1180), crop);
        }

        const AdButtonHit hit = AdButtonFinder::instance().find(canvas);
        QVERIFY2(hit.ok(), "the white-on-black Skip glyph was not found");
        QVERIFY(!hit.close);
        QVERIFY(hit.name.contains(QStringLiteral("skip_next")));
        QVERIFY2(qAbs(hit.point.x() - (600 + crop.width() / 2)) <= 12,
                 qPrintable(QStringLiteral("x = %1").arg(hit.point.x())));
        QVERIFY2(qAbs(hit.point.y() - (1180 + crop.height() / 2)) <= 12,
                 qPrintable(QStringLiteral("y = %1").arg(hit.point.y())));
    }

    void findsCloseOnLightAd()
    {
        QVERIFY(AdButtonFinder::instance().loadFromDirectory(crops()) > 0);
        const QImage crop(QStringLiteral("%1/close.png").arg(crops()));
        QVERIFY(!crop.isNull());

        QImage canvas(720, 1600, QImage::Format_RGB32);
        canvas.fill(QColor(246, 246, 246));
        {
            QPainter painter(&canvas);
            painter.drawImage(QPoint(620, 700), crop);
        }

        const AdButtonHit hit = AdButtonFinder::instance().find(canvas);
        QVERIFY2(hit.ok(), "the Close cross was not found");
        QVERIFY(hit.close);
        QVERIFY(hit.name.startsWith(QStringLiteral("close")));
    }

    void findsDarkCircleClose()
    {
        // The dark round "x" badge that some ads put in their top-right corner.
        QVERIFY(AdButtonFinder::instance().loadFromDirectory(crops()) > 0);
        const QImage crop(QStringLiteral("%1/close_circle_dark.png").arg(crops()));
        QVERIFY(!crop.isNull());

        for (const QColor &background : { QColor(170, 170, 170), QColor(250, 250, 250) }) {
            QImage canvas(720, 1600, QImage::Format_RGB32);
            canvas.fill(background);
            {
                QPainter painter(&canvas);
                painter.drawImage(QPoint(640, 110), crop);
            }
            const AdButtonHit hit = AdButtonFinder::instance().find(canvas);
            QVERIFY2(hit.ok(), qPrintable(QStringLiteral("the round Close badge was not found on %1").arg(background.name())));
            QVERIFY(hit.close);
            QVERIFY(hit.name.startsWith(QStringLiteral("close")));
            QVERIFY2(qAbs(hit.point.x() - (640 + crop.width() / 2)) <= 14,
                     qPrintable(QStringLiteral("x = %1").arg(hit.point.x())));
            QVERIFY2(qAbs(hit.point.y() - (110 + crop.height() / 2)) <= 14,
                     qPrintable(QStringLiteral("y = %1").arg(hit.point.y())));
        }
    }

    void findsNavyCircleClose()
    {
        // The same round badge on a dark blue ad: the light-background crop of the
        // badge does not match here, so this theme has its own crop.
        QVERIFY(AdButtonFinder::instance().loadFromDirectory(crops()) > 0);
        const QImage crop(QStringLiteral("%1/close_circle_navy.png").arg(crops()));
        QVERIFY(!crop.isNull());

        QImage canvas(720, 1600, QImage::Format_RGB32);
        canvas.fill(QColor(68, 69, 118));
        {
            QPainter painter(&canvas);
            painter.drawImage(QPoint(650, 110), crop);
        }

        const AdButtonHit hit = AdButtonFinder::instance().find(canvas);
        QVERIFY2(hit.ok(), "the round Close badge on the dark blue ad was not found");
        QVERIFY(hit.close);
        QVERIFY2(qAbs(hit.point.x() - 663) <= 14, qPrintable(QStringLiteral("x = %1").arg(hit.point.x())));
        QVERIFY2(qAbs(hit.point.y() - 122) <= 14, qPrintable(QStringLiteral("y = %1").arg(hit.point.y())));
    }

    void sweepFolder()
    {
        // Developer aid: point FARM_SHOTS at a folder of phone screenshots and run
        // `tst_adbutton sweepFolder` to see what the crops match on real screens.
        const QString root = qEnvironmentVariable("FARM_SHOTS");
        if (root.isEmpty()) {
            QSKIP("FARM_SHOTS is not set");
        }
        QVERIFY(AdButtonFinder::instance().loadFromDirectory(crops()) > 0);
        QDir dir(root);
        const QStringList files = dir.entryList(QStringList{ QStringLiteral("*.png") }, QDir::Files, QDir::Name);
        int hits = 0;
        for (const QString &name : files) {
            if (name.contains(QStringLiteral(".marked")) || name.contains(QStringLiteral("crop"))) {
                continue;
            }
            const QImage shot(dir.filePath(name));
            if (shot.isNull()) {
                continue;
            }
            const AdButtonHit hit = AdButtonFinder::instance().find(shot);
            if (hit.ok()) {
                ++hits;
                qInfo("%s -> %s %.3f at %d,%d", qPrintable(name), qPrintable(hit.name), hit.score, hit.point.x(), hit.point.y());
            }
        }
        qInfo("sweep: %d files, %d hits", int(files.size()), hits);
    }
    void ignoresGameScreen()
    {
        QVERIFY(AdButtonFinder::instance().loadFromDirectory(crops()) > 0);
        const QImage board(fixture(QStringLiteral("ad-board-720.png")));
        QVERIFY2(!board.isNull(), "missing tests/fixtures/ad-board-720.png");
        const AdButtonHit hit = AdButtonFinder::instance().find(board);
        QVERIFY2(!hit.ok(), qPrintable(QStringLiteral("false positive %1 at %2,%3 (%4)")
                                           .arg(hit.name)
                                           .arg(hit.point.x())
                                           .arg(hit.point.y())
                                           .arg(hit.score)));
    }

    void ignoresBlankScreen()
    {
        QVERIFY(AdButtonFinder::instance().loadFromDirectory(crops()) > 0);
        QImage blank(720, 1600, QImage::Format_RGB32);
        blank.fill(QColor(18, 18, 22));
        QVERIFY(!AdButtonFinder::instance().find(blank).ok());
    }
};

QTEST_GUILESS_MAIN(TestAdButton)
#include "tst_adbutton.moc"
