// App-owned assembly of one whole prepared native admission for a live player.
//
// A playing core announces only what its own decoder and probe observed: the
// requested selection, the source geometry, the device and the module the core
// was actually loaded from. This layer turns that announcement into the
// canonical `PreparedAdmissionRequest` the per-core controller resolves and
// submits. It reads the app-owned model store, resolves the registered or
// builtin recipe against the observed source, takes the inference core's
// provenance and the backend content revision from the validated bundle, measures
// the device, runtime and builder policy with the preparation toolchain's own
// describe mode, and proves the whole resolved plan and every artifact it names -
// all off the GUI thread.
//
// Four rules are load-bearing here.
//
//  * **Nothing is built.** This is the admission path, not the preparation
//    path: no engine is compiled, no CPU reference is run, no cache entry is
//    written, and no device or digest is copied from a cache as the current
//    one. A stage whose concrete geometry has no recorded resolution refuses by
//    name rather than measuring one now.
//
//  * **Requested is not effective.** Assembling a plan proves an ordered plan
//    can be admitted against observed facts. It says nothing about any engine
//    load, and this module never emits Prepared, Ready or Active.
//
//  * **Whole plan or nothing.** One stage that cannot be resolved, one owned row
//    that no longer matches its identity, one artifact that is not present and
//    proved drops the whole admission. There is no partial success.
//
//  * **Nothing is invented.** The selection is used verbatim, the source and
//    device are the owning core's own observation, and the build identity is
//    measured - an absent fact is a named refusal, never a default.
//
// It is deliberately not a service: one free worker function and one free
// binding function, wired per dialog. No singleton, no event bus, no QObject
// parent chain.

#pragma once

#include <optional>

#include <QtCore/QString>
#include <QtGlobal>

#include "native/prepare/NativePreparedPlan.hpp"
#include "native/prepare/NativePreparationContext.hpp"
#include "native/status/NativeTypes.hpp"

namespace mikai::app {

class Runner;

}  // namespace mikai::app

namespace mplayer::shell {
class PlayerDialog;
}  // namespace mplayer::shell

namespace mikai::app {

/// One announced request, exactly as the owning core reported it. No member is
/// re-derived here: the selection is verbatim from the preference store and the
/// context is the owning core's own observation.
struct NativeAssemblyRequest {
  mplayer::native::SelectionState selection;
  mplayer::native::NativePreparationContext context;
  /// Carried verbatim into the admission so a superseded reply can be named and
  /// discarded by its caller.
  QString requestId;
  qint64 revision = 0;
};

/// The whole-assembly outcome. `request` is meaningful only when `ok` is true;
/// otherwise `reasonKey`/`reason` name the first fact that refused it.
struct NativeAssemblyOutcome {
  bool ok = false;
  mplayer::native::PreparedAdmissionRequest request;
  QString reasonKey;
  QString reason;
};

/// Assembles the whole admission for one announced request.
///
/// It reads the app-owned model store, resolves the requested recipe against
/// the observed source, measures the machine identity off the GUI, resolves the
/// plan with only the resolutions the owned rows already recorded, and proves
/// the whole plan and every artifact it names before returning. It may block on
/// file IO and on the describe helper; it never builds and never runs a CPU
/// reference, so it is safe (and intended) to call from a worker thread.
///
/// A missing prerequisite is named rather than worked around: an unobserved
/// source or device, an absent toolchain, an unowned model store, an unknown or
/// ineligible recipe, an unmeasured build identity and a missing artifact each
/// refuse by their own stable key.
NativeAssemblyOutcome assembleNativeAdmission(const NativeAssemblyRequest& request);

/// Binds one dialog's `nativeAdmissionRequested` and `nativeAdmissionCancelled`
/// to the app assembly.
///
/// The work runs on `runner`; the assembled request is delivered on the GUI
/// thread through the dialog, guarded by the dialog's QPointer and by the run's
/// generation, so a result for a selection or an owning core that has since
/// moved is discarded rather than applied. A refusal is reported through
/// `PlayerDialog::reportNativeProcessingFailure`: it is never routed to a media
/// retry, and it never re-extracts a URL or re-probes a quality.
void bindNativePlaybackAssembly(Runner& runner, mplayer::shell::PlayerDialog& dialog);

}  // namespace mikai::app
