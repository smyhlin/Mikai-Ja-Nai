#include "TestCompareGif.hpp"
#include "CompareGif.hpp"

#include <QFile>
#include <QImageReader>
#include <QPointer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>

using mplayer::shell::saveComparisonGif;

void TestCompareGif::exportAlternatesBothImagesWithTimingAndInfiniteLoop() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("comparison.gif"));
    QImage a(511, 257, QImage::Format_RGB32);
    QImage b(a.size(), a.format());
    uint32_t random = 12345;
    for (int y = 0; y < a.height(); ++y) {
        auto* row = reinterpret_cast<QRgb*>(a.scanLine(y));
        for (int x = 0; x < a.width(); ++x) {
            random = random * 1664525u + 1013904223u;
            row[x] = qRgb(random >> 24, (random >> 16) & 255, (random >> 8) & 255);
        }
    }
    b.fill(Qt::blue);
    std::atomic_bool cancelled{false};
    QString error;
    QVERIFY2(saveComparisonGif(path, a, b, 333, cancelled, &error), qPrintable(error));
    QImageReader reader(path);
    QVERIFY(reader.supportsAnimation());
    QCOMPARE(reader.imageCount(), 2);
    const QImage decoded = reader.read();
    QVERIFY2(!decoded.isNull(), qPrintable(reader.errorString()));
    QCOMPARE(reader.nextImageDelay(), 330);
    QCOMPARE(reader.loopCount(), -1);
    for (const QPoint sample : {QPoint(0, 0), QPoint(255, 128), QPoint(510, 256)}) {
        const QColor source = a.pixelColor(sample);
        const QColor actual = decoded.pixelColor(sample);
        QVERIFY(std::abs(source.red() - actual.red()) <= 36);
        QVERIFY(std::abs(source.green() - actual.green()) <= 36);
        QVERIFY(std::abs(source.blue() - actual.blue()) <= 84);
    }
    const QImage second = reader.read();
    QCOMPARE(second.size(), b.size());
    QCOMPARE(second.pixelColor(255, 128), QColor(Qt::blue));
    QCOMPARE(reader.nextImageDelay(), 330);
    QVERIFY(!reader.canRead());
}

void TestCompareGif::differentlySizedImagesShareOneCanvasWithoutCropping() {
    QTemporaryDir directory;
    const QString path = directory.filePath(QStringLiteral("comparison.gif"));
    QImage a(40, 20, QImage::Format_RGB32);
    QImage b(80, 80, QImage::Format_RGB32);
    a.fill(Qt::red);
    b.fill(Qt::green);
    std::atomic_bool cancelled{false};
    QString error;
    QVERIFY2(saveComparisonGif(path, a, b, 1000, cancelled, &error), qPrintable(error));
    QImageReader reader(path);
    const QImage first = reader.read();
    QCOMPARE(first.size(), QSize(80, 80));
    QCOMPARE(first.pixelColor(40, 40), QColor(Qt::red));
    QCOMPARE(first.pixelColor(40, 5), QColor(Qt::black));
    const QImage second = reader.read();
    QCOMPARE(second.size(), first.size());
    QCOMPARE(second.pixelColor(40, 5), QColor(Qt::green));
}

void TestCompareGif::cancellationPreservesExistingDestination() {
    QTemporaryDir directory;
    const QString path = directory.filePath(QStringLiteral("comparison.gif"));
    QFile existing(path);
    QVERIFY(existing.open(QIODevice::WriteOnly));
    const QByteArray original("previous export");
    QCOMPARE(existing.write(original), original.size());
    existing.close();
    QImage image(64, 64, QImage::Format_RGB32);
    image.fill(Qt::red);
    std::atomic_bool cancelled{true};
    QString error;
    QVERIFY(!saveComparisonGif(path, image, image, 500, cancelled, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(existing.open(QIODevice::ReadOnly));
    QCOMPARE(existing.readAll(), original);
}

void TestCompareGif::invalidDestinationReportsFailure() {
    QTemporaryDir directory;
    QImage image(32, 32, QImage::Format_RGB32);
    image.fill(Qt::red);
    std::atomic_bool cancelled{false};
    QString error;
    QVERIFY(!saveComparisonGif(directory.path(), image, image, 500, cancelled, &error));
    QVERIFY(!error.isEmpty());
}

void TestCompareGif::backgroundExportLetsGuiTimersRun() {
    QTemporaryDir directory;
    QImage image(4096, 2160, QImage::Format_RGB32);
    image.fill(Qt::red);
    QPointer<mplayer::shell::CompareGifExport> job = new mplayer::shell::CompareGifExport();
    QSignalSpy done(job, &mplayer::shell::CompareGifExport::finished);
    bool guiServiced = false;
    QTimer::singleShot(0, this, [&] {
        guiServiced = true;
        if (job) job->cancel();
    });
    job->start(directory.filePath(QStringLiteral("comparison.gif")), image, image, 1000);
    QVERIFY(!guiServiced);
    QTRY_VERIFY_WITH_TIMEOUT(guiServiced && !done.isEmpty(), 10000);
    QVERIFY(!done.first().at(0).toBool());
    QVERIFY(!QFile::exists(directory.filePath(QStringLiteral("comparison.gif"))));
}
