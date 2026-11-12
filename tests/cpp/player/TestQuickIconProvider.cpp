#include "TestQuickIconProvider.hpp"

#include <player/quick/PlayerIconProvider.hpp>

#include <QColor>
#include <QImage>
#include <QPointF>
#include <QSize>
#include <QString>
#include <QtGlobal>

#include <cmath>
#include <thread>

using mplayer::quick::PlayerIconProvider;

namespace {

QImage requestIcon(PlayerIconProvider& provider, const QString& id, QSize* reportedSize = nullptr) {
    QSize size;
    const QImage image = provider.requestImage(id, &size, QSize());
    if (reportedSize != nullptr) *reportedSize = size;
    return image;
}

/// Reads the device pixel the provider rasterised under a logical coordinate.
/// The caller picks coordinates well inside a glyph's ink or well outside it, so
/// the answer is not a statement about the antialiased edge pixel.
QColor pixelAt(const QImage& image, qreal logicalX, qreal logicalY) {
    const qreal ratio = image.devicePixelRatio();
    const int x = qBound(0, int(std::floor(logicalX * ratio)), image.width() - 1);
    const int y = qBound(0, int(std::floor(logicalY * ratio)), image.height() - 1);
    return image.pixelColor(x, y);
}

bool isNear(int actual, int expected, int tolerance) {
    return actual >= expected - tolerance && actual <= expected + tolerance;
}

}  // namespace

void TestQuickIconProvider::glyphPixelsKeepTheirLogicalPosition_data() {
    QTest::addColumn<qreal>("ratio");
    QTest::newRow("1x") << 1.0;
    QTest::newRow("1.5x") << 1.5;
    QTest::newRow("5x") << 5.0;
}

void TestQuickIconProvider::glyphPixelsKeepTheirLogicalPosition() {
    QFETCH(qreal, ratio);

    PlayerIconProvider provider;
    QSize reported;
    const QImage image = requestIcon(
        provider,
        QStringLiteral("play?size=16&dpr=%1&color=%23ff5c7a").arg(ratio, 0, 'g', 10),
        &reported);
    QVERIFY2(!image.isNull(), "the provider refused a valid play request");

    const QSize physical(qRound(16.0 * ratio), qRound(16.0 * ratio));
    QCOMPARE(image.size(), physical);
    QCOMPARE(reported, physical);
    QCOMPARE(image.devicePixelRatio(), ratio);
    QCOMPARE(image.format(), QImage::Format_ARGB32_Premultiplied);

    // The play triangle is drawn in a 24-unit grid: vertices at (8,5), (8,19),
    // (19.5,12), scaled to the requested logical size. These samples sit well
    // inside the filled triangle at a 16 px logical size.
    const QColor ink(QStringLiteral("#ff5c7a"));
    const QPointF interior[] = {
        QPointF(6.667, 8.0),
        QPointF(8.667, 7.333),
        QPointF(8.667, 8.667),
    };
    for (const QPointF& at : interior) {
        const QColor pixel = pixelAt(image, at.x(), at.y());
        QCOMPARE(pixel.alpha(), 255);
        QCOMPARE(pixel.rgb(), ink.rgb());
    }

    // Left of the triangle's vertical edge, above it, and beyond its point:
    // the logical canvas is 16 px, so the last sample can still be read.
    const QPointF exterior[] = {
        QPointF(1.333, 1.333),
        QPointF(2.667, 8.0),
        QPointF(14.667, 8.0),
    };
    for (const QPointF& at : exterior) {
        QCOMPARE(pixelAt(image, at.x(), at.y()).alpha(), 0);
    }
}

void TestQuickIconProvider::playAndPausePaintVisiblyDifferentShapes() {
    PlayerIconProvider provider;
    const QImage play =
        requestIcon(provider, QStringLiteral("play?size=24&dpr=1&color=%23ff5c7a"));
    const QImage pause =
        requestIcon(provider, QStringLiteral("pause?size=24&dpr=1&color=%23ff5c7a"));
    QVERIFY(!play.isNull());
    QVERIFY(!pause.isNull());

    // At size 24 the grid is logical pixels. Pixel column 12 lies inside the
    // play triangle and inside the pause gap (bars end at 10.7 and start at
    // 13.3), so the same sample distinguishes the two symbols.
    QCOMPARE(pixelAt(play, 12.0, 12.0).alpha(), 255);
    QCOMPARE(pixelAt(pause, 12.0, 12.0).alpha(), 0);

    // Both bars are solid in the middle, so the pause glyph is not blank: the
    // visible difference is the gap rather than one image being empty.
    QCOMPARE(pixelAt(pause, 8.0, 12.0).alpha(), 255);
}

void TestQuickIconProvider::eachRequestPaintsItsOwnColour() {
    PlayerIconProvider provider;
    const QImage first =
        requestIcon(provider, QStringLiteral("play?size=24&dpr=1&color=%23ff5c7a"));
    const QImage second =
        requestIcon(provider, QStringLiteral("play?size=24&dpr=1&color=%2322cc55"));
    QVERIFY(!first.isNull());
    QVERIFY(!second.isNull());

    const QColor firstInk = pixelAt(first, 12.0, 12.0);
    QCOMPARE(firstInk.alpha(), 255);
    QCOMPARE(firstInk.rgb(), QColor(QStringLiteral("#ff5c7a")).rgb());

    const QColor secondInk = pixelAt(second, 12.0, 12.0);
    QCOMPARE(secondInk.alpha(), 255);
    QCOMPARE(secondInk.rgb(), QColor(QStringLiteral("#22cc55")).rgb());
}

void TestQuickIconProvider::aTranslucentRequestKeepsItsAlphaAndTone() {
    PlayerIconProvider provider;
    const QImage image =
        requestIcon(provider, QStringLiteral("play?size=24&dpr=1&color=%2380ff5c7a"));
    QVERIFY(!image.isNull());

    // Premultiplied storage with full glyph coverage: the alpha is the requested
    // 0x80 and the unpremultiplied colour returns to the requested tone.
    const QColor pixel = pixelAt(image, 12.0, 12.0);
    QCOMPARE(pixel.alpha(), 128);
    QVERIFY2(isNear(pixel.red(), 0xff, 2), qPrintable(pixel.name()));
    QVERIFY2(isNear(pixel.green(), 0x5c, 2), qPrintable(pixel.name()));
    QVERIFY2(isNear(pixel.blue(), 0x7a, 2), qPrintable(pixel.name()));
}

void TestQuickIconProvider::anUnknownSymbolFailsInsteadOfDrawingAnother() {
    PlayerIconProvider provider;
    QSize reported(123, 456);
    const QImage image = provider.requestImage(QStringLiteral("playy"), &reported, QSize());
    QVERIFY(image.isNull());
    QCOMPARE(reported, QSize());
}

void TestQuickIconProvider::anImageThreadRequestPaintsTheSameGlyph() {
    PlayerIconProvider provider;
    // The Quick image loader answers off the GUI thread. Warm the provider's
    // static theme table on the GUI thread first, then let a worker rasterise a
    // request and check the resulting pixels rather than the plumbing.
    QVERIFY(!requestIcon(provider, QStringLiteral("play?size=24&dpr=1&color=%2322cc55")).isNull());

    QImage image;
    QSize reported;
    std::thread worker([&] {
        image = provider.requestImage(
            QStringLiteral("play?size=24&dpr=1&color=%2322cc55"), &reported, QSize());
    });
    worker.join();

    QVERIFY2(!image.isNull(), "the provider refused the request on the image thread");
    QCOMPARE(image.size(), QSize(24, 24));
    QCOMPARE(reported, QSize(24, 24));
    const QColor ink = pixelAt(image, 12.0, 12.0);
    QCOMPARE(ink.alpha(), 255);
    QCOMPARE(ink.rgb(), QColor(QStringLiteral("#22cc55")).rgb());
}
