// Synthetic status snapshots exercise presentation only, not inference.
// --framebuffer-capture checks fullscreen GPU chrome, not desktop presentation.

#include <QApplication>
#include <QColor>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QLabel>
#include <QPixmap>
#include <QScreen>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>
#include <QWindow>

#include <cstdio>
#include <optional>

#include "PlayerShell.hpp"
#include "ProbeRuntime.hpp"
#include "ProbeUi.hpp"
#include "VideoSurface.hpp"
#include "native/status/NativeMenu.hpp"
#include "native/status/NativeStatus.hpp"
#include "native/status/NativeTypes.hpp"

using namespace mplayer;
using namespace mplayer::shell;

namespace {

int failures = 0;

void check(bool ok, const QString& what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", qPrintable(what));
    if (!ok) {
        ++failures;
    }
}

void note(const QString& what) { std::printf("NOTE %s\n", qPrintable(what)); }

struct ChipReading {
    bool visible = false;
    QString text;
};

ChipReading readChip(const QLabel* chip) {
    ChipReading reading;
    if (chip == nullptr) {
        return reading;
    }
    reading.visible = chip->isVisible();
    reading.text = chip->text();
    return reading;
}

native::StatusSnapshot stateSnapshot(native::NativeState state, const QString& reason) {
    native::StatusSnapshot snapshot;
    snapshot.version = native::kNativeStatusProtocol;
    snapshot.revision = 1;
    snapshot.epoch = 1;
    snapshot.state = state;
    snapshot.reason = state == native::NativeState::Active ? QString() : reason;
    return snapshot;
}

native::StatusSnapshot activePlanSnapshot() {
    using namespace native;
    StatusSnapshot snapshot = stateSnapshot(NativeState::Active, QString());

    NativeEffectivePlan plan;
    plan.recipe = QStringLiteral("animejanai_2x");
    plan.provider = QStringLiteral("TensorRT");
    plan.model = QStringLiteral("2x_AnimeJaNai_HD_V3");
    plan.spatialStages = 1;
    plan.order = QStringLiteral("spatial-first");
    plan.sourceSize = NativeSize{320, 180};
    plan.outputSize = NativeSize{640, 360};
    plan.sourceFps = Rational{24, 1};
    plan.outputFps = Rational{48, 1};
    plan.interpolation = NativeInterpolation{Rational{2, 1}, QStringLiteral("rife_v4.14")};
    plan.decoder = QStringLiteral("nvdec");
    plan.csp = QStringLiteral("bt709");
    snapshot.effective = plan;

    NativeCadenceCounters counters;
    counters.framesIn = 1000000;
    counters.framesOut = 1999500;
    counters.dropped = 1234;
    counters.interpolated = 999500;
    counters.sceneCuts = 3;
    snapshot.counters = counters;
    return snapshot;
}

void pushNative(PlayerShell* shell, const native::NativePreferences& preferences,
                const std::optional<native::StatusSnapshot>& snapshot) {
    native::menu::Inputs inputs = shell->nativeInputs();
    inputs.preferences = preferences;
    inputs.status = native::NativeStatusView{};
    if (snapshot.has_value()) {
        inputs.status.snapshot = *snapshot;
        inputs.status.availability = native::statusSnapshotAvailability(*snapshot);
    } else {
        inputs.status.availability = native::unavailableAvailability(
            QStringLiteral("native.statusAbsent"),
            QStringLiteral("No native status has been read."));
    }
    shell->setNativeMenuInputs(inputs);
    QCoreApplication::processEvents();
}

/// What a crop carries: the bright pixels the chip's text is drawn with, how many
/// columns they span, and how much of the crop is the dark composition behind it.
struct CropReading {
    long total = 0;
    long bright = 0;
    long brightColumns = 0;
    long dark = 0;

    double darkFraction() const { return total > 0 ? double(dark) / double(total) : 0.0; }
};

CropReading readCrop(const QImage& crop) {
    CropReading reading;
    if (crop.isNull()) return reading;
    for (int x = 0; x < crop.width(); ++x) {
        bool columnCarriesText = false;
        for (int y = 0; y < crop.height(); ++y) {
            const QColor pixel = crop.pixelColor(x, y);
            const int sum = pixel.red() + pixel.green() + pixel.blue();
            ++reading.total;
            // The chip draws white(255)/white(185)/#ffcf7a/#79dcf2 text on the
            // 32,34,37 pill: 480 is glyph ink, 150 is that pill over the picture.
            if (sum > 480) {
                ++reading.bright;
                columnCarriesText = true;
            } else if (sum < 150) {
                ++reading.dark;
            }
        }
        if (columnCarriesText) ++reading.brightColumns;
    }
    return reading;
}

/// The pixels a viewer actually sees over `widget`. The adopted chrome is painted
/// into the surface's framebuffer, so the widget's own backing store is empty and
/// widget->grab() would produce a blank image of exactly the right size.
QImage presentedCrop(QWidget* widget) {
    if (widget == nullptr) return {};
    QWidget* window = widget->window();
    QWindow* handle = window != nullptr ? window->windowHandle() : nullptr;
    if (handle == nullptr) return {};
    QScreen* screen = handle->screen();
    if (screen == nullptr) screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) return {};
    QPoint origin = widget->mapToGlobal(QPoint(0, 0));
#ifdef Q_OS_WIN
    origin -= screen->geometry().topLeft();
#endif
    return screen->grabWindow(0, origin.x(), origin.y(), widget->width(), widget->height())
        .toImage();
}

/// Crops the presented pixels over one built widget, writes them out, and proves
/// the crop carries the HUD's text: a blank or uncomposed crop has the right size
/// and no glyphs, which is what the old size-only readout could not tell apart.
void shoot(PlayerShell* shell, QWidget* widget, const QString& name, bool framebufferCapture) {
    if (widget == nullptr) {
        check(false, QStringLiteral("there is a widget to screenshot: ") + name);
        return;
    }
    QImage crop;
    if (framebufferCapture) {
        VideoSurface* surface = shell->videoSurface();
        const bool ready = surface != nullptr && surface->waitForContext(5000);
        check(ready, QStringLiteral("the GPU chrome surface has a render context"));
        if (!ready) return;
        const QImage full = surface->grabFramebuffer();
        const qreal ratio = surface->devicePixelRatioF();
        const QPoint at = widget->mapTo(shell, QPoint()) - surface->pos();
        const QRect deviceRect(at * ratio, widget->size() * ratio);
        std::fprintf(stderr,
                     "GPU crop: shell %dx%d surface %d,%d %dx%d framebuffer %dx%d chip %d,%d %dx%d dpr=%.2f\n",
                     shell->width(), shell->height(), surface->x(), surface->y(),
                     surface->width(), surface->height(), full.width(), full.height(),
                     deviceRect.x(), deviceRect.y(), deviceRect.width(), deviceRect.height(), ratio);
        const bool contained = !full.isNull() && full.rect().contains(deviceRect);
        check(contained, QStringLiteral("the full HUD chip lies inside the fullscreen framebuffer"));
        if (!contained) return;
        crop = full.copy(deviceRect);
        const QString fullPath = QDir::current().filePath(
            QStringLiteral("build-cmake/native_hud_probe_fbo_full.png"));
        check(QDir().mkpath(QFileInfo(fullPath).absolutePath()) && full.save(fullPath),
              QStringLiteral("the fullscreen GPU chrome evidence was saved"));
    } else {
        if (QWidget* window = widget->window()) {
            window->raise();
            window->activateWindow();
        }
        QCoreApplication::processEvents();
        QThread::msleep(250);
        QCoreApplication::processEvents();
        crop = presentedCrop(widget);
    }
    const QPoint origin = widget->mapToGlobal(QPoint(0, 0));
    std::fprintf(stderr,
                 "capture %s: widget %dx%d at %d,%d visible=%d crop %dx%d dpr=%.2f\n",
                 qPrintable(name), widget->width(), widget->height(), origin.x(), origin.y(),
                 int(widget->isVisible()), crop.width(), crop.height(),
                 widget->devicePixelRatioF());

    const QString path = QDir::current().filePath(QStringLiteral("build-cmake/") + name);
    const bool written =
        !crop.isNull() && QDir().mkpath(QFileInfo(path).absolutePath()) && crop.save(path);
    check(written, QStringLiteral("screenshot written: ") + name);

    const CropReading reading = readCrop(crop);
    check(reading.bright >= 120 && reading.brightColumns >= 40,
          QStringLiteral("the captured HUD shows its text: %1 bright pixels over %2 columns")
              .arg(reading.bright)
              .arg(reading.brightColumns));
    check(reading.darkFraction() >= 0.25,
          QStringLiteral("the captured HUD sits on the dark composition (%1% of the crop is the "
                         "pill or darker)")
              .arg(100.0 * reading.darkFraction(), 0, 'f', 1));
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    if (!mikai::probes::initializeUi(app)) return 2;
    app.setQuitOnLastWindowClosed(false);
    const bool framebufferCapture =
        app.arguments().contains(QStringLiteral("--framebuffer-capture"));
    std::fprintf(stderr, "capture mode: %s\n",
                 framebufferCapture ? "fullscreen GPU framebuffer (not presented screen)"
                                    : "presented screen");

    QTemporaryDir settingsDir;
    if (!settingsDir.isValid()) {
        std::fprintf(stderr, "FAIL cannot create an isolated settings directory\n");
        return 1;
    }
    QCoreApplication::setOrganizationName(QStringLiteral("MikaiNativeHudProbe"));
    QCoreApplication::setApplicationName(QStringLiteral("NativeHudProbe"));
    // The user's own player state stays theirs: every read and write below lands
    // in a temporary directory that dies with the process.
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    QSettings settings;
    settings.clear();
    settings.sync();

    if (!mplayer::probes::selectRuntime()) {
        return 2;
    }

    auto* shell = new shell::PlayerShell(nullptr, /*frameless=*/false);
    shell->resize(1280, 760);
    if (framebufferCapture) {
        shell->setFixedSize(1280, 760);
        shell->enterFullscreen();
    }
    shell->show();
    QCoreApplication::processEvents();
    shell->wake();
    QCoreApplication::processEvents();

    auto* chip = shell->findChild<QLabel*>(QStringLiteral("playerNativeStatus"));
    auto* stats = shell->findChild<QLabel*>(QStringLiteral("playerNetworkStats"));
    check(chip != nullptr, "the shell exposes the native status chip");
    check(stats != nullptr, "the shell exposes the stream-stats label");
    if (chip == nullptr || stats == nullptr) {
        std::fprintf(stderr, "FAIL no HUD widget to read\n");
        return 2;
    }

    check(!readChip(chip).visible,
          "a machine with no request and no snapshot shows no native chip");

    native::NativePreferences requested;
    requested.enabled = true;
    requested.recipeId = QStringLiteral("animejanai_2x");

    auto inputs = shell->nativeInputs();
    inputs.runtime.loaded = true;
    inputs.runtime.kind = native::BundleKind::Standard;
    shell->setNativeMenuInputs(inputs);
    pushNative(shell, requested, activePlanSnapshot());
    check(!readChip(chip).visible,
          "a Standard runtime hides private Native state even with an Active snapshot");

    inputs = shell->nativeInputs();
    inputs.runtime.kind = native::BundleKind::Native;
    shell->setNativeMenuInputs(inputs);
    pushNative(shell, native::NativePreferences{}, std::nullopt);
    check(!readChip(chip).visible,
          "a Native presentation without a request or snapshot shows no chip");

    struct StateCase {
        native::NativeState state;
        QString reason;
    };
    const QVector<StateCase> stateCases = {
        {native::NativeState::Off,
         QStringLiteral("The filter is attached, but native processing is off.")},
        {native::NativeState::Probe,
         QStringLiteral("Probe-only passthrough: real decoder device observed; no engine, "
                        "no inference.")},
        {native::NativeState::Preparing,
         QStringLiteral("An engine is being prepared for the requested chain.")},
        {native::NativeState::NeedsPreparation,
         QStringLiteral("No prepared artifact matches the requested chain yet.")},
        {native::NativeState::Failed,
         QStringLiteral("The filter reported a failure while running the requested chain.")},
    };

    for (const StateCase& entry : stateCases) {
        native::StatusSnapshot snapshot = stateSnapshot(entry.state, entry.reason);
        QString key;
        QString text;
        check(native::statusSnapshotIsConsistent(snapshot, &key, &text),
              QStringLiteral("the %1 presentation fixture is a consistent snapshot")
                  .arg(native::stableName(entry.state)));
        pushNative(shell, requested, snapshot);

        const native::menu::StatusLines lines =
            native::menu::statusLines(shell->nativeInputs());
        const ChipReading seen = readChip(chip);
        check(seen.visible,
              QStringLiteral("state %1 with a request shows the chip")
                  .arg(native::stableName(entry.state)));
        check(!lines.active,
              QStringLiteral("state %1 never claims a running chain")
                  .arg(native::stableName(entry.state)));
    }

    pushNative(shell, requested, std::nullopt);
    const QString requestedText = readChip(chip).text;
    {
        const native::menu::StatusLines lines =
            native::menu::statusLines(shell->nativeInputs());
        const ChipReading seen = readChip(chip);
        check(seen.visible, "a stored request shows the chip without any snapshot");
        check(!lines.active, "a stored request alone never claims the chain is active");
    }
    note(QStringLiteral("requested-only caption: %1").arg(requestedText));

    // A filter that reports a state of its own shows the chip even with nothing
    // stored: the producer's report is the fact the chip exists to show.
    {
        native::NativePreferences none;
        pushNative(shell, none, activePlanSnapshot());
        const native::menu::StatusLines lines =
            native::menu::statusLines(shell->nativeInputs());
        check(readChip(chip).visible,
              "the filter's own report shows the chip with no stored request");
        check(lines.active, "an Active snapshot from the filter reads as active");
    }

    // ── the caller's Active snapshot changes the visible caption ────────
    //
    // The same stored request, now with a valid Active snapshot behind it: the
    // caption must change from "requested" to a running chain. This snapshot is
    // a presentation fixture; it proves the HUD renders a report, not that any
    // GPU inference ran.
    {
        pushNative(shell, requested, activePlanSnapshot());
        const ChipReading seen = readChip(chip);
        const native::menu::StatusLines lines =
            native::menu::statusLines(shell->nativeInputs());

        check(seen.visible && lines.active,
              "the caller's Active snapshot makes the chip read as a running chain");
        check(seen.text != requestedText,
              "the caller's Active snapshot changes the visible caption");
        check(seen.text.contains(QStringLiteral("320x180")) &&
                  seen.text.contains(QStringLiteral("640x360")),
              "the chip surfaces the snapshot's source and output geometry");
        check(seen.text.contains(QStringLiteral("24/1")) &&
                  seen.text.contains(QStringLiteral("48/1")),
              "the chip surfaces the snapshot's nominal source-to-output cadence");
        check(seen.text.contains(QStringLiteral("2/1")),
              "the chip surfaces the snapshot's interpolation factor");
        check(seen.text.contains(QStringLiteral("1234")),
              "the chip surfaces the snapshot's cumulative dropped count");
        check(!seen.text.contains(QStringLiteral("1999500")) &&
                  !seen.text.contains(QStringLiteral("999500")),
              "cumulative counters are never rendered as an instantaneous FPS");
        note(QStringLiteral("Active caption (presentation fixture, not GPU inference): %1")
                 .arg(seen.text));
    }

    {
        shell->setStreamStats(1'000'000.0, 25.0, StreamFpsSource::MpvOutput);
        check(stats->text().contains(QString::number(25.0, 'f', 2)),
              "an mpv-observed rate is shown as the measured FPS");

        shell->setStreamStats(500'000.0, 15.0, StreamFpsSource::Container);
        check(stats->text().contains(QString::number(15.0, 'f', 2)),
              "a container rate is shown as the measured FPS");
        check(!stats->text().contains(QString::number(48.0, 'f', 2)),
              "the nominal native cadence is never presented as a measured FPS");

        shell->setStreamStats(12'000'000'000.0, 0.0, StreamFpsSource::Unknown);
        check(stats->text().contains(
                  QString::number(12'000'000'000.0 * 8.0 / 1'000'000.0, 'f', 2)),
              "a huge cumulative byte count is shown as throughput, not as an FPS");
    }

    {
        native::NativePreferences none;
        pushNative(shell, none, std::nullopt);
        shell->setStreamStats(500'000.0, 15.0, StreamFpsSource::Container);
        check(!readChip(chip).visible,
              "a measured FPS never makes the native chip claim a native chain");
        shell->setStreamStats(0.0, 0.0, StreamFpsSource::Unknown);
    }

    pushNative(shell, requested, activePlanSnapshot());
    shell->setStreamStats(500'000.0, 15.0, StreamFpsSource::Container);
    QCoreApplication::processEvents();
    shoot(shell, shell->findChild<QWidget*>(QStringLiteral("playerInfoChip")),
          framebufferCapture ? QStringLiteral("native_hud_probe_fbo.png")
                             : QStringLiteral("native_hud_probe.png"),
          framebufferCapture);

    if (failures == 0) {
        std::printf("OK native_hud_probe\n");
    } else {
        std::fprintf(stderr, "FAILURES %d\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
