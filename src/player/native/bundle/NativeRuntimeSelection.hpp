// Which mpv module this process runs on, decided once, before any core exists.
//
// The choice is additive under the `player/native` settings group; nothing else
// in the saved settings is touched. Standard keeps the existing backend
// discovery of the mpv modules shipped beside the executable; nothing here
// provisions, downloads or caches a runtime, so discovery now ends in a
// `MpvApi::load` instead of a `PATH` edit.
//
// One module per process. A successful load pins it; every later call reports
// the recorded selection and asks for a restart instead of swapping. A native
// bundle that is missing, incomplete or corrupt is rejected *before* the loader
// is asked, so the Standard fallback is a first load rather than a hot swap.

#pragma once

#include <native/bundle/NativeBundle.hpp>

#include <optional>

#include <QtCore/QMetaType>
#include <QtCore/QString>

#include <functional>

class QObject;

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
/// The write is the whole of what happens here. A stored choice is applied on
/// the next launch and never to the module this process already pinned, so a
/// user who switches it gets exactly what `restartRequiredFor()` reports and no
/// hot swap behind their back.
bool storeRuntimeChoice(RuntimeChoice choice, QString* reason = nullptr);

/// True when storing `choice` cannot take effect until the app restarts, because
/// a module of the other kind is already pinned. The settings surface asks this
/// to say "restart required" while the dialog is still open, rather than
/// discovering it from a swap that never happens.
///
/// False when nothing is pinned yet: the next selection reads whatever was stored.
bool restartRequiredFor(RuntimeChoice choice);

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
/// Discovery is offline; `hint` precedes the executable-adjacent runtime.
RuntimeSelection selectRuntime(RuntimeChoice choice, const QString& hint = {});

/// Explicit Standard selection for probe and test entrypoints, which set up
/// isolated settings and then need the same module the app would pick.
RuntimeSelection selectStandardRuntime(const QString& hint = {});

/// Startup pins the installed module before any core exists; absence stays a named failure.
RuntimeSelection selectStartupRuntime(const QString& hint = {});

/// Resolution validates installed bytes but neither loads nor downloads a module.
RuntimeSelection resolveRuntime(RuntimeChoice choice, const QString& hint = {});

/// Loads a resolved descriptor. Runs on the GUI thread and, once it succeeds,
/// pins the module for the rest of the process.
RuntimeSelection commitRuntime(const RuntimeSelection& resolved);

/// The recorded selection. Empty until something has been selected.
const RuntimeSelection& currentSelection();

/// True only for a loaded Standard selection whose validated module inventory,
/// canonical path, and actual mpv API version identify the pinned AnimeJaNai fork.
bool isAnimeJanaiForkRuntime(const RuntimeSelection& selection);

/// True once a module is pinned. Recovery must not run when it is true: that is
/// what keeps a retry from becoming a second load.
bool runtimeSelected();

/// True while one runtime resolution is running. Startup resolves on a worker
/// thread, so this is the question a playback request has to ask before it
/// reports anything: a resolution that is still hashing is not a missing
/// runtime, and claiming a module is absent one turn before it loads is the
/// failure this state exists to prevent.
bool runtimeResolutionPending();

/// The one line a surface shows while the runtime is still being resolved. It
/// names the work in progress, never a module that is missing, and it is what
/// `runtimeStatusText` and `runtimeUnavailablePlaybackText` fall back to while
/// a resolution is in flight.
QString runtimePendingPlaybackText();

/// Runs `callback` on the GUI thread once the in-flight resolution has
/// published its terminal state: a module is pinned, or a named failure is on
/// record. `context` owns nothing and is never retained; a registration whose
/// context has been destroyed is discarded rather than delivered, so a caller
/// that closes mid-resolution cannot be handed a callback it no longer wants.
///
/// A registration that arrives while nothing is in flight is queued rather
/// than called, so a caller never re-enters itself from inside the request
/// that registered it. `callback` is invoked exactly once; a caller that wants
/// to be told again registers again.
void whenRuntimeResolutionFinishes(QObject* context, const std::function<void()>& callback);

/// The owner of the one resolution a process may run at a time. Only
/// `beginResolution()` produces a valid one and only its holder may end it.
/// That is what keeps a resolution whose owner was destroyed from leaving
/// every later caller locked out, and a stale holder from releasing the
/// resolution that replaced it.
struct ResolutionLease {
    ResolutionLease() = default;
    explicit operator bool() const noexcept { return id_ != 0; }
    quint64 id() const noexcept { return id_; }

private:
    friend ResolutionLease beginResolution();
    friend void endRecovery();
    explicit ResolutionLease(quint64 id) noexcept : id_(id) {}
    quint64 id_ = 0;
};

/// Takes the one resolution, or returns an invalid lease when a module is
/// already pinned or another resolution owns it. GUI thread only.
ResolutionLease beginResolution();

/// Releases the resolution `lease` took and wakes the waiters registered
/// against it after the terminal state has been published by the caller. A
/// lease that is not the current owner - one that already ended, or one from a
/// superseded owner - does nothing, so a cancelled recovery can never clear
/// the resolution that replaced it. GUI thread only.
void endResolution(const ResolutionLease& lease) noexcept;

/// One line naming the runtime that is loaded and, when it is not the one that
/// was asked for, why. Safe to show to a user.
QString runtimeStatusText();

/// The single line a player shows when the runtime could not be produced at
/// all. Shared by the title page and the standalone player so the same failure
/// reads the same way wherever it surfaces.
///
/// It is the pending line, not a missing-runtime error, while a resolution is
/// in flight: a module that is still being resolved has not been refused, and
/// reporting it as absent is a lie the user can act on.
QString runtimeUnavailablePlaybackText(const QString& reason);

/// The Windows system-driver dependencies this module's own PE import table
/// asks for: read from the image, so a module that declares none is never asked
/// about a dependency it does not have. Loads nothing and copies nothing.
///
/// This is the packaged mpv module's static `vulkan-1.dll` import, which is
/// present even though the player renders through OpenGL.
QStringList moduleSystemDriverImports(const QString& modulePath);

/// The named refusal `modulePath` takes when one of those dependencies is not
/// provided by the trusted Windows system driver path. Empty when the module
/// needs none, or when every one of them is present there; also empty when the
/// image cannot be read, so the loader's own refusal speaks for that case
/// instead of a guess about bytes this function never saw.
QString runtimeLoaderDependencyRefusal(const QString& modulePath);

/// Guards one recovery attempt per process, so two players asking for a runtime
/// at once produce one resolution and one retry. `beginRecovery()` returns
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
