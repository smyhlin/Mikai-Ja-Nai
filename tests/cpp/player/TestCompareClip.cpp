#include "TestCompareClip.hpp"
#include "CompareClip.hpp"

#include <QFile>
#include <QImageReader>
#include <QTemporaryDir>

using mplayer::shell::CompareClipFormat;
using mplayer::shell::saveComparisonClip;

namespace {
quint32 be32(const QByteArray& bytes, int pos) {
    return (quint32(uchar(bytes[pos])) << 24) | (quint32(uchar(bytes[pos + 1])) << 16) |
           (quint32(uchar(bytes[pos + 2])) << 8) | quint32(uchar(bytes[pos + 3]));
}
quint32 le32(const QByteArray& bytes, int pos) {
    return quint32(uchar(bytes[pos])) | (quint32(uchar(bytes[pos + 1])) << 8) |
           (quint32(uchar(bytes[pos + 2])) << 16) | (quint32(uchar(bytes[pos + 3])) << 24);
}
int findTag(const QByteArray& bytes, const char* tag) { return bytes.indexOf(QByteArray(tag, 4)); }
}

void TestCompareClip::mp4ContainsTimedJpegSamples() {
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("clip.mp4"));
    QImage a(32, 24, QImage::Format_RGB32); a.fill(Qt::red);
    QImage b(32, 24, QImage::Format_RGB32); b.fill(Qt::blue);
    std::atomic_bool cancelled{false}; QString error;
    QVERIFY2(saveComparisonClip(path, a, b, 250, CompareClipFormat::Mp4, cancelled, &error), qPrintable(error));
    QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly)); const QByteArray bytes = file.readAll();
    QCOMPARE(bytes.mid(4, 4), QByteArray("ftyp")); QVERIFY(bytes.indexOf("jpeg") >= 0);
    const int stts = findTag(bytes, "stts"); QVERIFY(stts >= 4);
    QCOMPARE(be32(bytes, stts + 8), 1u); QCOMPARE(be32(bytes, stts + 12), 40u); QCOMPARE(be32(bytes, stts + 16), 250u);
    const int stsz = findTag(bytes, "stsz"); QVERIFY(stsz >= 4);
    const quint32 sampleCount = be32(bytes, stsz + 12); QCOMPARE(sampleCount, 40u);
    const int stco = findTag(bytes, "stco"); QVERIFY(stco >= 4);
    QCOMPARE(be32(bytes, stco + 8), 40u);
    for (int i = 0; i < 2; ++i) {
        const quint32 offset = be32(bytes, stco + 12 + i * 4);
        const quint32 size = be32(bytes, stsz + 16 + i * 4);
        QVERIFY(offset + size <= quint32(bytes.size()));
        QImage decoded; QVERIFY(decoded.loadFromData(bytes.mid(offset, size), "JPEG"));
        QCOMPARE(decoded.size(), a.size());
    }
}

void TestCompareClip::animatedWebPContainsTimedFrames() {
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("clip.webp"));
    QImage a(24, 18, QImage::Format_RGB32); a.fill(Qt::red);
    QImage b(24, 18, QImage::Format_RGB32); b.fill(Qt::blue);
    std::atomic_bool cancelled{false}; QString error;
    QVERIFY2(saveComparisonClip(path, a, b, 200, CompareClipFormat::AnimatedWebP, cancelled, &error), qPrintable(error));
    QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly)); const QByteArray bytes = file.readAll();
    QCOMPARE(bytes.mid(0, 4), QByteArray("RIFF")); QCOMPARE(bytes.mid(8, 4), QByteArray("WEBP"));
    QCOMPARE(le32(bytes, 4) + 8, quint32(bytes.size()));
    const int first = findTag(bytes, "ANMF"); QVERIFY(first >= 0);
    const quint32 firstSize = le32(bytes, first + 4); QVERIFY(firstSize >= 16);
    QCOMPARE(uchar(bytes[first + 20]), 200); QCOMPARE(uchar(bytes[first + 21]), 0); QCOMPARE(uchar(bytes[first + 22]), 0);
    QVERIFY(bytes.indexOf("ANMF", first + 4) > first);
    QImageReader reader(path); QVERIFY(reader.supportsAnimation()); QCOMPARE(reader.imageCount(), 2);
    QImage decoded = reader.read(); QVERIFY2(!decoded.isNull(), qPrintable(reader.errorString()));
    QCOMPARE(reader.nextImageDelay(), 200); QCOMPARE(decoded.size(), a.size());
    decoded = reader.read(); QVERIFY(!decoded.isNull()); QCOMPARE(reader.nextImageDelay(), 200);
}

void TestCompareClip::cancellationPreservesExistingDestination() {
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("clip.webp"));
    QFile existing(path); QVERIFY(existing.open(QIODevice::WriteOnly)); QCOMPARE(existing.write("keep"), qint64(4)); existing.close();
    QImage image(8, 8, QImage::Format_RGB32); image.fill(Qt::green);
    std::atomic_bool cancelled{true}; QString error;
    QVERIFY(!saveComparisonClip(path, image, image, 100, CompareClipFormat::AnimatedWebP, cancelled, &error));
    QVERIFY2(!error.isEmpty(), "Cancellation must report an error.");
    QVERIFY(existing.open(QIODevice::ReadOnly)); QCOMPARE(existing.readAll(), QByteArray("keep"));
}

void TestCompareClip::rejectsClipsLongerThanTenSeconds() {
    QImage image(8, 8, QImage::Format_RGB32); image.fill(Qt::black);
    std::atomic_bool cancelled{false}; QString error;
    QVERIFY(!saveComparisonClip(QString(), image, image, 5001, CompareClipFormat::Mp4, cancelled, &error));
    QVERIFY(error.contains(QStringLiteral("10 seconds")));
}
