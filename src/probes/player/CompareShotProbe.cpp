#include <QApplication>
#include <QCryptographicHash>
#include <QColorSpace>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QPixmap>
#include <QPointer>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <cstdio>
#include <cmath>
#include <algorithm>

#include "CompareRenderer.hpp"
#include "native/components/AnimeJanaiRuntime.hpp"

#include "CompareCanvas.hpp"
#include "CompareWindow.hpp"
#include "PlayerDialog.hpp"
#include "PlayerShell.hpp"
#include "ProbeRuntime.hpp"
#include "VideoSurface.hpp"

#ifdef Q_OS_WIN
#include <QtCore/qt_windows.h>
#endif

using namespace mplayer;
using namespace mplayer::shell;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::fprintf(stderr, "%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

class Host : public PlayerHost {
public:
    void requestVariant(const QString&, const QString&) override {}
    void requestEpisode(int) override {}
    void requestTrackProbe(const QString&) override {}
    void requestCopyStreamUrl() override {}
    void playerClosed() override {}
    void playbackFailed() override {}
};

QString backendName(native::Backend backend) {
    switch (backend) {
    case native::Backend::TensorRT: return QStringLiteral("TensorRT");
    case native::Backend::DirectML: return QStringLiteral("DirectML");
    case native::Backend::TensorRTRTX: return QStringLiteral("TensorRT-RTX");
    }
    return QStringLiteral("unknown");
}

bool writeText(const QString& path, const QString& text) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text) &&
           file.write(text.toUtf8()) == text.toUtf8().size();
}

QString sha256File(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return QStringLiteral("<unreadable>");
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        const QByteArray chunk = file.read(1024 * 1024);
        if (chunk.isEmpty() && file.error() != QFileDevice::NoError)
            return QStringLiteral("<read-error>");
        hash.addData(chunk);
    }
    return QString::fromLatin1(hash.result().toHex());
}

QString imageStats(const QImage& image) {
    if (image.isNull()) return QStringLiteral("null");
    const QImage rgb = image.convertToFormat(QImage::Format_RGB888);
    quint64 count = 0;
    double sums[3] = {};
    int mins[3] = {255, 255, 255};
    int maxs[3] = {};
    for (int y = 0; y < rgb.height(); y += 4) {
        const uchar* row = rgb.constScanLine(y);
        for (int x = 0; x < rgb.width(); x += 4) {
            for (int c = 0; c < 3; ++c) {
                const int value = row[x * 3 + c];
                sums[c] += value;
                mins[c] = std::min(mins[c], value);
                maxs[c] = std::max(maxs[c], value);
            }
            ++count;
        }
    }
    return QStringLiteral("%1x%2 format=RGB888 colorSpace=%3 sampledMeanRGB=%4,%5,%6 "
                          "sampledMinRGB=%7,%8,%9 sampledMaxRGB=%10,%11,%12")
        .arg(image.width()).arg(image.height())
        .arg(image.colorSpace().description().isEmpty()
                 ? QStringLiteral("<unset>") : image.colorSpace().description())
        .arg(count ? sums[0] / count : 0.0, 0, 'f', 2)
        .arg(count ? sums[1] / count : 0.0, 0, 'f', 2)
        .arg(count ? sums[2] / count : 0.0, 0, 'f', 2)
        .arg(mins[0]).arg(mins[1]).arg(mins[2])
        .arg(maxs[0]).arg(maxs[1]).arg(maxs[2]);
}

int runDirectDiagnostic(const QString& url) {
    const QString outputDir = qEnvironmentVariable("MIKAI_COMPARE_PROBE_DIAGNOSTIC_DIR");
    if (outputDir.isEmpty()) {
        std::fprintf(stderr, "FAIL diagnostic mode requires MIKAI_COMPARE_PROBE_DIAGNOSTIC_DIR\n");
        return 2;
    }
    if (!QDir().mkpath(outputDir)) {
        std::fprintf(stderr, "FAIL cannot create diagnostic directory: %s\n", qPrintable(outputDir));
        return 2;
    }
    bool timestampOk = false;
    const double timestamp = qEnvironmentVariable("MIKAI_COMPARE_PROBE_TIMESTAMP", "2.0")
                                 .toDouble(&timestampOk);
    bool widthOk = false;
    bool heightOk = false;
    bool sourceWidthOk = false;
    bool sourceHeightOk = false;
    bool sourceFpsOk = false;
    const int width = qEnvironmentVariable("MIKAI_COMPARE_PROBE_WIDTH", "1280").toInt(&widthOk);
    const int height = qEnvironmentVariable("MIKAI_COMPARE_PROBE_HEIGHT", "720").toInt(&heightOk);
    const int sourceWidth =
        qEnvironmentVariable("MIKAI_COMPARE_PROBE_SOURCE_WIDTH").toInt(&sourceWidthOk);
    const int sourceHeight =
        qEnvironmentVariable("MIKAI_COMPARE_PROBE_SOURCE_HEIGHT").toInt(&sourceHeightOk);
    const double sourceFps =
        qEnvironmentVariable("MIKAI_COMPARE_PROBE_SOURCE_FPS").toDouble(&sourceFpsOk);
    const QString modelId = qEnvironmentVariable(
        "MIKAI_COMPARE_PROBE_AJI",
        "upscaler:realesr-animevideov3-4x-fp32.onnx");
    native::Backend backend = native::Backend::TensorRT;
    const QString requestedBackend = qEnvironmentVariable("MIKAI_COMPARE_PROBE_BACKEND", "tensorrt")
                                         .trimmed().toLower();
    if (requestedBackend == QLatin1String("directml")) backend = native::Backend::DirectML;
    else if (requestedBackend == QLatin1String("tensorrt-rtx")) backend = native::Backend::TensorRTRTX;
    else if (requestedBackend != QLatin1String("tensorrt")) {
        std::fprintf(stderr, "FAIL unknown diagnostic backend: %s\n", qPrintable(requestedBackend));
        return 2;
    }
    if (!timestampOk || !std::isfinite(timestamp) || timestamp < 0 ||
        !widthOk || !heightOk || width <= 0 || height <= 0 ||
        (sourceWidthOk && sourceWidth <= 0) || (sourceHeightOk && sourceHeight <= 0) ||
        (sourceFpsOk && (!std::isfinite(sourceFps) || sourceFps < 0))) {
        std::fprintf(stderr, "FAIL invalid diagnostic timestamp or geometry\n");
        return 2;
    }

    QStringList log;
    log << QStringLiteral("mode=direct-compare-renderer")
        << QStringLiteral("source=%1").arg(url)
        << QStringLiteral("timestamp=%1").arg(timestamp, 0, 'f', 6)
        << QStringLiteral("requestedGeometry=%1x%2").arg(width).arg(height)
        << QStringLiteral("requestedSourceContext=%1x%2 fps=%3")
               .arg(sourceWidthOk ? sourceWidth : width)
               .arg(sourceHeightOk ? sourceHeight : height)
               .arg(sourceFpsOk ? sourceFps : 0.0, 0, 'f', 6)
        << QStringLiteral("presetId=%1").arg(modelId)
        << QStringLiteral("backend=%1").arg(backendName(backend))
        << QStringLiteral("inputSemantics=mpv-decoded/rendered QImage converted to RGB888; "
                          "this is not original encoded YUV or HDR metadata");
    const auto progress = [&log](const QString& detail) {
        log << QStringLiteral("renderer-progress=%1").arg(detail);
        std::fprintf(stderr, "     renderer: %s\n", qPrintable(detail));
    };
    const auto cancelled = [] { return false; };
    CompareRenderer renderer;
    QImage raw;
    if (!renderer.capture(url, timestamp, QSize(width, height), cancelled, progress, {}, &raw)) {
        log << QStringLiteral("capture-error=%1").arg(renderer.error());
        writeText(QDir(outputDir).filePath(QStringLiteral("diagnostics.txt")), log.join(QLatin1Char('\n')));
        std::fprintf(stderr, "FAIL diagnostic source capture: %s\n", qPrintable(renderer.error()));
        return 1;
    }
    const QString rawPath = QDir(outputDir).filePath(QStringLiteral("raw-source.png"));
    if (!raw.save(rawPath)) {
        std::fprintf(stderr, "FAIL could not save raw source image\n");
        return 1;
    }
    log << QStringLiteral("rawSource=%1").arg(imageStats(raw))
        << QStringLiteral("modelInputGeometry=%1x%2 (native decoded RGB; no source enlargement)")
               .arg(raw.width()).arg(raw.height())
        << QStringLiteral("rawSourceFile=%1").arg(rawPath);

    const bool sourceGeometryMismatch =
        (sourceWidthOk && sourceWidth != raw.width()) ||
        (sourceHeightOk && sourceHeight != raw.height());
    if (sourceGeometryMismatch) {
        log << QStringLiteral("result=FAIL requested source geometry %1x%2 differs from captured raw geometry %3x%4")
                   .arg(sourceWidthOk ? sourceWidth : raw.width())
                   .arg(sourceHeightOk ? sourceHeight : raw.height())
                   .arg(raw.width()).arg(raw.height());
        writeText(QDir(outputDir).filePath(QStringLiteral("diagnostics.txt")),
                  log.join(QLatin1Char('\n')));
        std::fprintf(stderr,
                     "FAIL requested source geometry differs from captured raw geometry (%dx%d vs %dx%d)\n",
                     sourceWidthOk ? sourceWidth : raw.width(),
                     sourceHeightOk ? sourceHeight : raw.height(), raw.width(), raw.height());
        return 1;
    }

    const native::AnimeJanaiPlan plan = native::prepareAnimeJanaiPlan(
        modelId, backend, cancelled,
        native::AnimeJanaiSourceContext{raw.width(), raw.height(),
                                        sourceFpsOk ? sourceFps : 0.0}, {});
    log << QStringLiteral("plan.enabled=%1").arg(plan.enabled)
        << QStringLiteral("plan.available=%1").arg(plan.available)
        << QStringLiteral("plan.reasonKey=%1").arg(plan.reasonKey)
        << QStringLiteral("plan.reason=%1").arg(plan.reason)
        << QStringLiteral("plan.presetId=%1").arg(plan.presetId)
        << QStringLiteral("plan.backend=%1").arg(backendName(plan.backend))
        << QStringLiteral("plan.slot=%1").arg(plan.slot)
        << QStringLiteral("plan.filterSpec=%1").arg(QString::fromUtf8(plan.filterSpec))
        << QStringLiteral("plan.libraryPath=%1").arg(plan.libraryPath)
        << QStringLiteral("plan.configPath=%1").arg(plan.configPath)
        << QStringLiteral("plan.modelDir=%1").arg(plan.modelDir)
        << QStringLiteral("plan.requiresAji=%1").arg(plan.requiresAji)
        << QStringLiteral("plan.stageCount=%1").arg(plan.stages.size());
    for (int i = 0; i < plan.stages.size(); ++i) {
        const native::AnimeJanaiPlanStage& stage = plan.stages.at(i);
        log << QStringLiteral("stage[%1].kind=%2").arg(i).arg(
                   stage.kind == native::AnimeJanaiPlanStage::Kind::Aji
                       ? QStringLiteral("AJI") : QStringLiteral("Anime4K"))
            << QStringLiteral("stage[%1].label=%2").arg(i).arg(stage.filterLabel)
            << QStringLiteral("stage[%1].filterSpec=%2").arg(i).arg(QString::fromUtf8(stage.filterSpec))
            << QStringLiteral("stage[%1].scaleFactor=%2").arg(i).arg(stage.scaleFactor)
            << QStringLiteral("stage[%1].configPath=%2").arg(i).arg(stage.configPath);
        if (!stage.configPath.isEmpty()) {
            QFile config(stage.configPath);
            if (config.open(QIODevice::ReadOnly | QIODevice::Text))
                log << QStringLiteral("stage[%1].configContents:\n%2").arg(i).arg(QString::fromUtf8(config.readAll()));
        }
    }
    const QFileInfoList graphs = QDir(plan.modelDir).entryInfoList(
        {QStringLiteral("*.onnx")}, QDir::Files, QDir::Name);
    for (const QFileInfo& graph : graphs)
        log << QStringLiteral("graph=%1 sha256=%2 bytes=%3")
                   .arg(graph.absoluteFilePath(), sha256File(graph.absoluteFilePath()))
                   .arg(graph.size());

    if (!plan.available || !plan.requiresAji || plan.presetId != modelId) {
        log << QStringLiteral("result=FAIL plan is unavailable, not AJI, or does not match the requested catalog identity");
        writeText(QDir(outputDir).filePath(QStringLiteral("diagnostics.txt")), log.join(QLatin1Char('\n')));
        std::fprintf(stderr, "FAIL requested AJI catalog plan unavailable/incorrect: %s\n", qPrintable(plan.reason));
        return 1;
    }

    const QImage baseline = renderer.render(QStringLiteral("off"), native::AnimeJanaiPlan{},
                                           cancelled, progress);
    if (baseline.isNull()) {
        log << QStringLiteral("baseline-error=%1").arg(renderer.error());
        writeText(QDir(outputDir).filePath(QStringLiteral("diagnostics.txt")), log.join(QLatin1Char('\n')));
        std::fprintf(stderr, "FAIL baseline renderer: %s\n", qPrintable(renderer.error()));
        return 1;
    }
    if (!baseline.save(QDir(outputDir).filePath(QStringLiteral("baseline-off.png")))) {
        std::fprintf(stderr, "FAIL could not save baseline output\n");
        return 1;
    }
    log << QStringLiteral("baseline=%1").arg(imageStats(baseline));

    const QImage modelOutput = renderer.render(QStringLiteral("off"), plan, cancelled, progress);
    if (modelOutput.isNull()) {
        log << QStringLiteral("model-error=%1").arg(renderer.error());
        writeText(QDir(outputDir).filePath(QStringLiteral("diagnostics.txt")), log.join(QLatin1Char('\n')));
        std::fprintf(stderr, "FAIL model renderer: %s\n", qPrintable(renderer.error()));
        return 1;
    }
    if (!modelOutput.save(QDir(outputDir).filePath(QStringLiteral("model-output.png")))) {
        std::fprintf(stderr, "FAIL could not save model output\n");
        return 1;
    }
    log << QStringLiteral("modelOutput=%1").arg(imageStats(modelOutput));

    const QImage offBetween = renderer.render(QStringLiteral("off"), native::AnimeJanaiPlan{},
                                              cancelled, progress);
    if (offBetween.isNull() ||
        !offBetween.save(QDir(outputDir).filePath(QStringLiteral("off-between-runs.png")))) {
        log << QStringLiteral("intermediateOffError=%1").arg(renderer.error());
        writeText(QDir(outputDir).filePath(QStringLiteral("diagnostics.txt")),
                  log.join(QLatin1Char('\n')));
        return 1;
    }
    const QImage repeatedOutput = renderer.render(QStringLiteral("off"), plan, cancelled, progress);
    if (repeatedOutput.isNull() ||
        !repeatedOutput.save(QDir(outputDir).filePath(QStringLiteral("model-output-repeat.png")))) {
        log << QStringLiteral("repeatModelError=%1").arg(renderer.error());
        writeText(QDir(outputDir).filePath(QStringLiteral("diagnostics.txt")),
                  log.join(QLatin1Char('\n')));
        return 1;
    }
    log << QStringLiteral("offBetweenRuns=%1").arg(imageStats(offBetween))
        << QStringLiteral("repeatedModelOutput=%1").arg(imageStats(repeatedOutput));
    const QImage a = baseline.convertToFormat(QImage::Format_RGB888);
    const QImage b = modelOutput.convertToFormat(QImage::Format_RGB888);
    quint64 different = 0;
    quint64 count = 0;
    quint64 absoluteError = 0;
    if (a.size() == b.size()) {
        for (int y = 0; y < a.height(); ++y) {
            const uchar* ar = a.constScanLine(y);
            const uchar* br = b.constScanLine(y);
            for (int x = 0; x < a.width(); ++x) {
                bool pixelDiffers = false;
                for (int c = 0; c < 3; ++c) {
                    const int delta = int(ar[x * 3 + c]) - int(br[x * 3 + c]);
                    absoluteError += static_cast<quint64>(std::abs(delta));
                    pixelDiffers = pixelDiffers || delta != 0;
                }
                different += pixelDiffers ? 1 : 0;
                ++count;
            }
        }
    }
    const double changedPercent = count ? 100.0 * double(different) / double(count) : 0.0;
    const double meanAbsoluteError = count ? double(absoluteError) / (3.0 * double(count)) : 0.0;
    log << QStringLiteral("baselineModelPixelDifference=%1/%2 (%3%) meanAbsoluteErrorRGB=%4")
               .arg(different).arg(count).arg(changedPercent, 0, 'f', 4)
               .arg(meanAbsoluteError, 0, 'f', 4)
        << QStringLiteral("runtimeProcessingObserved=required by CompareRenderer AJI settle path");
    const bool outputsDifferent = a.size() == b.size() && different != 0;
    log << QStringLiteral("result=%1").arg(outputsDifferent ? QStringLiteral("PASS")
                                                           : QStringLiteral("FAIL model output is identical or geometry differs"));
    const bool diagnosticsSaved = writeText(
        QDir(outputDir).filePath(QStringLiteral("diagnostics.txt")), log.join(QLatin1Char('\n')));
    renderer.stop();
    std::fprintf(stderr, "     diagnostic artifacts: %s\n", qPrintable(outputDir));
    std::fprintf(stderr, "     baseline/model changed pixels: %.4f%%; MAE %.4f\n",
                 changedPercent, meanAbsoluteError);
    return outputsDifferent && diagnosticsSaved ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    // Before the first PlayerDialog reads player/anime4k and
    // player/presetShortcuts, or the probe inherits a real session.
    QTemporaryDir settingsDir;
    if (!settingsDir.isValid()) {
        std::fprintf(stderr, "FAIL cannot create an isolated settings directory\n");
        return 1;
    }
    QCoreApplication::setOrganizationName(QStringLiteral("MikaiShotProbe"));
    QCoreApplication::setApplicationName(QStringLiteral("CompareShotProbe"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settingsDir.path());
    if (!mplayer::probes::selectRuntime()) {
        return 2;
    }
    // A real URL can be supplied to reproduce a source that refuses a second
    // decode; the default stays synthetic so the probe needs no network.
    const QString url = qEnvironmentVariableIsEmpty("MIKAI_COMPARE_PROBE_URL")
                            ? QStringLiteral("av://lavfi:testsrc2=size=1280x720:rate=25")
                            : qEnvironmentVariable("MIKAI_COMPARE_PROBE_URL");
    if (!qEnvironmentVariableIsEmpty("MIKAI_COMPARE_PROBE_DIAGNOSTIC_DIR"))
        return runDirectDiagnostic(url);
    static Host host;
    const QList<QualityEntry> qualities = {{480, url}, {720, url}, {1080, url}};

    auto* dialog = new PlayerDialog(QStringLiteral("shot"), url, qualities, 720, &host);
    dialog->setVariants({QVariantMap{{"label", QStringLiteral("v")},
                                    {"embed", QStringLiteral("e1")}}},
                        QStringLiteral("e1"));
    dialog->startPlayer();
    dialog->showFullScreen();

    // Optional: bind an AnimeJaNai chain to the playing surface first, so the
    // comparison pane preloads a closure the player already holds.
    if (!qEnvironmentVariableIsEmpty("MIKAI_COMPARE_PROBE_PLAYER_AJI")) {
        QTimer::singleShot(1500, &app, [&] {
            dialog->applyAnimeJanaiRequest(qEnvironmentVariable("MIKAI_COMPARE_PROBE_PLAYER_AJI"));
        });
        QTimer::singleShot(4500, &app, [&] {
            VideoSurface* surface = dialog->findChild<VideoSurface*>();
            const auto generation = surface != nullptr ? surface->renderContextGeneration() : 0;
            std::fprintf(stderr, "     player aji surface generation=%llu\n",
                         static_cast<unsigned long long>(generation));
        });
    }

    QTimer::singleShot(5000, &app, [&] {
        auto* shell = dialog->findChild<PlayerShell*>();
        if (shell != nullptr) {
            emit shell->compareRequested();
        }
    });

    QTimer::singleShot(11000, &app, [&] {
        auto* compare = dialog->findChild<CompareWindow*>();
        check(compare != nullptr, "the compare row opened a window");
        if (compare == nullptr) {
            app.exit(1);
            return;
        }
        // Optional AJI candidate: exercises the pane's own AnimeJaNai core,
        // including the backend preload a second core needs.
        if (!qEnvironmentVariableIsEmpty("MIKAI_COMPARE_PROBE_AJI")) {
            compare->setCurrentCandidateId(qEnvironmentVariable("MIKAI_COMPARE_PROBE_AJI"));
            QElapsedTimer ajiWait;
            ajiWait.start();
            while (ajiWait.elapsed() < 420000 && compare->comparisonPending()) QTest::qWait(100);
            std::fprintf(stderr,
                         "     aji candidate=%s renderedA=%dx%d renderedB=%dx%d pending=%d\n",
                         qPrintable(qEnvironmentVariable("MIKAI_COMPARE_PROBE_AJI")),
                         compare->renderedSideA().width(), compare->renderedSideA().height(),
                         compare->renderedSideB().width(), compare->renderedSideB().height(),
                         compare->comparisonPending() ? 1 : 0);
            const QImage a = compare->renderedSideA().convertToFormat(QImage::Format_RGB888);
            const QImage b = compare->renderedSideB().convertToFormat(QImage::Format_RGB888);
            if (!a.isNull() && !b.isNull() && a.size() == b.size()) {
                qint64 diff = 0;
                qint64 total = 0;
                for (int y = 0; y < a.height(); y += 3) {
                    for (int x = 0; x < a.width(); x += 3) {
                        ++total;
                        if (a.pixel(x, y) != b.pixel(x, y)) ++diff;
                    }
                }
                std::fprintf(stderr, "     aji panes differ: %.1f%% of samples\n",
                             total > 0 ? 100.0 * double(diff) / double(total) : 0.0);
            }
            for (QLabel* label : compare->findChildren<QLabel*>()) {
                const QString text = label->text();
                if (text.contains(QStringLiteral("unavailable"), Qt::CaseInsensitive) ||
                    text.contains(QStringLiteral("libnvinfer"), Qt::CaseInsensitive)) {
                    std::fprintf(stderr, "     aji status: %s\n", qPrintable(text));
                }
            }
        }
        compare->resize(1280, 760);
        CompareCanvas* canvas = compare->findChild<CompareCanvas*>(QString::fromLatin1("compareCanvas"));
        check(canvas != nullptr, "the window has a canvas");
        if (canvas == nullptr) {
            app.exit(1);
            return;
        }

        const QImage image = canvas->grab().toImage().convertToFormat(QImage::Format_RGB888);
        image.save(QStringLiteral("compare_canvas.png"));
        std::fprintf(stderr, "     canvas %dx%d\n", image.width(), image.height());

        int varied = 0;
        int sampled = 0;
        for (int y = 0; y < image.height(); y += 4) {
            for (int x = 0; x < image.width(); x += 4) {
                ++sampled;
                const int lightness = image.pixelColor(x, y).lightness();
                if (lightness > 12 && lightness < 243) ++varied;
            }
        }
        const double share = sampled > 0 ? 100.0 * varied / sampled : 0.0;
        std::fprintf(stderr, "     canvas has picture: %.1f%% of samples in range\n", share);
        check(share > 20.0, "the canvas shows an actual picture, not a blank panel");

        // Optional second capture: a recapture runs the whole still path again on
        // a core that already carries the raw selector, which is where a stale
        // demuxer read the network source as pixels.
        if (!qEnvironmentVariableIsEmpty("MIKAI_COMPARE_PROBE_RECAPTURE")) {
            auto* recapture = compare->findChild<QPushButton*>(QStringLiteral("compareRecapture"));
            check(recapture != nullptr, "the compare window exposes its Recapture action");
            if (recapture != nullptr) {
                QTest::mouseClick(recapture, Qt::LeftButton);
                QElapsedTimer recaptureWait;
                recaptureWait.start();
                while (recaptureWait.elapsed() < 8000 && compare->comparisonPending())
                    QTest::qWait(50);
                const QImage again = canvas->grab().toImage().convertToFormat(QImage::Format_RGB888);
                again.save(QStringLiteral("compare_canvas_recapture.png"));
                int variedAgain = 0;
                int sampledAgain = 0;
                for (int y = 0; y < again.height(); y += 4) {
                    for (int x = 0; x < again.width(); x += 4) {
                        ++sampledAgain;
                        const int lightness = again.pixelColor(x, y).lightness();
                        if (lightness > 12 && lightness < 243) ++variedAgain;
                    }
                }
                const double shareAgain =
                    sampledAgain > 0 ? 100.0 * variedAgain / sampledAgain : 0.0;
                std::fprintf(stderr, "     recapture canvas has picture: %.1f%% of samples\n",
                             shareAgain);
                check(shareAgain > 20.0, "a recapture still shows a real picture");
            }
        }

        compare->grab().save(QStringLiteral("compare_shot.png"));
        std::fprintf(stderr, "     compare_shot.png saved from the visible window\n");

        // WM_SYSCOMMAND/SC_CLOSE, what the native title bar sends.
        QPointer<CompareWindow> compareGuard(compare);
        VideoSurface* surface = dialog->findChild<VideoSurface*>();
        const QImage frozenBefore = compare->frozenFrame();
        std::fprintf(stderr,
                     "     before close: visible=%d frozen=%dx%d playing=%d planLocked=%d\n",
                     compare->isVisible() ? 1 : 0, frozenBefore.width(), frozenBefore.height(),
                     surface != nullptr && surface->isPlaying() ? 1 : 0,
                     surface != nullptr && surface->nativePlanLocked() ? 1 : 0);
        check(compare->isVisible(), "the compare window is on screen before the close");
#ifdef Q_OS_WIN
        const HWND hwnd = reinterpret_cast<HWND>(compare->winId());
        if (hwnd == nullptr || !IsWindow(hwnd)) {
            check(false, "the compare window owns a native HWND to close");
            app.exit(1);
            return;
        }
        SendMessageW(hwnd, WM_SYSCOMMAND, SC_CLOSE, 0);
#else
        compare->close();
#endif
        QElapsedTimer afterClose;
        afterClose.start();
        while (afterClose.elapsed() < 2000) {
            const bool settled = (compareGuard.isNull() || !compareGuard->isVisible()) &&
                                 (compareGuard.isNull() || !compareGuard->playbackHeld()) &&
                                 (compareGuard.isNull() || !compareGuard->comparisonPending()) &&
                                 (surface == nullptr || !surface->nativePlanLocked());
            if (settled) break;
            QTest::qWait(50);
        }

        check(!compareGuard.isNull() && !compareGuard->isVisible(),
              "the native close command hid the Compare window");
        check(!compareGuard.isNull() && !compareGuard->playbackHeld(),
              "the close released the playback the frozen capture had paused");
        check(!compareGuard.isNull() && !compareGuard->comparisonPending(),
              "the close left no pending capture, build or release");
        check(surface != nullptr && !surface->nativePlanLocked(),
              "the close released the native plan lock the frozen capture held");
        check(!compareGuard.isNull() && compareGuard->frozenFrame().isNull() &&
                  compareGuard->renderedSideA().isNull() &&
                  compareGuard->renderedSideB().isNull(),
              "the native close clears the frozen source and both comparison outputs");
        if (!compareGuard.isNull()) {
            std::fprintf(stderr,
                         "     after close: visible=%d held=%d pending=%d planLocked=%d "
                         "frozenNull=%d outputsNull=%d\n",
                         compareGuard->isVisible() ? 1 : 0,
                         compareGuard->playbackHeld() ? 1 : 0,
                         compareGuard->comparisonPending() ? 1 : 0,
                         surface != nullptr && surface->nativePlanLocked() ? 1 : 0,
                         compareGuard->frozenFrame().isNull() ? 1 : 0,
                         compareGuard->renderedSideA().isNull() &&
                                 compareGuard->renderedSideB().isNull()
                             ? 1 : 0);
        }

        std::fprintf(stderr, "RESULT: %s\n", failures == 0 ? "PASS" : "FAIL");
        app.exit(failures == 0 ? 0 : 1);
    });
    return app.exec();
}
