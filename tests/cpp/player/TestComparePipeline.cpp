#include "TestComparePipeline.hpp"

#include "native/components/AnimeJanaiBuildLog.hpp"
#include "native/components/NativePipelineSpec.hpp"
#include "player/compare/PictureMetrics.hpp"
#include "player/compare/CompareWindow.hpp"
#include "player/quick/PlayerController.hpp"
#include "player/quick/PlayerWindow.hpp"
#include "player/quick/ClipWindow.hpp"
#include "player/shell/MediaInformationDialog.hpp"
#include "../QuickPlayerTestSupport.hpp"

#include <QApplication>
#include <QFileInfo>
#include <QImage>
#include <QPointer>

#include <cmath>
#include <memory>

namespace {

/// A picture with real structure: horizontal bars plus a checkerboard, so its
/// laplacian energy is far above the flat-frame refusal.
QImage texturedFrame(int width = 64, int height = 48) {
    QImage image(width, height, QImage::Format_RGB32);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int bar = (y / 4) % 2 == 0 ? 200 : 40;
            const int checker = ((x / 4) + (y / 4)) % 2 == 0 ? 30 : -30;
            const int value = qBound(0, bar + checker, 255);
            image.setPixel(x, y, qRgb(value, value, value));
        }
    }
    return image;
}

QImage flatFrame(int width = 64, int height = 48) {
    QImage image(width, height, QImage::Format_RGB32);
    image.fill(qRgb(16, 16, 16));
    return image;
}

mplayer::native::AnimeJanaiPlanStage ajiStage(const QString& label, int scale) {
    mplayer::native::AnimeJanaiPlanStage stage;
    stage.kind = mplayer::native::AnimeJanaiPlanStage::Kind::Aji;
    stage.filterLabel = label;
    stage.filterSpec = QByteArrayLiteral("@mikai_aji:conf=example.ini:slot=2001");
    stage.scaleFactor = scale;
    return stage;
}

}  // namespace

void TestComparePipeline::metricsMeasureLevelsAndRefuseMismatchedGeometry() {
    const QImage reference = texturedFrame();
    QVERIFY(mplayer::shell::meanDifference(reference, reference) == 0.0);

    QImage shifted = reference;
    for (int y = 0; y < shifted.height(); ++y) {
        auto* row = reinterpret_cast<QRgb*>(shifted.scanLine(y));
        for (int x = 0; x < shifted.width(); ++x) {
            const int value = qRed(row[x]);
            row[x] = qRgb(qMin(255, value + 8), qMin(255, value + 8), qMin(255, value + 8));
        }
    }
    const double difference = mplayer::shell::meanDifference(reference, shifted);
    QVERIFY(difference > 1.0);
    QVERIFY(difference < 9.0);

    // Different geometry is not "no difference": it is not comparable at all.
    QVERIFY(mplayer::shell::meanDifference(reference, texturedFrame(32, 24)) < 0.0);
}

void TestComparePipeline::metricsSeparateATexturedFrameFromAFlatOne() {
    const double textured = mplayer::shell::edgeEnergy(texturedFrame());
    const double flat = mplayer::shell::referenceDetail(flatFrame());
    QVERIFY(textured > 5.0);
    QVERIFY(flat < mplayer::shell::kMinimumReferenceDetail);
}

void TestComparePipeline::verdictAcceptsAModelEffectAndRejectsAColourShift() {
    const QImage reference = texturedFrame();
    QImage model = reference;
    for (int y = 0; y < model.height(); ++y) {
        auto* row = reinterpret_cast<QRgb*>(model.scanLine(y));
        for (int x = 0; x < model.width(); ++x) {
            const int value = qRed(row[x]);
            row[x] = qRgb(qMin(255, value + 6), qMin(255, value + 6), qMin(255, value + 6));
        }
    }
    const mplayer::shell::EffectVerdict accepted =
        mplayer::shell::judgeEffect(reference, model);
    QCOMPARE(accepted.kind, mplayer::shell::EffectVerdict::Kind::Ok);

    // An NV12 round trip plus one resample moves the picture by hundredths.
    const mplayer::shell::EffectVerdict refused =
        mplayer::shell::judgeEffect(reference, reference);
    QCOMPARE(refused.kind, mplayer::shell::EffectVerdict::Kind::NoEffect);
}

void TestComparePipeline::verdictRefusesAFlatReferenceFrame() {
    const QImage flat = flatFrame();
    const mplayer::shell::EffectVerdict verdict = mplayer::shell::judgeEffect(flat, flat);
    QCOMPARE(verdict.kind, mplayer::shell::EffectVerdict::Kind::FlatFrame);

    const mplayer::shell::EffectVerdict mismatch =
        mplayer::shell::judgeEffect(texturedFrame(), texturedFrame(32, 24));
    QCOMPARE(mismatch.kind, mplayer::shell::EffectVerdict::Kind::GeometryMismatch);
}

void TestComparePipeline::verdictRefusesABlankModelAndADitheredCard() {
    // A model that emits pure black must never pass, however far it differs.
    const QImage reference = texturedFrame();
    QImage black = flatFrame();
    black.fill(qRgb(0, 0, 0));
    QCOMPARE(mplayer::shell::judgeEffect(reference, black).kind,
             mplayer::shell::EffectVerdict::Kind::BlankModel);

    // A black card with about one level of dither has laplacian energy above the
    // edge floor, so the range - not the edges - is what refuses it.
    QImage dithered = flatFrame();
    for (int y = 0; y < dithered.height(); ++y) {
        for (int x = 0; x < dithered.width(); ++x) {
            const int value = ((x + y) % 3 == 0) ? 17 : 16;
            dithered.setPixel(x, y, qRgb(value, value, value));
        }
    }
    // Dither raises the laplacian above the detail floor; the range is what refuses it.
    QVERIFY(mplayer::shell::referenceDetail(dithered) > mplayer::shell::kMinimumReferenceDetail);
    QCOMPARE(mplayer::shell::judgeEffect(dithered, dithered).kind,
             mplayer::shell::EffectVerdict::Kind::FlatFrame);
}

void TestComparePipeline::detailMeasureSurvivesAUpscaledCanvas() {
    // A 4x canvas halves the per-pixel gradient of the same picture. The gate
    // must see the picture, not the canvas, or every 4x model is refused.
    const QImage source = texturedFrame(320, 240);
    QImage upscaled = source.scaled(1280, 960, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    QVERIFY(mplayer::shell::edgeEnergy(source) > mplayer::shell::edgeEnergy(upscaled));
    QVERIFY(mplayer::shell::referenceDetail(source) > mplayer::shell::kMinimumReferenceDetail);
    QVERIFY(mplayer::shell::referenceDetail(upscaled) >
            mplayer::shell::kMinimumReferenceDetail);
    // A flat card stays flat at any canvas size.
    QCOMPARE(mplayer::shell::judgeEffect(upscaled, upscaled).kind,
             mplayer::shell::EffectVerdict::Kind::NoEffect);
    QImage flatLarge = flatFrame(1280, 960);
    QCOMPARE(mplayer::shell::judgeEffect(flatLarge, flatLarge).kind,
             mplayer::shell::EffectVerdict::Kind::FlatFrame);
}

void TestComparePipeline::configuredSlotParsesGeometryAndDepth() {
    const auto parsed = mplayer::native::parseAnimeJanaiConfiguredSlot(
        QStringLiteral("Configured slot 2001: 480x360 -> 960x720 (depth 3)"));
    QVERIFY(parsed.has_value());
    QCOMPARE(parsed->slot, 2001);
    QCOMPARE(parsed->input, QSize(480, 360));
    QCOMPARE(parsed->output, QSize(960, 720));
    QVERIFY(parsed->depth.has_value());
    QCOMPARE(parsed->depth.value_or(0), 3);

    QVERIFY(!mplayer::native::parseAnimeJanaiConfiguredSlot(
                 QStringLiteral("VO: [libmpv] 960x720 nv12"))
                 .has_value());
}

void TestComparePipeline::configuredSlotAcceptsAMissingDepthSuffix() {
    const auto parsed = mplayer::native::parseAnimeJanaiConfiguredSlot(
        QStringLiteral("[animejanai] Configured slot 1: 1920x1080 -> 1920x1080"));
    QVERIFY(parsed.has_value());
    QCOMPARE(parsed->slot, 1);
    QCOMPARE(parsed->output, QSize(1920, 1080));
    QVERIFY(!parsed->depth.has_value());
}

void TestComparePipeline::pipelineSpecMatchesThePlayersDecoderAndFilter() {
    mplayer::native::AnimeJanaiPlan plan;
    plan.enabled = true;
    plan.requiresAji = true;
    plan.backend = mplayer::native::Backend::TensorRTRTX;
    plan.filterSpec = QByteArrayLiteral("@mikai_aji:conf=example.ini:slot=2001");
    plan.stages.append(ajiStage(QStringLiteral("mikai_aji"), 2));

    const mplayer::native::NativePipelineSpec spec =
        mplayer::native::nativePipelineFor(plan, QSize(480, 360));
    // The player attaches this string verbatim and decodes through nvdec; a
    // comparison that builds its own variant measures a different program.
    QCOMPARE(spec.hwdec, QStringLiteral("nvdec"));
    QCOMPARE(spec.vf, QString::fromUtf8(plan.filterSpec));
    QCOMPARE(spec.ajiLabels, QStringList{QStringLiteral("mikai_aji")});
    QCOMPARE(spec.expectedOutput, QSize(960, 720));

    mplayer::native::AnimeJanaiPlan directMl = plan;
    directMl.backend = mplayer::native::Backend::DirectML;
    QCOMPARE(mplayer::native::nativePipelineFor(directMl, QSize(480, 360)).hwdec,
             QStringLiteral("d3d11"));
}

void TestComparePipeline::pipelineSpecIsEmptyForADisabledPlan() {
    mplayer::native::AnimeJanaiPlan plan;
    plan.enabled = false;
    const mplayer::native::NativePipelineSpec spec =
        mplayer::native::nativePipelineFor(plan, QSize(480, 360));
    QVERIFY(spec.hwdec.isEmpty());
    QVERIFY(spec.vf.isEmpty());
    QVERIFY(spec.ajiLabels.isEmpty());
}

void TestComparePipeline::decoderVerdictRequiresADeviceFramesDecoder() {
    using mplayer::native::Backend;
    using mplayer::native::decoderFeedsNativeBackend;
    QVERIFY(decoderFeedsNativeBackend(QStringLiteral("nvdec"), Backend::TensorRT));
    QVERIFY(decoderFeedsNativeBackend(QStringLiteral("nvdec"), Backend::TensorRTRTX));
    QVERIFY(!decoderFeedsNativeBackend(QStringLiteral("nvdec-copy"), Backend::TensorRT));
    QVERIFY(!decoderFeedsNativeBackend(QStringLiteral("no"), Backend::TensorRT));
    QVERIFY(!decoderFeedsNativeBackend(QStringLiteral("auto-safe"), Backend::TensorRT));
    QVERIFY(decoderFeedsNativeBackend(QStringLiteral("d3d11"), Backend::DirectML));
    QVERIFY(!decoderFeedsNativeBackend(QStringLiteral("d3d11va-copy"), Backend::DirectML));
    QVERIFY(!decoderFeedsNativeBackend(QStringLiteral("nvdec"), Backend::DirectML));
}
void TestComparePipeline::controllerDestructionClosesAuxiliariesBeforeReleasingSession() {
    QString error;
    if (!mikai_test::quick::loadExplicitMpvRuntime(&error)) {
        QSKIP(qPrintable(error));
    }

    const QString media = mikai_test::quick::benchmarkVideoPath();
    QVERIFY2(QFileInfo::exists(media), qPrintable(media));
    auto controller = std::make_unique<mplayer::quick::PlayerController>(
        QStringLiteral("Compare teardown test"), media);
    mplayer::VideoSession* session = controller->session();
    QVERIFY(session != nullptr);
    QPointer<mplayer::VideoSession> guardedSession(session);
    QPointer<mplayer::shell::CompareWindow> compare;
    QPointer<mplayer::quick::ClipWindow> clip;
    QPointer<QWidget> information;
    bool compareReleasedBeforeSession = false;
    bool clipClosedBeforeSession = false;
    bool informationClosedBeforeSession = false;

    auto* playerWindow = controller->window();
    QVERIFY(playerWindow != nullptr);
    playerWindow->showWindowed();
    controller->startPlayer();
    QTRY_VERIFY_WITH_TIMEOUT(session->hasVideo(), 15000);
    QTRY_VERIFY_WITH_TIMEOUT(std::isfinite(session->duration()) && session->duration() > 0.0,
                             15000);

    controller->openMediaInformation();
    controller->openClipWindow();
    controller->openCompare();
    QTRY_VERIFY_WITH_TIMEOUT([&] {
        for (QWidget* window : QApplication::topLevelWidgets()) {
            if (auto* candidate = qobject_cast<mplayer::shell::CompareWindow*>(window)) {
                compare = candidate;
            } else if (window->objectName() == QStringLiteral("playerMediaInformation")) {
                information = window;
            }
        }
        for (QWindow* window : QGuiApplication::topLevelWindows()) {
            if (auto* candidate = qobject_cast<mplayer::quick::ClipWindow*>(window)) clip = candidate;
        }
        return compare != nullptr && clip != nullptr && information != nullptr;
    }(), 15000);
    QObject::connect(clip, &mplayer::quick::ClipWindow::dismissed, qApp,
                     [&] { clipClosedBeforeSession = guardedSession != nullptr; });
    QObject::connect(qobject_cast<QDialog*>(information.data()), &QDialog::finished, qApp,
                     [&](int) { informationClosedBeforeSession = guardedSession != nullptr; });

    QTRY_VERIFY_WITH_TIMEOUT(session->nativePlanLocked(), 5000);
    compare->close();
    QTRY_VERIFY_WITH_TIMEOUT(compare == nullptr, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!session->nativePlanLocked(), 5000);

    controller->openCompare();
    QTRY_VERIFY_WITH_TIMEOUT([&] {
        for (QWidget* window : QApplication::topLevelWidgets()) {
            if (auto* candidate = qobject_cast<mplayer::shell::CompareWindow*>(window)) {
                compare = candidate;
                return true;
            }
        }
        return false;
    }(), 15000);
    QTRY_VERIFY_WITH_TIMEOUT(session->nativePlanLocked(), 5000);
    QObject::connect(compare, &QObject::destroyed, qApp, [&] {
        compareReleasedBeforeSession =
            guardedSession != nullptr && !guardedSession->nativePlanLocked();
    });

    controller.reset();
    QTRY_VERIFY_WITH_TIMEOUT(compare == nullptr, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(clip == nullptr, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(information == nullptr, 5000);
    QVERIFY2(compareReleasedBeforeSession,
             "Compare's native plan lock and renderer must be released before the session");
    QVERIFY(clipClosedBeforeSession);
    QVERIFY(informationClosedBeforeSession);
    QTRY_VERIFY_WITH_TIMEOUT(guardedSession == nullptr, 5000);
}
