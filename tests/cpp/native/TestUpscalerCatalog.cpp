#include "TestUpscalerCatalog.hpp"

#include "native/models/NativeUpscalers.hpp"

#include <QtCore/QByteArray>
#include <QtCore/QCryptographicHash>
#include <QtCore/QRegularExpression>
#include <QtCore/QSet>

using namespace mplayer::native;

void TestUpscalerCatalog::theCataloguePinsFourGraphsWithExactDigests() {
    const QVector<UpscalerModel> catalog = upscalerCatalog();
    QCOMPARE(catalog.size(), 4);

    const QRegularExpression hex(QStringLiteral("^[0-9a-f]{64}$"));
    QSet<QString> ids;
    QSet<QString> digests;
    QSet<QString> names;
    for (const UpscalerModel& model : catalog) {
        QVERIFY2(hex.match(model.sha256).hasMatch(), qPrintable(model.sha256));
        QCOMPARE(model.id, QStringLiteral("sha256:") + model.sha256);
        QVERIFY(model.bytes > 0);
        QVERIFY(!model.displayName.isEmpty());
        QVERIFY(model.fileName.endsWith(QStringLiteral(".onnx")));
        QVERIFY(!model.sourceCard.isEmpty());
        // Every download is a pinned file, never a page or an archive: the
        // digest check is worthless if the URL can answer with something else.
        QVERIFY2(model.url.contains(QStringLiteral("/resolve/")), qPrintable(model.url));
        QVERIFY2(model.url.startsWith(QStringLiteral("https://huggingface.co/")),
                 qPrintable(model.url));
        ids.insert(model.id);
        digests.insert(model.sha256);
        names.insert(model.fileName);
    }
    // A duplicated digest would make two menu rows the same bytes, and a
    // duplicated file name would let one download overwrite the other.
    QCOMPARE(ids.size(), catalog.size());
    QCOMPARE(digests.size(), catalog.size());
    QCOMPARE(names.size(), catalog.size());
}

void TestUpscalerCatalog::everyRowResolvesByIdFileNameAndStem() {
    for (const UpscalerModel& model : upscalerCatalog()) {
        const auto byId = upscalerForId(model.id);
        QVERIFY(byId.has_value());
        QCOMPARE(byId->sha256, model.sha256);

        const auto byName = upscalerForId(model.fileName);
        QVERIFY(byName.has_value());
        QCOMPARE(byName->sha256, model.sha256);

        const QString stem = model.fileName.left(model.fileName.size() - 5);
        const auto byStem = upscalerForId(stem);
        QVERIFY(byStem.has_value());
        QCOMPARE(byStem->sha256, model.sha256);
    }
}

void TestUpscalerCatalog::everyRowResolvesByItsOwnDigestAndNoOther() {
    for (const UpscalerModel& model : upscalerCatalog()) {
        const auto resolved = upscalerForDigest(model.sha256.toUpper());
        QVERIFY(resolved.has_value());
        QCOMPARE(resolved->id, model.id);

        for (const UpscalerModel& other : upscalerCatalog()) {
            if (other.sha256 == model.sha256) continue;
            const auto wrong = upscalerForDigest(other.sha256);
            QVERIFY(wrong.has_value());
            QVERIFY(wrong->sha256 != model.sha256);
        }
    }
}

void TestUpscalerCatalog::theScaleAndPrecisionMatchThePublishedGraphs() {
    // The factor is the graph's own: the 4x Real-ESRGAN AnimeVideoV3 models and
    // the two 2x HFA2kCompact variants. A catalogue row that mixed them up would
    // apply the wrong output size before a frame was ever decoded.
    for (const UpscalerModel& model : upscalerCatalog()) {
        const bool realEsr = model.fileName.startsWith(QStringLiteral("realesr-animevideov3"));
        QCOMPARE(model.scale, realEsr ? 4 : 2);
        if (realEsr) {
            QVERIFY(model.precision == QStringLiteral("fp32") ||
                    model.precision == QStringLiteral("fp16"));
            QVERIFY(model.fileName.contains(model.precision));
        } else {
            QCOMPARE(model.precision, QStringLiteral("fp32"));
        }
    }
}

void TestUpscalerCatalog::theUrlsStayOnTheirPinnedHosts() {
    for (const UpscalerModel& model : upscalerCatalog()) {
        if (model.url.contains(QStringLiteral("nesaorg"))) {
            QVERIFY2(model.url.startsWith(QStringLiteral("https://huggingface.co/nesaorg/")),
                     qPrintable(model.url));
            QVERIFY(model.url.endsWith(model.fileName) ||
                    model.url.contains(QStringLiteral("_fp32_opset17.onnx")) ||
                    model.url.contains(QStringLiteral("multijpg_fp32_opset17.onnx")));
        } else {
            QVERIFY2(model.url.startsWith(
                         QStringLiteral("https://huggingface.co/buckets/smyhlin/")),
                     qPrintable(model.url));
        }
    }
}

void TestUpscalerCatalog::anUnknownIdentifierResolvesNothing() {
    QVERIFY(!upscalerForId(QString()).has_value());
    QVERIFY(!upscalerForId(QStringLiteral("not-a-model")).has_value());
    QVERIFY(!upscalerForId(QStringLiteral("sha256:00")).has_value());
    QVERIFY(!upscalerForDigest(QString()).has_value());
    QVERIFY(!upscalerForDigest(QStringLiteral("00")).has_value());
}

void TestUpscalerCatalog::onlyThePinnedBytesVerify() {
    // A synthetic graph with the pin rebuilt around those exact bytes: the check
    // is the rule, not the four published files.
    const QByteArray payload = QByteArray("synthetic upscaler bytes for the rule");
    UpscalerModel model;
    model.bytes = payload.size();
    model.sha256 = QString::fromLatin1(
        QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex());
    QString reason;
    QVERIFY(verifyUpscalerBytes(model, payload, &reason));
    QVERIFY(reason.isEmpty());
}

void TestUpscalerCatalog::aTruncatedOrForeignDownloadIsRefusedByName() {
    const QByteArray payload = QByteArray("synthetic upscaler bytes for the rule");
    UpscalerModel model;
    model.bytes = payload.size();
    model.sha256 = QString::fromLatin1(
        QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex());

    QString reason;
    QVERIFY(!verifyUpscalerBytes(model, payload.left(payload.size() - 1), &reason));
    QVERIFY2(reason.contains(QStringLiteral("bytes")), qPrintable(reason));

    QByteArray foreign = payload;
    foreign[0] = 'S';
    QVERIFY(!verifyUpscalerBytes(model, foreign, &reason));
    QVERIFY2(reason.contains(QStringLiteral("digest")), qPrintable(reason));
}
