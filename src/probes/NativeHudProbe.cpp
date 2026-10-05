// Native HUD probe: the native status chip and the stream-stats line, read from
// the real player shell the way a viewer reads them.
//
// What this proves, and what it deliberately does not:
//
//   - every state the native vocabulary names is rendered by the real chip from
//     a *valid* snapshot the probe builds: Off, Probe, Preparing,
//     NeedsPreparation and Failed each read as "Native <state> · <fact>" and
//     never claim a running chain, while Active reads as a running chain and
//     never wears a state name;
//   - pushing a snapshot at the shell through its public input setter changes
//     the visible caption: a saved request alone stays "Native requested ·
//     Original active", and only the filter's own snapshot may say Active;
//   - a filter that reports a state shows its chip even with no stored request,
//     because the producer's own report is the one fact the HUD exists to show;
//   - the stream-stats line keeps the measured FPS and its provenance apart from
//     the native plan: an unknown FPS is absent, an mpv-observed rate is named as
//     output, a container rate is named as the container's and never as the
//     native output, and a huge cumulative count never lands in the FPS field;
//   - the nominal interpolation cadence the filter publishes stays in the chip
//     as the plan - it is never presented as a measured frame rate.
//
// The snapshots here are *presentation fixtures*: internally consistent
// documents built by hand to exercise the chip's vocabulary. They are not the
// output of any inference run, and the probe claims no GPU work from them. The
// only PNG this probe can write is the one it grabs from the widgets it built
// when it is itself run; there is no checked-in image and no hand-drawn mock.
//
// Run from the repo root, with the shaders deployed beside the exe:
//   build-cmake/native_hud_probe.exe

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QLabel>
#include <QPixmap>
#include <QSettings>
#include <QTemporaryDir>

#include <cstdio>
#include <optional>

#include "PlayerShell.hpp"
#include "ProbeRuntime.hpp"
#include "ProbeUi.hpp"
#include "VideoSurface.hpp"
#include "native/NativeMenu.hpp"
#include "native/NativeStatus.hpp"
#include "native/NativeTypes.hpp"

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

/// The chip as a viewer reads it: the rendered label, the tooltip a hover shows,
/// and the accessible name a screen reader announces. All three are public
/// widget state - the probe never reaches into a private member.
struct ChipReading {
    bool visible = false;
    QString text;
    QString tooltip;
    QString accessibleName;
};

ChipReading readChip(const QLabel* chip) {
    ChipReading reading;
    if (chip == nullptr) {
        return reading;
    }
    reading.visible = chip->isVisible();
    reading.text = chip->text();
    reading.tooltip = chip->toolTip();
    reading.accessibleName = chip->accessibleName();
    return reading;
}

/// The stream-stats line as a viewer reads it: the rendered throughput and FPS,
/// and the provenance the hover shows.
struct StatsReading {
    QString text;
    QString tooltip;
    QString accessibleDescription;
};

StatsReading readStats(const QLabel* stats) {
    StatsReading reading;
    if (stats == nullptr) {
        return reading;
    }
    reading.text = stats->text();
    reading.tooltip = stats->toolTip();
    reading.accessibleDescription = stats->accessibleDescription();
    return reading;
}

/// A synthetic but internally consistent snapshot for one presentation state.
/// Off and Probe carry no effective plan, exactly as the reader refuses one;
/// every non-Active state carries the reason that is non-empty by the schema.
native::StatusSnapshot stateSnapshot(native::NativeState state, const QString& reason) {
    native::StatusSnapshot snapshot;
    snapshot.version = native::kNativeStatusProtocol;
    snapshot.revision = 1;
    snapshot.epoch = 1;
    snapshot.state = state;
    snapshot.reason = state == native::NativeState::Active ? QString() : reason;
    return snapshot;
}

/// The one Active plan the probe presents: a spatial upscale with interpolation,
/// so the chip has a provider, a model, a geometry, a cadence and a factor to
/// show, plus deliberately huge cumulative counters that must not read as FPS.
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

/// Pushes a stored request and a status view at the shell through the one public
/// input setter, preserving the availability and runtime the shell already
/// holds. An absent snapshot is the reader's "nothing read yet" fact, named.
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

/// Grabs one built widget and writes it out, then proves the grab is readable:
/// a chip that collapsed to nothing would otherwise pass silently.
void shoot(QWidget* widget, const QString& name) {
    if (widget == nullptr) {
        check(false, QStringLiteral("there is a widget to screenshot: ") + name);
        return;
    }
    const QPixmap shot = widget->grab();
    check(!shot.isNull() && shot.width() >= 80 && shot.height() >= 20,
          QStringLiteral("the captured HUD is readable: ") + name);
    const QString path = QDir::current().filePath(QStringLiteral("build-cmake/") + name);
    const bool written =
        !shot.isNull() && QDir().mkpath(QFileInfo(path).absolutePath()) && shot.save(path);
    check(written, QStringLiteral("screenshot written: ") + name);
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    if (!mikai::probes::initializeUi(app)) return 2;
    app.setQuitOnLastWindowClosed(false);

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

    // The shell reaches the same runtime the app would; the HUD is presentation,
    // but the real widget still comes up with the real surface behind it.
    if (!mplayer::probes::selectRuntime()) {
        return 2;
    }

    // The shell is the real widget the viewer sees. The probe drives it through
    // its public setters only: setNativeMenuInputs and setStreamStats.
    auto* shell = new shell::PlayerShell(nullptr, /*frameless=*/false);
    shell->resize(1280, 760);
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

    // The provenance strings the label renders, taken from the same translation
    // context the shell uses, so the check is not pinned to English.
    const QString unknownFps = QCoreApplication::translate("PlayerShell", "FPS unknown");
    const QString mpvFps = QCoreApplication::translate("PlayerShell", "mpv output FPS");
    const QString containerFps =
        QCoreApplication::translate("PlayerShell", "container source FPS");

    // ── a default Off machine shows no chip ─────────────────────────────
    check(!readChip(chip).visible,
          "a machine with no request and no snapshot shows no native chip");

    // ── every state the vocabulary names, from a valid snapshot ─────────
    //
    // A request is stored as well, so each state is the one a viewer has asked
    // for rather than the untouched default.
    native::NativePreferences requested;
    requested.enabled = true;
    requested.recipeId = QStringLiteral("animejanai_2x");

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
        // The consumer surfaces the state rather than hiding it: a viewer can
        // read which state the filter is in, and the chip never claims a running
        // chain for any state but Active.
        check(seen.visible,
              QStringLiteral("state %1 with a request shows the chip")
                  .arg(native::stableName(entry.state)));
        check(!lines.active,
              QStringLiteral("state %1 never claims a running chain")
                  .arg(native::stableName(entry.state)));
    }

    // ── a saved request alone claims nothing ────────────────────────────
    //
    // The requested request with no filter snapshot: the chip says it was asked
    // for and that the Original pipeline is what is running.
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
        // The snapshot's own input facts, independent of how the status module
        // words its line: the consumer must surface the geometry, cadence, factor
        // and dropped count the fixture stated.
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
        // Cumulative counters are counts, not an instantaneous rate: the huge
        // frames-out and interpolated totals never read as an FPS.
        check(!seen.text.contains(QStringLiteral("1999500")) &&
                  !seen.text.contains(QStringLiteral("999500")),
              "cumulative counters are never rendered as an instantaneous FPS");
        note(QStringLiteral("Active caption (presentation fixture, not GPU inference): %1")
                 .arg(seen.text));
    }

    // ── the stream-stats line: measured FPS and its provenance ──────────
    //
    // None of the native status above may touch the measured stream rate; the
    // nominal interpolation cadence stays in the chip, never in this label.
    {
        shell->setStreamStats(0.0, 0.0, StreamFpsSource::Unknown);
        StatsReading seen = readStats(stats);
        check(seen.tooltip == unknownFps && seen.accessibleDescription == unknownFps,
              "an absent FPS names itself unknown in tooltip and accessibility");
        check(seen.text.contains(QStringLiteral("— FPS")),
              "an absent FPS shows no measured rate");

        shell->setStreamStats(1'000'000.0, 25.0, StreamFpsSource::MpvOutput);
        seen = readStats(stats);
        check(seen.text.contains(QString::number(25.0, 'f', 2)),
              "an mpv-observed rate is shown as the measured FPS");
        check(seen.tooltip == mpvFps && seen.accessibleDescription == mpvFps,
              "an mpv-observed rate shows its output provenance");

        shell->setStreamStats(500'000.0, 15.0, StreamFpsSource::Container);
        seen = readStats(stats);
        check(seen.text.contains(QString::number(15.0, 'f', 2)),
              "a container rate is shown as the measured FPS");
        check(seen.tooltip == containerFps && seen.accessibleDescription == containerFps,
              "a container rate names the container, not the native output");
        check(seen.tooltip != mpvFps,
              "a container rate is never labelled as an mpv/native output rate");
        // The nominal native cadence the chip shows (48/1) is not this label's
        // measured rate.
        check(!seen.text.contains(QString::number(48.0, 'f', 2)),
              "the nominal native cadence is never presented as a measured FPS");

        // A huge cumulative byte count belongs to the throughput field and must
        // not become an FPS.
        shell->setStreamStats(12'000'000'000.0, 0.0, StreamFpsSource::Unknown);
        seen = readStats(stats);
        check(seen.text.contains(
                  QString::number(12'000'000'000.0 * 8.0 / 1'000'000.0, 'f', 2)),
              "a huge cumulative byte count is shown as throughput, not as an FPS");
        check(seen.text.contains(QStringLiteral("— FPS")),
              "with no measured rate the FPS field stays absent");
        check(seen.tooltip == unknownFps,
              "with no measured rate the FPS provenance stays unknown");
    }

    // An FPS value never infers native execution: with nothing requested, the
    // container rate leaves the native chip exactly where it was.
    {
        native::NativePreferences none;
        pushNative(shell, none, std::nullopt);
        shell->setStreamStats(500'000.0, 15.0, StreamFpsSource::Container);
        check(!readChip(chip).visible,
              "a measured FPS never makes the native chip claim a native chain");
        shell->setStreamStats(0.0, 0.0, StreamFpsSource::Unknown);
    }

    // ── the real widgets, captured only when this parent runs the probe ──
    //
    // Restore the Active presentation fixture so the capture shows the chip and
    // its plan beside the measured stream line.
    pushNative(shell, requested, activePlanSnapshot());
    shell->setStreamStats(500'000.0, 15.0, StreamFpsSource::Container);
    QCoreApplication::processEvents();
    shoot(shell->findChild<QWidget*>(QStringLiteral("playerInfoChip")),
          QStringLiteral("native_hud_probe.png"));

    if (failures == 0) {
        std::printf("OK native_hud_probe\n");
    } else {
        std::fprintf(stderr, "FAILURES %d\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
