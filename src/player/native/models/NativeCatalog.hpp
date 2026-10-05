// Model identity, the declared boundary of a graph, and the shape of a supplied
// catalogue.
//
// Types only. Importing graphs, reading declared metadata and resolving which
// backends a runtime carries are M11's behaviour; what lives here is the
// vocabulary those behaviours produce, so a recipe can be checked against an
// explicitly supplied catalogue without this module inventing a row, a model or
// a runtime capability.
//
// Two identities are kept apart. `contentId` is the graph's content identity -
// role, ONNX digest and the actually declared I/O - and depends on nothing about
// the machine that may run the graph. The prepared-build identity (M14's build
// key and its separate post-serialization `engineSha256`) is deliberately absent:
// a stage names a model, and which prepared build it resolves to is a later join.
//
// A dimension the graph does not statically declare is absent, never zero. A
// dynamic graph genuinely does not know its H and W at import time, and a
// fabricated 1x1 placeholder would be a known geometry that does not exist.
// Declared geometry is the graph's own; the geometry it resolves to at a given
// stage belongs to the preparation plan, not here.

#pragma once

#include <optional>

#include <QtCore/QCryptographicHash>
#include <QtCore/QByteArray>
#include <QtCore/QSize>
#include <QtCore/QString>
#include <QtCore/QVector>

#include "native/status/NativeTypes.hpp"

namespace mplayer {
namespace native {

enum class ModelOrigin { Builtin, UserImported };

/// The declared shape and dtype of one graph tensor.
struct TensorProfile {
  QString dtype;
  int rank = 0;
  qint64 batch = 0;
  qint64 channels = 0;
  std::optional<qint64> height;
  std::optional<qint64> width;

  bool hasGeometry() const { return height.has_value() && width.has_value(); }
  bool isDynamicGeometry() const { return !hasGeometry(); }

  /// "1x3x1080x1920", with H and W where the graph left them dynamic. Error
  /// messages quote this, so they name what was declared.
  QString describe() const {
    return QStringLiteral("%1x%2x%3x%4")
        .arg(batch)
        .arg(channels)
        .arg(height ? QString::number(*height) : QStringLiteral("H"))
        .arg(width ? QString::number(*width) : QStringLiteral("W"));
  }

  bool operator==(const TensorProfile& other) const {
    return dtype == other.dtype && rank == other.rank && batch == other.batch &&
           channels == other.channels && height == other.height && width == other.width;
  }
  bool operator!=(const TensorProfile& other) const { return !(*this == other); }
};

/// What a graph declares at its boundary: exactly one input, exactly one output.
struct ModelProfile {
  TensorProfile input;
  TensorProfile output;

  /// The uniform positive integer scale implied by fixed input and output
  /// geometry, identity included as 1. Absent while either side is dynamic: the
  /// scale cannot be learned from a geometry the graph does not declare, and
  /// guessing one - including defaulting to 1 or 2 - would invent a factor.
  std::optional<int> derivedUniformScale() const;

  bool isDynamicGeometry() const {
    return input.isDynamicGeometry() || output.isDynamicGeometry();
  }

  /// The declared source/output geometry, present only where the graph declares
  /// it statically.
  std::optional<QSize> sourceGeometry() const;
  std::optional<QSize> outputGeometry() const;
};

/// The spatial boundary rules: rank 4, N=1, C=3, positive dimensions and the
/// eligible dtype on *both* tensors, plus a uniform integer scale the declared
/// geometry actually supports. Empty means eligible.
///
/// These are the rules for a spatial graph only. A temporal graph is a separate
/// kind with a different boundary, and applying 1x3xHxW to it would refuse a
/// genuine RIFE model; its eligibility comes from the role-specific inspection
/// that provisions it, not from here.
QString spatialEligibilityRefusal(const ModelProfile& profile);

/// The structural rules a *declared candidate* is checked against, and the only
/// difference from `spatialEligibilityRefusal`: the declared output batch may be
/// the symbolic -1 an exporter writes for a dynamic batch. The input batch is
/// still exactly 1, and rank 4, C=3, the eligible dtype on both tensors, positive
/// dimensions and the uniform integer scale are unchanged. Empty means the
/// declaration is structurally acceptable - not that it has been proved, since
/// only a run can settle what a symbolic batch really is.
QString spatialCandidateRefusal(const ModelProfile& profile);

/// Whether a reference run's boundary proves a declared candidate. Refuses unless
/// `declared` passes `spatialCandidateRefusal`, the `reference` is itself a
/// strictly eligible 1x3xFP16 run boundary with the concrete dimensions a run
/// returns, and every dimension and dtype the declaration actually states is the
/// one the reference produced. A symbolic batch is the one dimension the
/// comparison leaves alone - it is exactly what the run resolves - and nothing is
/// ever forced onto `declared` to make it fit. Empty means the reference proves
/// the declaration; it says nothing about any backend's ability to run it.
QString spatialReferenceRefusal(const ModelProfile& declared, const ModelProfile& reference);

/// One catalogue row: identity, the declared boundary, where it can run, and
/// whether it is usable now. Everything the boundary *declares* lives in
/// `actualProfile`; there is no second declaration of it here.
struct ModelRecord {
  /// The persistence key: stable and named for a built-in, content-addressed for
  /// a user import.
  QString id;
  QString displayName;
  ModelOrigin origin = ModelOrigin::Builtin;

  /// A read-only input path. External model directories are read, never written
  /// and never used as a cache.
  QString onnxPath;
  /// Required. A record without one is not identifiable and is not offered.
  QString onnxSha256;
  qint64 onnxBytes = 0;

  ModelRole role = ModelRole::Spatial;

  /// The declared boundary, and the only place it is stated.
  ModelProfile actualProfile;

  /// Evidence, not a declaration: the concrete boundary a CPU reference run
  /// actually produced for this row - the input as bound and the output as
  /// returned. Present only once such a run has finished, and deliberately
  /// outside identity: `contentId()` and every published profile stay the
  /// declared ones, so the proof moves no id and is never serialized as a shape
  /// the graph declared. It is consulted only to settle a declared symbolic
  /// batch, never to replace the declared geometry.
  std::optional<ModelProfile> verifiedReferenceProfile;

  QVector<Backend> availableBackends;

  /// Attribution text carried verbatim from the catalogue record.
  QString termsNote;

  /// Usability now, with the mandatory reason when it is not.
  AvailabilityState availability;

  bool supportsBackend(Backend backend) const { return availableBackends.contains(backend); }

  /// The graph's content identity. Derived from the fields above rather than
  /// stored beside them, so it cannot go stale against the row it describes.
  QString contentId() const;

  /// Whether the row is a well-formed, identifiable and eligible declaration
  /// that states an availability. This is the structural offer, not usability
  /// now: a row that is Preparing or Unavailable is still offered, and a caller
  /// that would run it must additionally require `availability.isUsable()` and
  /// honour its reason. The store offers such a row for inspection and refuses
  /// it for use with the row's own fact.
  bool isOffered() const;

  /// Why `isOffered()` is false, as a stable key plus the fact.
  void refusal(QString* reasonKey, QString* reason) const;
};

/// The backends a runtime actually carries, supplied by whoever provisioned it.
/// Never inferred from the presence of a model.
struct BackendAvailability {
  QVector<Backend> available;

  bool has(Backend backend) const { return available.contains(backend); }
};

/// The exact set of rows a caller vouches for. An absent row is not "unknown but
/// probably fine"; a recipe naming one is rejected.
struct CatalogView {
  QVector<ModelRecord> models;

  const ModelRecord* find(const QString& id) const;
  bool contains(const QString& id) const;
};

/// Role, ONNX digest and the declared I/O of both tensors. Nothing machine- or
/// provider-specific may appear here.
QString contentIdentityPayload(const ModelRole role, const QString& onnxSha256,
                               const ModelProfile& profile);

/// "sha256:<hex>". Two files sharing a basename get two ids, and editing a file
/// in place changes its id, which is what makes artifacts built from it invalid
/// rather than silently reusable.
QString computeContentId(const ModelRole role, const QString& onnxSha256,
                         const ModelProfile& profile);

QString userModelIdFor(const QString& contentId);

}  // namespace native
}  // namespace mplayer