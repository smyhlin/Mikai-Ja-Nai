// The app-owned model store: where an inspected graph's bytes live, how a copy
// is published atomically, and how the store is proved still to describe what it
// says.
//
// Six rules are load-bearing here.
//
//  * The external file a model was imported from is a **read-only input**. The
//    store never writes beside it, never uses its directory as a cache, and never
//    reaches outside the app-owned `native/models` tree - the same containment
//    policy `NativePaths` gives the artifact store. That tree is the *default*
//    root, so the store is owned before the directory exists: naming a path
//    inside the app-owned tree, with no reparse point on the part of it that
//    exists, is enough to claim it, and the first write creates it. A root whose
//    existing part resolves elsewhere - a user's model folder, a shared drive, a
//    junction - is refused exactly as it was when it already existed.
//
//  * A published copy is **write-once**. Its directory is named by the graph's
//    content digest, so two files sharing a basename occupy two rows, and the
//    bytes that were inspected are the bytes that are stored: they are written
//    from the in-memory image the inspector read, not copied from a path that may
//    have changed in between.
//
//  * A graph whose declared boundary is only a *candidate* - a symbolic output
//    batch, or an H/W the graph left dynamic - is published, but is only ever
//    offered once a genuine CPU reference run has proved the concrete N=1/C=3
//    boundary it produces at the requested geometry. The proof is recorded as
//    `verifiedReferenceProfile`, separate evidence beside the declared profile;
//    without it the row is `Unavailable` naming `model.reference.needed`.
//
//  * Nothing is published for a graph the inspector refused. A truncated graph, a
//    graph with two inputs, a rank-5 or C!=3 boundary, or a non-uniform declared
//    scale leaves the store exactly as it was.
//
//  * A non-FP16 graph **is** published, with availability `Unavailable` and
//    `model.io.unsupported` naming the observed dtype. Recording it is the point:
//    the user can see the model and why it cannot be used. It is never converted,
//    re-exported or executed.
//
//  * The row's **backend support** is model compatibility, not runtime presence.
//    It is exactly the set the importing provisioner supplied: recorded when the
//    row is published, persisted with it and rehydrated by every read. A
//    provisioner that supplies nothing records nothing - the row claims no
//    backend rather than being credited with this build's vocabulary - and a
//    document that does not record the set is refused rather than read as some
//    other one. A later import of the same byte graph that supplies a non-empty
//    set updates only that member of the row's document, never its bytes,
//    identity, provenance or reference proof, and a supply of nothing never
//    erases what was vouched before. Which backends the runtime currently carries
//    is supplied separately to the planner, so the model's claim and the
//    runtime's capability stay two facts, and a model is planned only on a
//    backend that is both recorded as supported and currently present.

#pragma once

#include <atomic>
#include <optional>

#include <QtCore/QByteArray>
#include <QtCore/QSize>
#include <QtCore/QString>
#include <QtCore/QVector>

#include "NativeCatalog.hpp"
#include "NativeModelInspect.hpp"
#include "NativeTypes.hpp"

namespace mplayer {
namespace native {

/// Lower-case hex SHA-256 of `bytes`, the one digest convention the store uses.
QString sha256HexOf(const QByteArray& bytes);

/// The one cancellation gate an import and its submitter share. `Inspecting` is
/// the only state a cancel can win in; the worker's publication gate moves it
/// atomically to `Publishing` immediately before the store is written, so a
/// cancel that loses that race is refused by name instead of being reported as
/// having stopped a write that already committed.
class ImportCancellation {
 public:
  enum class State { Inspecting, Cancelled, Publishing };
  enum class CancelResult {
    /// This call moved the gate from `Inspecting` to `Cancelled`.
    CancelledNow,
    /// An earlier call had already cancelled it; the state is unchanged.
    AlreadyCancelled,
    /// The worker already passed the publication gate. Nothing can be stopped,
    /// and the committed outcome must still be delivered.
    TooLate,
  };

  /// Cancels only while the import has not begun publishing. Never claims to
  /// have stopped a publication that already started.
  CancelResult cancel() {
    State expected = State::Inspecting;
    if (state_.compare_exchange_strong(expected, State::Cancelled)) {
      return CancelResult::CancelledNow;
    }
    return expected == State::Cancelled ? CancelResult::AlreadyCancelled : CancelResult::TooLate;
  }

  /// The worker's pre-publication gate: true only for the one call that moves
  /// `Inspecting` to `Publishing`. False means a cancel won first, and nothing
  /// may be written.
  bool beginPublication() {
    State expected = State::Inspecting;
    return state_.compare_exchange_strong(expected, State::Publishing);
  }

  State state() const { return state_.load(); }
  bool isCancelled() const { return state() == State::Cancelled; }

 private:
  std::atomic<State> state_{State::Inspecting};
};

/// One published model: the catalogue row, the external file it was imported
/// from, and the immutable owned copy.
struct StoredModel {
  ModelRecord record;
  /// The external file the bytes were originally taken from. Provenance only:
  /// recorded and displayed, never written and never read at runtime, so moving
  /// or deleting it cannot invalidate the owned copy.
  QString sourcePath;
  /// The immutable owned copy, inside the store root.
  QString storedPath;
  /// The geometry the import verified for a dynamic graph, as a sample at the
  /// stage size it ran at, carrying the verified reference boundary when a CPU
  /// run produced one. Absent for a fixed, strictly eligible graph (nothing to
  /// resolve), for an unsupported dtype (nothing was run), and for a candidate
  /// whose run did not happen (nothing was proved).
  std::optional<StageResolution> importResolution;
};

struct ModelImportOutcome {
  /// True when the row is offered (`record.availability.isUsable()`).
  bool ok = false;
  /// True only when this call actually wrote a new immutable copy.
  bool published = false;
  QString reasonKey;
  QString reason;
  StoredModel model;
};

class ModelInventory {
 public:
  /// `AppLocalDataLocation()/native/models`, per §9.2. Never created by this call.
  static QString defaultRoot();
  /// An explicit root. Constructing validates and creates nothing.
  explicit ModelInventory(QString root = defaultRoot());

  const QString& root() const { return root_; }

  /// True when this root is inside the app-owned `native/models` hierarchy and
  /// crosses no reparse point. A store pointed at a user's model folder is the
  /// failure this exists to stop, and no argument turns it into a store. A root
  /// that does not exist yet is owned exactly when the part of it that does exist
  /// is that same app-owned tree, so `defaultRoot()` may be claimed before its
  /// first write; a missing root whose existing part resolves anywhere else is
  /// still refused.
  bool isOwned(QString* reason = nullptr) const;
  /// Creates the root if it is owned and absent. The only writer of it.
  bool ensureOwned(QString* reason = nullptr) const;

  /// Reads `sourcePath` (read-only), inspects it through the ORT C API, and
  /// publishes an immutable copy. A refused graph writes nothing. `stageGeometry`
  /// is the geometry a dynamic graph's CPU reference is resolved at; it is
  /// ignored by a fixed graph.
  ///
  /// `backends` is the availability the importing provisioner supplies, and it is
  /// recorded as the row's backend support (model compatibility) and persisted
  /// with it. Supplying nothing records nothing: the row claims no backend rather
  /// than this build's whole vocabulary, so the runtime's capability is never
  /// mistaken for the model's. Importing the same bytes again with a non-empty
  /// supply raises that one claim in the row's own document, and a supply of
  /// nothing leaves a previous vouch exactly as it was.
  ///
  /// `cancellation` is optional (null means no cancellation, as every CLI and
  /// test call site has it). When present, a cancel that wins before the
  /// publication gate means nothing is inspected after the check and nothing is
  /// written; once the gate has been passed the copy is committed and a later
  /// cancel is refused rather than re-labelling that committed outcome.
  ModelImportOutcome importFile(const QString& sourcePath, const InspectionRuntime& runtime,
                                const QSize& stageGeometry,
                                const BackendAvailability& backends = BackendAvailability(),
                                const QString& termsNote = QString(),
                                ImportCancellation* cancellation = nullptr);

  /// Publishes bytes that were already inspected. Does not run anything. The
  /// digest is computed over exactly these bytes, so the stored copy and the
  /// identity cannot disagree. `backends` is the provisioner's supplied
  /// availability, recorded as the row's backend support exactly as `importFile`
  /// records it; when these exact bytes are already published, a non-empty supply
  /// updates only that recorded set - no new identity and no second copy - and an
  /// empty one leaves it alone.
  ModelImportOutcome publish(const QByteArray& bytes, const InspectionResult& inspection,
                             const QString& sourcePath, const QString& displayName,
                             const BackendAvailability& backends = BackendAvailability(),
                             const QString& termsNote = QString());

  /// Every published row, each re-proved against the immutable copy on disk.
  /// An entry whose manifest or bytes do not verify is returned as a row that is
  /// `Unavailable` with the reason - never dropped, never deleted, never treated
  /// as usable. A root the app owns but has not created yet is an empty
  /// inventory with no reason: the missing root is not a missing store. A root
  /// that is not an owned store is refused instead, with the ownership reason
  /// set and no rows.
  QVector<StoredModel> stored(QString* reason = nullptr) const;

  /// Re-proves the immutable owned copy against the identity it was published
  /// under: the stored path is inside the owned root and crosses no reparse
  /// point, its bytes still hash to the recorded digest and size, and the row's
  /// id still derives from that digest and declared profile. The original import
  /// path is provenance and is never read, so a moved or deleted source cannot
  /// fail this. False means the owned copy no longer describes what the row
  /// claims and must not be resolved.
  bool verifyOwnedModelIntegrity(const StoredModel& model, QString* reasonKey = nullptr,
                                 QString* reason = nullptr) const;

  static QString manifestFileName();
  static QString modelFileName();

 private:
  QString root_;
};

}  // namespace native
}  // namespace mplayer
