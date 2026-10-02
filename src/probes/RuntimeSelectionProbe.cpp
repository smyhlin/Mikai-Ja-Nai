// What one startup would select, printed instead of only asserted inside the
// app: the stored choice, the descriptor it resolved to, and whether a module
// is actually loaded. One module per process, so each scenario is its own run.
//
// The probe never searches for a runtime of its own and never edits PATH; the
// shared selection entrypoint does all of it, which is the behaviour under
// test.
//
// Usage: runtime_selection_probe [standard|native|corrupt]
//   standard  (default) the stored choice is cleared; Standard is selected.
//   native    the stored choice is native; a missing native bundle must be
//             refused by name, with Standard carrying playback.
//   corrupt   the stored value is corrupt; it must read back as Standard.
//
// Exit code: 0 PASS, 1 FAIL, 2 SKIP -- the machine has no mpv runtime to
// select, and a probe never starts a download.

#include "MpvApi.hpp"
#include "native/NativeRuntimeSelection.hpp"

#include <backend/Runtime.hpp>

#include <QCoreApplication>
#include <QSettings>
#include <QStringList>
#include <QTemporaryDir>

#include <cstdio>

using mplayer::native::BundleKind;
using mplayer::native::RuntimeChoice;
using mplayer::native::RuntimeSelection;

namespace {

int failures = 0;

void check(bool ok, const QString& what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.toUtf8().constData());
    if (!ok) {
        ++failures;
    }
}

void dump(const RuntimeSelection& selection) {
    std::printf("selection: loaded=%d kind=%s requested=%s api=%llu nativeNotApplied=%d "
                "runtimeUnavailable=%d restartRequired=%d\n",
                selection.loaded ? 1 : 0,
                qPrintable(mplayer::native::bundleKindName(selection.kind)),
                qPrintable(mplayer::native::runtimeChoiceToString(selection.requested)),
                static_cast<unsigned long long>(selection.apiVersion),
                selection.nativeNotApplied ? 1 : 0, selection.runtimeUnavailable ? 1 : 0,
                selection.restartRequired ? 1 : 0);
    std::printf("root=%s\nmodule=%s\nreason=%s\n", qPrintable(selection.root),
                qPrintable(selection.modulePath), qPrintable(selection.reason));
}

/// The invariants every selection must satisfy, whatever it selected. They are
/// the "never a silent black player" rule, stated once.
void checkInvariants(const RuntimeSelection& selection) {
    check(selection.loaded || !selection.reason.isEmpty(),
          QStringLiteral("a selection that loaded nothing still names why"));
    check(!selection.nativeNotApplied || !selection.reason.isEmpty(),
          QStringLiteral("a refused native request names why"));
    check(!selection.restartRequired || !selection.reason.isEmpty(),
          QStringLiteral("a restart requirement says so"));
    check(!selection.loaded || selection.apiVersion > 0,
          QStringLiteral("a loaded module reports the API version it was loaded against"));
    // Selection carries the descriptor it validated, not a rebuilt shell of it.
    check(!selection.loaded || selection.resolvedBundle.has_value(),
          QStringLiteral("a loaded selection carries the bundle it validated"));
    check(!selection.resolvedBundle.has_value() ||
              selection.resolvedBundle->modulePath == selection.modulePath,
          QStringLiteral("the display module is the one the resolved closure named"));
}

/// The loader was handed the resolved closure itself: the same module path and
/// the same dependency directories the descriptor carried. A descriptor rebuilt
/// from the display fields would lose the closure here, and native playback
/// would fail at load rather than at selection.
void checkLoadedMatchesClosure(const RuntimeSelection& selection) {
    if (!selection.loaded || !selection.resolvedBundle.has_value()) {
        return;
    }
    const mplayer::MpvBundle loaded = mplayer::MpvApi::instance().bundle();
    check(loaded.modulePath == selection.modulePath,
          QStringLiteral("the loader was given the resolved module"));
    check(loaded.descriptorId == selection.resolvedBundle->descriptorId,
          QStringLiteral("the loader was given the resolved descriptor identity"));
    check(loaded.deps == selection.resolvedBundle->toMpvBundleDescriptor().deps,
          QStringLiteral("the loader was given the resolved dependency closure"));
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("MikaiRuntimeProbe"));
    QCoreApplication::setApplicationName(QStringLiteral("RuntimeSelectionProbe"));

    // The selection reads QSettings before it loads anything, so the scope has
    // to die with this process: a probe must not inherit what a real session
    // left behind, and must not leave anything behind itself.
    QTemporaryDir settingsDir;
    if (!settingsDir.isValid()) {
        std::fprintf(stderr, "FAIL cannot create an isolated settings directory\n");
        return 1;
    }
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settingsDir.path());

    const QStringList arguments = QCoreApplication::arguments();
    const QString scenario = arguments.size() > 1 ? arguments.at(1) : QStringLiteral("standard");
    std::printf("scenario=%s\n", qPrintable(scenario));

    QString stored = QStringLiteral("standard");
    if (scenario == QLatin1String("native")) {
        stored = QStringLiteral("native");
    } else if (scenario == QLatin1String("corrupt")) {
        stored = QStringLiteral("not-a-runtime");
    } else if (scenario != QLatin1String("standard")) {
        std::fprintf(stderr, "FAIL unknown scenario %s\n", scenario.toUtf8().constData());
        return 1;
    }
    {
        QSettings settings;
        settings.beginGroup(mplayer::native::runtimeChoiceSettingsGroup());
        settings.setValue(mplayer::native::runtimeChoiceSettingsKey(), stored);
        settings.endGroup();
        settings.sync();
    }
    std::printf("stored=%s readback=%s\n", qPrintable(stored),
                qPrintable(mplayer::native::runtimeChoiceToString(
                    mplayer::native::storedRuntimeChoice())));

    // Every scenario ends in a Standard fallback when native is refused, so a
    // machine with no runtime at all is skipped rather than made to download
    // one: the probe reports the selection, it does not provision.
    if (!mikai::backend::findRuntime().has_value()) {
        std::printf("RESULT: SKIP (no mpv runtime is present; a probe must not download one)\n");
        return 2;
    }

    const RuntimeSelection selection = mplayer::native::selectRuntime();
    dump(selection);
    checkInvariants(selection);
    checkLoadedMatchesClosure(selection);

    if (scenario == QLatin1String("corrupt")) {
        check(mplayer::native::storedRuntimeChoice() == RuntimeChoice::Standard,
              QStringLiteral("a corrupt stored value reads back as Standard"));
        check(selection.loaded && selection.kind == BundleKind::Standard,
              QStringLiteral("a corrupt preference still selects Standard"));
    } else if (scenario == QLatin1String("native")) {
        // Either the bundle really loaded, or the refusal is named and Standard
        // carries playback. What must never happen is a native claim with no
        // loaded module behind it.
        const bool nativeLoaded = selection.loaded && selection.kind == BundleKind::Native;
        const bool refusedByName = selection.nativeNotApplied && !selection.reason.isEmpty() &&
                                   selection.runtimeUnavailable;
        check(nativeLoaded || refusedByName,
              QStringLiteral("a native request is either applied or refused by name"));
        check(!nativeLoaded || !selection.nativeNotApplied,
              QStringLiteral("an applied native request is not reported as refused"));
        check(nativeLoaded || !selection.loaded || selection.kind == BundleKind::Standard,
              QStringLiteral("a refused native request falls back to Standard, not native"));
    } else {
        check(selection.loaded && selection.kind == BundleKind::Standard,
              QStringLiteral("Standard is selected and loaded"));
        check(!selection.nativeNotApplied, QStringLiteral("nothing native was claimed"));
    }

    // One module per process: the choice whose kind differs from the loaded one
    // can only ask for a restart, and the loaded module must survive the ask.
    if (selection.loaded) {
        const RuntimeChoice other = selection.kind == BundleKind::Native ? RuntimeChoice::Standard
                                                                        : RuntimeChoice::Native;
        const RuntimeSelection restart = mplayer::native::selectRuntime(other);
        dump(restart);
        checkInvariants(restart);
        checkLoadedMatchesClosure(restart);
        check(restart.loaded && restart.kind == selection.kind,
              QStringLiteral("a different choice never hot-swaps the pinned module"));
        check(restart.restartRequired && !restart.reason.isEmpty(),
              QStringLiteral("a different choice reports that it needs a restart"));
    }

    std::printf("RESULT: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
