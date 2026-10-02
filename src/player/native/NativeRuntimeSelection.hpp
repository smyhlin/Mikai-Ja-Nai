// Which mpv module this process runs on, decided once, before any core exists.
//
// The choice is additive under the `player/native` settings group; nothing else
// in the saved settings is touched. Standard keeps using the existing backend
// discovery/provision, so the only thing that changed is that it now ends in a
// `MpvApi::load` instead of a `PATH` edit.
//
// One module per process. A successful load pins it; every later call reports
// the recorded selection and asks for a restart instead of swapping. A native
// bundle that is missing, incomplete or corrupt is rejected *before* the loader
// is asked, so the Standard fallback is a first load rather than a hot swap.

#pragma once

#include <native/NativeBundle.hpp>

#include <optional>

#include <QtCore/QMetaType>
#include <QtCore/QString>

namespace mplayer {
namespace native {

/// The runtime the user picked. Absent, unrecognised or corrupt stored values
/// read as Standard: the fallback must be the answer when the preference is not.
enum class RuntimeChoice { Standard, Native };

QString runtimeChoiceSettingsGroup();
QString runtimeChoiceSettingsKey();
QString runtimeChoiceToString(RuntimeChoice choice);
RuntimeChoice runtimeChoiceFromString(const QString& stored);

/// The stored choice. Reads QSettings on every call, so a test that rewrites the
/// key is seen immediately.
RuntimeChoice storedRuntimeChoice();

/// Writes the choice. Returns false with a reason when the value cannot be
/// stored; the process keeps running on what it already selected either way.
bool storeRuntimeChoice(RuntimeChoice choice, QString* reason = nullptr);

/// What selection produced. Every field here is observed: the kind and version
/// come from the descriptor that was validated, the API version from the loaded
/// module. No capability or native state is inferred from a file label.
struct RuntimeSelection {
    /// A module is loaded and pinned.
    bool loaded = false;
    /// The kind of the module that is loaded.
    BundleKind kind = BundleKind::Standard;
    /// The kind the user asked for, kept when the fallback changed it.
    RuntimeChoice requested = RuntimeChoice::Standard;
    QString root;
    QString modulePath;
    QString descriptorId;
    QString version;
    quint64 apiVersion = 0;

    /// The validated bundle this selection resolved, carried intact so
    /// `commitRuntime` can prove the bytes still match and hand the loader
    /// exactly the descriptor that was validated - dependency directories and
    /// protocol included. Empty when nothing was resolved. It is never
    /// reconstructed from the display fields above, because those are a lossy
    /// view of the descriptor and dropping the closure would silently change
    /// what gets loaded.
    std::optional<Bundle> resolvedBundle;

    /// The runtime the process was asked for could not be provided, and
    /// `reason` names it. It survives a Standard fallback, because the *native*
    /// runtime is what is unavailable there, not playback: `loaded` is the only
    /// field that says whether a module exists.
    bool runtimeUnavailable = false;
    /// Native was asked for and refused, so the loaded kind is Standard (or
    /// nothing at all). Never set from a descriptor's label alone.
    bool nativeNotApplied = false;
    /// A module is already pinned and this call asked for a different one.
    bool restartRequired = false;
    /// Named reason, always set when `loaded` is false or `restartRequired`.
    QString reason;
};

/// Selects and loads, resolving the stored choice. One module per process: a
/// second call reports the recorded selection instead of loading again.
RuntimeSelection selectRuntime();

/// As above, for an explicit choice. Native that cannot be resolved falls back
/// to Standard as this process's first and only module, with the native reason
/// recorded alongside it.
///
/// This never provisions: it answers from what is already installed, so it can
/// run on the GUI thread without blocking. `resolveRuntime()` is the half that
/// may download, and the recovery path runs that on the pool.
RuntimeSelection selectRuntime(RuntimeChoice choice);

/// Explicit Standard selection for probe and test entrypoints, which set up
/// isolated settings and then need the same module the app would pick.
RuntimeSelection selectStandardRuntime();

/// The startup entrypoint: selects the stored choice from what is already
/// installed, without provisioning and without touching the network. `main()`
/// calls this before the first core, so a machine with nothing installed gets a
/// named reason instead of a download that blocks startup; the first play runs
/// the provisioning attempt on the pool through `resolveRuntime()`.
RuntimeSelection selectStartupRuntime();

/// Resolves a choice to a validated descriptor without loading anything. This
/// is the half that may download, so it blocks and belongs off the GUI thread.
RuntimeSelection resolveRuntime(RuntimeChoice choice);

/// Loads a resolved descriptor. Runs on the GUI thread and, once it succeeds,
/// pins the module for the rest of the process.
RuntimeSelection commitRuntime(const RuntimeSelection& resolved);

/// The recorded selection. Empty until something has been selected.
const RuntimeSelection& currentSelection();

/// True once a module is pinned. Recovery must not run when it is true: that is
/// what keeps a retry from becoming a second load.
bool runtimeSelected();

/// One line naming the runtime that is loaded and, when it is not the one that
/// was asked for, why. Safe to show to a user.
QString runtimeStatusText();

/// The single line a player shows when the runtime could not be produced at
/// all. Shared by the title page and the standalone player so the same failure
/// reads the same way wherever it surfaces.
QString runtimeUnavailablePlaybackText(const QString& reason);

/// Guards one recovery attempt per process, so two players asking for a runtime
/// at once produce one provisioning run and one retry. `beginRecovery()` returns
/// false while an attempt is in flight or a module is already pinned: recovery
/// exists to select, never to reload.
bool beginRecovery();
void endRecovery();
bool recoveryInFlight();

}  // namespace native
}  // namespace mplayer

// The recovery path hands this across the app's worker pool as a QVariant, so
// it has to be a registered type rather than an opaque blob.
Q_DECLARE_METATYPE(mplayer::native::RuntimeSelection)