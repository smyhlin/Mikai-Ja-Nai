// Prepared-engine identity for the native (AnimeJaNai) cache.
//
// Two identities, kept apart and never substituted for one another:
//  * `GraphContentIdentity` is device-independent: role, the SHA-256 of the
//    graph bytes and the I/O the graph actually declares.
//  * `EngineBuildIdentity` is the machine build request: provider, runtime and
//    plugin versions, the core source SHA and ABI, device UUID / SM / LUID,
//    CUDA and driver, precision and TF32, and the build options.
//
// `BuildRequest` joins one graph, one stage and one machine. Its `buildKey()` is
// the pre-build lookup key; the manifest also records `engineSha256`, the
// post-serialization product digest, which is not derivable from that key.
//
// Nothing here builds or deserializes an engine, and the full prepare/activate
// lifecycle is not claimed here. A value the caller cannot observe is refused
// rather than defaulted: a build key that looks complete while naming no real
// machine would be reused on hardware the engine was never compiled for.

#pragma once

#include <optional>

#include <QtCore/QByteArray>
#include <QtCore/QJsonObject>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVector>

#include "NativeCatalog.hpp"
#include "NativeTypes.hpp"

namespace mplayer {
namespace native {

/// The TensorRT/DirectML build precision. Persisted as a stable string; a build
/// request naming anything else is refused rather than rounded to the nearest
/// one, because the two produce different engines from identical inputs.
enum class EnginePrecision { Fp16, Fp32 };

QString stableName(EnginePrecision precision);
std::optional<EnginePrecision> enginePrecisionFromName(const QString& name);

// --------------------------------------------------------------- build options

/// One parsed build option: `--name` or `--name=value`. The value is kept
/// byte-exact; nothing is trimmed, case-folded or re-spelled here, because the
/// option string is handed to a real builder whose spelling is its own.
struct BuildOption {
  QString name;
  QString value;
  bool hasValue = false;

  bool operator==(const BuildOption& other) const {
    return name == other.name && value == other.value && hasValue == other.hasValue;
  }
  bool operator!=(const BuildOption& other) const { return !(*this == other); }
};

/// Splits a raw option string on whitespace, honouring double quotes so a value
/// with a space survives. Every token must be `--name` or `--name=value`.
/// Returns nothing with a reason for a dangling quote or a token that is not an
/// option.
std::optional<QVector<BuildOption>> parseBuildOptions(const QString& raw, QString* reason = nullptr);

/// The one canonical form. The options keep the order and the spelling they were
/// given, because the string is handed to a real builder whose option semantics
/// are its own; reordering is not this module's call. The only value touched is
/// `tacticSources`, whose comma-separated members are a genuine unordered set and
/// are sorted and de-duplicated. A value holding spaces is re-quoted so the
/// canonical string parses back to the same options.
QString canonicalizeBuildOptions(const QVector<BuildOption>& options);

/// Parse then canonicalize. Convenience for the one call site that has a raw
/// string; returns empty with a reason for an unparseable input.
QString canonicalBuildOptionString(const QString& raw, QString* reason = nullptr);

// --------------------------------------------------------------- identities

/// The graph's own identity. Nothing machine- or provider-specific appears
/// here, and the declared I/O comes from the inspected profile rather than from
/// a name, so a graph that lies about its boundary cannot be identified as
/// something else.
struct GraphContentIdentity {
  ModelRole role = ModelRole::Spatial;
  /// SHA-256 of the graph file's bytes, lower-case hex.
  QString graphSha256;
  /// The boundary the graph actually declares. Required: an identity that names
  /// no declared boundary cannot be hashed to the same content id as the
  /// catalogue row it came from, so it is refused rather than completed with a
  /// default profile no graph stated.
  ModelProfile declaredProfile;

  /// True only when the digest is a sha256 and the declared boundary is
  /// populated. Completeness is what makes `contentId()` recomputable at all.
  bool isComplete() const;
  /// The canonical, hashed payload. Stable across processes and machines.
  QString payload() const;
  /// "sha256:<hex>" over `payload()`, computed by the catalogue so there is one
  /// content-id convention rather than two.
  QString contentId() const;

  bool operator==(const GraphContentIdentity& other) const;
  bool operator!=(const GraphContentIdentity& other) const { return !(*this == other); }
};

/// Convenience over a catalogue row. The row's `contentId()` and this must
/// agree; nothing here re-reads the file.
GraphContentIdentity graphIdentityOf(const ModelRecord& record);

/// The machine- and provider-specific half of the build request.
struct EngineBuildIdentity {
  Backend provider = Backend::TensorRT;

  /// Observed versions. Empty is refused, never treated as "whatever it is".
  QString runtimeVersion;
  QString pluginVersion;
  /// The inference core's source SHA (40 hex), not a marketing version string.
  QString coreSha;
  int sourceAbi = 0;

  EnginePrecision precision = EnginePrecision::Fp16;
  bool tf32 = false;
  bool stronglyTyped = true;
  bool skipInference = true;

  /// Observed device facts.
  QString deviceUuid;
  /// D3D adapter LUID when DirectML is in play; required for it, absent for
  /// TensorRT.
  QString adapterLuid;
  int smMajor = 0;
  int smMinor = 0;
  QString cudaVersion;
  QString driverVersion;

  /// Raw options as the builder will receive them, before canonicalization.
  QVector<BuildOption> options;

  /// The canonical option string, computed on demand from `options`.
  QString canonicalOptions() const { return canonicalizeBuildOptions(options); }

  /// SM as "major.minor". Two components rather than a single opaque "120",
  /// because 12.0 and 1.20 are not the same revision and a joined number cannot
  /// tell them apart.
  QString smText() const;

  /// Names the first required field that is missing, empty-looking, an explicit
  /// placeholder, malformed (a `coreSha` that is not 40 hex) or a value that
  /// looks like a URL or a credential (a signed download URL, an `mk_` key, a
  /// bearer token). Empty means every required value is observed and none is
  /// sensitive.
  QString firstMissingRequiredField() const;

  /// Names the first observed text field that looks like a URL or a credential.
  /// Such a value names no real runtime, plugin or device, so it is refused by
  /// name rather than recorded in a manifest or echoed in a diagnostic. Empty
  /// means no field looks sensitive.
  QString firstSensitiveField() const;

  /// The canonical, hashed payload of this half alone.
  QString payload() const;

  bool operator==(const EngineBuildIdentity& other) const;
  bool operator!=(const EngineBuildIdentity& other) const { return !(*this == other); }
};

/// One prepared stage: the padded shapes the builder is asked to compile for and
/// the tensor format it is bound to. The padding is recorded because two stages
/// resolving to the same shape by different routes are the same request only if
/// the padding agrees.
struct StageBuildRequest {
  ModelRole role = ModelRole::Spatial;
  /// "1x3x1088x1920". The *padded* shape handed to the builder, not the source
  /// geometry and not the graph's declared shape.
  QString inputShape;
  QString outputShape;
  /// "<dtype>:<layout>", e.g. "fp16:chw".
  QString ioFormat;
  int padLeft = 0;
  int padTop = 0;

  bool isComplete() const;
  QString payload() const;
};

/// The whole pre-build request: one graph, one stage, one machine's build
/// inputs. Its `buildKey()` is the lookup key for the artifact cache.
struct BuildRequest {
  GraphContentIdentity graph;
  StageBuildRequest stage;
  EngineBuildIdentity build;

  /// Names the first thing that would make this request unbuildable: an
  /// incomplete identity, or a value that is present but only a placeholder.
  QString firstMissingRequiredField() const;
  bool isComplete() const { return firstMissingRequiredField().isEmpty(); }

  /// Hashed over the graph's content identity - role, content digest and the
  /// declared boundary the graph states - the stage request and the build
  /// identity. The declared boundary is hashed, not merely implied by the
  /// digest: a build key and a `modelContentId` that disagree about what the
  /// graph declares must not be able to describe the same artifact, and the
  /// manifest records the boundary so it can recompute this key.
  QString payload() const;
  /// "sha256:<hex>" over `payload()`. Known before any engine exists.
  QString buildKey() const;
};

// --------------------------------------------------------------- manifest

/// The recorded stage, exactly as the manifest states it.
struct ManifestStage {
  ModelRole role = ModelRole::Spatial;
  QString inputShape;
  QString outputShape;
  QString ioFormat;
  int padLeft = 0;
  int padTop = 0;

  bool operator==(const ManifestStage& other) const;
};

/// The recorded build inputs, exactly as the manifest states them.
struct ManifestBuild {
  Backend provider = Backend::TensorRT;
  QString runtime;
  QString plugin;
  QString coreSha;
  int sourceAbi = 0;
  EnginePrecision precision = EnginePrecision::Fp16;
  bool tf32 = false;
  bool stronglyTyped = true;
  bool skipInference = true;
  /// Already canonical, including `tacticSources`. A manifest carrying a
  /// non-canonical string is refused rather than normalized, because a
  /// hand-edited option string would otherwise quietly produce a different key
  /// than the one it claims.
  QString canonicalOptions;
};

/// The recorded device, exactly as the manifest states it.
struct ManifestDevice {
  QString uuid;
  QString sm;
  QString driver;
  QString cuda;
  /// Present for DirectML, absent for TensorRT.
  QString luid;
};

/// The recorded engine file: a name relative to the owned artifact directory
/// that holds this manifest - never absolute, and never resolved against the
/// process working directory - its exact size and its SHA-256. `sha256` is the
/// post-serialization product digest and is never derivable from the build key.
struct ManifestEngine {
  QString file;
  qint64 bytes = 0;
  QString sha256;
};

/// The recorded graph side: which graph, by content identity, plus the digest
/// and byte count of the file it was built from. There is deliberately no
/// absolute source path - the same manifest has to stay meaningful on another
/// machine, and the path is re-supplied by the catalogue at verification time.
struct ManifestSource {
  QString modelContentId;
  QString onnxSha256;
  qint64 onnxBytes = 0;
  /// The boundary the graph declared, recorded so `modelContentId` stays
  /// recomputable from the manifest and `graphIdentity()` reconstructs the
  /// whole content identity. It is the graph's boundary, not the prepared,
  /// padded stage the manifest records under `stage`.
  ModelProfile declaredProfile;
};

/// One prepared artifact's manifest, schema `mikai.native.manifest/1`.
///
/// Strict parse: exact schema id, every member present, no unknown member, and
/// every value in the shape this module defines. A member this build cannot
/// honour - a future schema, a provider it does not know, an SM or shape that
/// does not parse - is refused rather than interpreted.
struct ArtifactManifest {
  QString schema;
  /// The pre-build lookup key this artifact was published under.
  QString buildKey;
  /// The post-serialization product digest. Never a substitute for `buildKey`.
  QString engineSha256;
  ManifestSource source;
  ManifestStage stage;
  ManifestBuild build;
  ManifestDevice device;
  ManifestEngine engine;
  /// ISO-8601 UTC, exactly as recorded. Not used as identity.
  QString createdAt;
  int mikaiFormat = 0;

  /// The identities this manifest asserts, rebuilt from the recorded fields
  /// rather than stored beside them: role, the graph digest and the declared
  /// boundary the manifest records. `isSelfConsistent` requires the result's
  /// `contentId()` to equal `source.modelContentId`, so the reconstructed
  /// identity and the recorded content id cannot disagree. The manifest names
  /// the *prepared* padded stage separately; this is the graph's own boundary.
  GraphContentIdentity graphIdentity() const;
  EngineBuildIdentity buildIdentity() const;
  StageBuildRequest stageRequest() const;
  /// The request whose `buildKey()` must equal `buildKey`.
  BuildRequest request() const;

  /// True when the recorded fields agree with each other. `reasonKey`/`reason`
  /// name the first disagreement.
  bool isSelfConsistent(QString* reasonKey = nullptr, QString* reason = nullptr) const;
};

/// "mikai.native.manifest/1". The one schema this build writes and reads.
QString artifactManifestSchema();

QJsonObject artifactManifestToJson(const ArtifactManifest& manifest);

/// Parses strictly. `manifest` is only written on success, so a caller that
/// ignores the return value cannot act on a half-parsed document.
bool parseArtifactManifest(const QJsonObject& object, ArtifactManifest* manifest,
                           QString* reasonKey = nullptr, QString* reason = nullptr);
bool parseArtifactManifest(const QByteArray& payload, ArtifactManifest* manifest,
                           QString* reasonKey = nullptr, QString* reason = nullptr);

QByteArray serializeArtifactManifest(const ArtifactManifest& manifest);

// --------------------------------------------------------------- shape rules

/// A stage shape is four positive decimal dimensions, "NCHW", in that order.
/// Anything else - three dimensions, a wildcard, a negative, a float, leading
/// zeroes - is refused rather than padded into place.
bool isStageShapeText(const QString& text);

/// "<dtype>:<layout>": an eligible dtype this bridge can bind, and a layout
/// name from the known set. An unknown dtype is refused, never converted.
bool isIoFormatText(const QString& text);

/// The SM string this module writes and accepts: two non-negative decimal
/// components separated by a dot.
bool isSmText(const QString& text);

}  // namespace native
}  // namespace mplayer
