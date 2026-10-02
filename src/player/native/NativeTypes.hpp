// Typed vocabulary for the native (AnimeJaNai) pipeline: the enums that cross a
// persistence boundary, the stable strings they persist as, and the value types
// the model and recipe contracts are built from.
//
// Every enum persists as a stable string and never as an ordinal, so a value
// added to an enum later cannot silently repoint a persisted selection. A name
// this build does not know parses to nothing rather than to the first
// enumerator, which is how a stale or hand-edited document is detected instead
// of applied.

#pragma once

#include <optional>

#include <QtCore/QChar>
#include <QtCore/QString>
#include <QtCore/QVector>

namespace mplayer {
namespace native {

enum class ModelRole { Spatial, Temporal };

/// A recipe naming a backend the runtime does not carry fails visibly; it never
/// runs on the other one.
enum class Backend { TensorRT, DirectML };

enum class Availability { Ready, Preparing, Unavailable };

/// Learned from the declared geometry, never from a file name.
enum class SpatialRelation { Identity, Upscale, Downscale, Reframe };

enum class TemporalOrder {
  BeforeSpatial,  ///< Interpolate first, then upscale.
  AfterSpatial    ///< Upscale first, then interpolate.
};

/// Explicit rather than a free string, so a document claiming a revision this
/// build does not carry is rejected instead of read as the one it does.
enum class RifeVersion { V4_14 };

/// Ensemble is a flag on top of Full/Lite, not a third revision: the reference
/// builds it by ensembling one of the two.
enum class RifeVariant { Full, Lite };

QString stableName(ModelRole role);
QString stableName(Backend backend);
QString stableName(Availability availability);
QString stableName(SpatialRelation relation);
QString stableName(TemporalOrder order);
QString stableName(RifeVersion version);
QString stableName(RifeVariant variant);

std::optional<ModelRole> modelRoleFromName(const QString& name);
std::optional<Backend> backendFromName(const QString& name);
std::optional<Availability> availabilityFromName(const QString& name);
std::optional<SpatialRelation> spatialRelationFromName(const QString& name);
std::optional<TemporalOrder> temporalOrderFromName(const QString& name);
std::optional<RifeVersion> rifeVersionFromName(const QString& name);
std::optional<RifeVariant> rifeVariantFromName(const QString& name);

/// The vocabulary, not a claim about any runtime. What a runtime actually
/// carries is supplied by whoever provisioned it.
QVector<Backend> allBackends();

/// "rife_v4.14", "rife_v4.14_lite", "rife_v4.14_ensemble", "rife_v4.14_lite_ensemble".
QString rifeModelName(RifeVersion version, RifeVariant variant, bool ensemble);

/// 414 for full, 4141 for lite. Ensemble is a separate conf key and never
/// changes this number.
int rifeModelCode(RifeVariant variant);

/// The only dtype the FP16 NCHW bridge can bind. A graph in another dtype is
/// recorded as unsupported with that dtype named - never converted or
/// re-exported, and never hinted towards FP32, which this bridge would refuse
/// too.
inline constexpr const char* kEligibleIoType = "fp16";

bool isEligibleIoType(const QString& ioType);

/// A positive factor. A zero numerator would turn interpolation into frame
/// dropping, and a zero or negative denominator is not a frame rate.
struct Rational {
  int num = 1;
  int den = 1;

  bool isValid() const { return num > 0 && den > 0; }

  /// The same value in lowest terms. 4/2 and 2/1 are one frame rate but only one
  /// of them is the request, so reduction is opt-in and never applied on the way
  /// in or out of a document.
  Rational reduced() const;

  bool isCanonical() const;

  bool operator==(const Rational& other) const {
    return num == other.num && den == other.den;
  }
  bool operator!=(const Rational& other) const { return !(*this == other); }

  QString toString() const;
};

/// The reason is non-empty exactly when the state is not Ready: an unavailable
/// row with no reason is a lie about why nothing works.
struct AvailabilityState {
  Availability value = Availability::Unavailable;
  /// A stable machine key.
  QString reasonKey;
  /// The fact that blocks this.
  QString reason;

  bool hasMandatoryReason() const;
  bool isUsable() const { return value == Availability::Ready && hasMandatoryReason(); }
};

/// Built through these so the invariant above cannot be violated by hand.
AvailabilityState readyAvailability();
AvailabilityState preparingAvailability(const QString& reasonKey, const QString& reason);
AvailabilityState unavailableAvailability(const QString& reasonKey, const QString& reason);

/// Requested and effective are separate values because a requested-but-not-
/// applied choice has to be visible as such. Collapsing them is how a recipe that
/// silently fell back to something else comes to look like it worked.
struct SelectionState {
  /// Empty is the untouched Original pipeline.
  QString recipeId;
  Backend backend = Backend::TensorRT;

  bool isOriginal() const { return recipeId.isEmpty(); }

  bool operator==(const SelectionState& other) const {
    return recipeId == other.recipeId && backend == other.backend;
  }
  bool operator!=(const SelectionState& other) const { return !(*this == other); }
};

/// A basename is a legal id; a basename alone is never identity.
bool isValidModelId(const QString& id);

/// 64 lowercase hex characters. A record without one has no verifiable identity.
bool isSha256Hex(const QString& digest);

/// So a content-addressed user id can never be read as a built-in name.
inline constexpr const char* kUserModelIdPrefix = "user:";

bool isBuiltinModelId(const QString& id);
bool isUserModelId(const QString& id);

}  // namespace native
}  // namespace mplayer