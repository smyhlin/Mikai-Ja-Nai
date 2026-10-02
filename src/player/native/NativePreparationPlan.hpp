// The source-aware preparation plan (M13): pure planning, QtCore-only, over
// values somebody observed. It starts no process, opens no engine and reads no
// file; M15 performs the work it describes.
//
// Observed, never guessed: source geometry, cadence, colour tags and device come
// from the decoder and the probe snapshot. Unobserved facts leave the plan
// Preparing (absent, not a 1x1 or device-0 placeholder); a requested recipe is
// all-or-nothing, so an absent model, unavailable backend, non-integer scale or
// an overflow refuses the whole plan by name instead of degrading like the
// reference's missing-model skip.
//
// The plan resolves the cropped-then-rotated display geometry (a portrait source
// plans as portrait, not transposed back), passes matrix/range/siting/pixel
// format through unchanged, and pads the temporal tensors to a centred multiple
// of 64, rounded down to even, as `setup_rife` does. A dynamic graph resolves at
// the stage it actually runs at through a caller-supplied resolver - never from
// a file name and never from a default of 1 or 2.
//
// No build key and no engine digest live here (those are M14's).
// `ResolvedStage::buildRequestPayload()` is canonical and carries no source URL,
// id, plan id or cadence, so two sources with the same stage geometry produce one
// prepared artifact set.

#pragma once

#include <functional>
#include <optional>

#include <QtCore/QRect>
#include <QtCore/QSize>
#include <QtCore/QString>
#include <QtCore/QVector>

#include "NativeCatalog.hpp"
#include "NativeRecipe.hpp"
#include "NativeTypes.hpp"

namespace mplayer {
namespace native {

/// The reference pads to this multiple, centred and rounded down to even.
inline constexpr int kTensorAlign = 64;

/// A dimension beyond this is refused by name rather than multiplied into an
/// allocation. The reference keeps these in `int`; doing the arithmetic in 64 bits
/// and refusing the result keeps a bad chain from producing a wrapped size.
inline constexpr qint64 kMaxDimension = 65536;

/// What the decoder actually reported for the source.
///
/// `codedSize` is the frame as decoded. `crop` is in coded coordinates and is
/// applied before `rotationDegrees`, the same order mpv applies them. Both are
/// absent when nothing has been observed; an absent rotation means no rotation
/// was observed, and absent is not the same claim as zero only in that a caller
/// may still learn of one.
struct SourceGeometry {
  std::optional<QSize> codedSize;
  std::optional<QRect> crop;
  std::optional<int> rotationDegrees;
  std::optional<Rational> fps;

  /// Exactly as libmpv reported them, passed on unchanged. Empty is unknown.
  QString csp;
  QString matrix;
  QString range;
  QString siting;
  /// 8, 10, ... - 0 is unknown, never "8 because that is the usual case".
  int bitDepth = 0;

  /// The size the filter is configured with: cropped, then rotated. Absent when
  /// the coded size is not known or the rotation is not one of 0/90/180/270,
  /// because a size cannot be derived from a rotation this build does not model.
  std::optional<QSize> displaySize() const;

  /// True only when an observed rotation is one of 0/90/180/270. An absent
  /// rotation is "not observed yet", not a confirmed none, so it is false.
  bool hasModelledRotation() const;

  /// The pixel count of `displaySize()`. Absent with it.
  std::optional<qint64> pixelCount() const;

  bool isKnown() const { return displaySize().has_value() && fps.has_value(); }
};

/// What the probe reported about the machine. Every field empty is a real
/// "Preparing", never a default device.
struct DeviceIdentity {
  /// The actual decoder device, from the AVHWFramesContext.
  QString decoderDeviceUuid;
  /// The device inference would run on. Distinct from the decoder's: a machine
  /// can decode on one adapter and compute on another.
  QString computeDeviceUuid;
  /// D3D adapter LUID, when DirectML is in play.
  QString adapterLuid;
  /// 0 is unknown. There is no such thing as a default SM.
  int smMajor = 0;
  int smMinor = 0;

  bool isKnown() const {
    return !decoderDeviceUuid.isEmpty() && !computeDeviceUuid.isEmpty() && smMajor > 0;
  }
};

/// The chain's admitted source window, in the reference's own terms: a pixel
/// count range and an fps range. Zero pixels on either side is unbounded, and an
/// absent fps bound is unbounded - which is what `0x0` / `infxinf` mean in the
/// reference conf.
struct ChainWindow {
  qint64 minPixels = 0;
  qint64 maxPixels = 0;
  std::optional<Rational> minFps;
  std::optional<Rational> maxFps;

  bool admits(qint64 pixels, const Rational& fpsValue) const;
  /// Why it does not, as a stable key plus the observed values.
  QString refusal(qint64 pixels, const Rational& fpsValue, QString* reasonKey,
                  QString* reason) const;
};

/// What a dynamic graph resolved to at one concrete stage geometry.
///
/// This is the answer to "what does this graph do with a 1080x1920 input", and it
/// is supplied by whoever inspected the graph for that input. It is never
/// inferred: a resolver that answers from a file name has been wired wrongly.
struct ResolvedProfile {
  /// The stage input the answer is for. Checked against the real stage input, so
  /// a resolver answering for the wrong geometry is refused rather than applied.
  QSize input;
  /// The stage output the graph actually produces at that input.
  QSize output;
  int inputChannels = 3;
  int outputChannels = 3;
};

/// Resolves a dynamic graph's geometry at a concrete stage input. Absent or
/// nullopt is not "assume 2x": it is an unresolved stage, refused by name.
using DynamicProfileResolver =
    std::function<std::optional<ResolvedProfile>(const ModelRecord& record, const QSize& stageInput)>;

enum class StageKind { Spatial, Temporal };

/// One resolved step of the chain, in execution order.
///
/// The declared tensors stay declared and the runtime geometry stays resolved:
/// `declaredInput`/`declaredOutput` are what the graph says about itself, and
/// `inputGeometry`/`outputGeometry` are what this stage will actually be built
/// and run at. Collapsing the two is how a dynamic graph ends up described by a
/// shape it never declared.
struct ResolvedStage {
  StageKind kind = StageKind::Spatial;
  ModelRole role = ModelRole::Spatial;

  QString modelId;
  /// `computeContentId` over the graph: role, ONNX digest, declared I/O.
  QString modelContentId;
  /// Index into the recipe's spatial stages; -1 for the temporal stage.
  int recipeIndex = -1;

  /// The geometry the engine is built at, which is what the shape strings report.
  QSize inputGeometry;
  QSize outputGeometry;

  /// The frame size entering and leaving the stage. Equal to the geometry above
  /// except for a padded temporal stage, whose output is cropped back to it.
  QSize contentGeometry;

  /// The uniform positive integer scale for a spatial stage, absent for temporal
  /// (which interpolates and does not rescale). Absent on a spatial stage means
  /// the graph does not support an integer scale at this geometry.
  std::optional<int> scale;

  /// The graph's own declaration, unaltered, including which dimensions it left
  /// dynamic.
  TensorProfile declaredInput;
  TensorProfile declaredOutput;
  /// True when the declaration left H/W dynamic and this stage's geometry came
  /// from the supplied resolver.
  bool geometryFromResolver = false;

  int inputChannels = 3;
  int outputChannels = 3;

  /// Centred padding to a multiple of 64, each rounded down to even. Zero on a
  /// spatial stage, which is built for its exact work shape.
  int padL = 0;
  int padT = 0;

  Rational inputFps{1, 1};
  Rational outputFps{1, 1};

  /// Temporal only.
  TemporalOrder order = TemporalOrder::AfterSpatial;
  Rational factor{1, 1};
  /// The declared scene-cut threshold, carried for the engine configuration.
  double sceneThreshold = 0.0;

  /// "1x3x1080x1920" - N, C, H, W, the order the reference prints its build dims.
  QString inputShape() const;
  QString outputShape() const;
  /// "fp16:chw". The only binding this bridge has.
  QString ioFormats() const;

  /// Canonical, deterministic, and independent of any source URL, source id, plan
  /// id, cadence or recipe: exactly the facts that determine which engine file
  /// this stage needs. Two sources with the same stage geometry produce identical
  /// payloads. M14 hashes this with the device and build options to get the
  /// pre-build identity; nothing here is the engine digest.
  QString buildRequestPayload() const;
};

/// A per-model pre-resize, in the order it applies.
///
/// `beforeTemporal` marks the one the reference hoists ahead of interpolation:
/// when RIFE runs before the spatial chain, the first model's work size applies
/// to the source frames, so interpolation runs at the smaller geometry and the
/// upscale models then see every interpolated frame at that same geometry.
struct PreResizeStep {
  /// Index into `PreparationPlan::stages`, or -1 for a hoisted pre-resize, which
  /// runs before the temporal stage and so has no stage of its own.
  int stageIndex = -1;
  QString modelId;
  QSize from;
  QSize to;
  bool beforeTemporal = false;
};

/// A hard stop, named. Any non-empty list refuses the plan as a whole.
struct PlanBlocker {
  /// Stable machine key.
  QString reasonKey;
  QString reason;
  /// Index into `PreparationPlan::stages`, or -1 for a whole-plan blocker.
  int stageIndex = -1;
};

/// Everything one resolution was asked for. Nothing in it is discovered here.
struct PlanRequest {
  Recipe recipe;
  CatalogView catalog;
  BackendAvailability backends;

  SourceGeometry source;
  DeviceIdentity device;
  ChainWindow window;

  /// Required only for a dynamic graph. Absent, a dynamic stage is refused by
  /// name.
  DynamicProfileResolver dynamicResolver;
};

/// The resolved plan. `isResolved()` is the only success signal: an unknown fact
/// leaves it Preparing, and a blocker leaves it Unavailable.
struct PreparationPlan {
  /// sha256 over the canonical body. Content of the plan, never of a URL.
  QString id;
  QString recipeId;
  Backend backend = Backend::TensorRT;

  SourceGeometry source;
  DeviceIdentity device;

  /// In execution order. Empty whenever the plan is not resolved.
  QVector<ResolvedStage> stages;
  QVector<PreResizeStep> resizes;

  /// Facts nobody has observed yet. Non-empty means Preparing, not failure.
  QVector<QString> unknowns;
  /// Hard stops. Non-empty means Unavailable, and no stage runs.
  QVector<PlanBlocker> blockers;

  AvailabilityState availability;

  /// No blockers and nothing left unknown.
  bool isResolved() const { return blockers.isEmpty() && unknowns.isEmpty(); }
  /// True only when this plan can be turned into real work.
  bool canPrepare() const { return isResolved(); }

  /// The geometry the chain ends at. Absent while unresolved.
  std::optional<QSize> outputGeometry() const;
  /// The exact output cadence, `in_fps * num/den` in lowest terms.
  std::optional<Rational> outputCadence() const;

  /// The bytes `id` is a digest of. Canonical and order-stable.
  QString canonicalBody() const;
};

/// Resolves the plan. Never throws and never blocks.
PreparationPlan resolvePreparationPlan(const PlanRequest& request);

// ---------------------------------------------------------------- geometry
// Small, exact and separately testable: the reference's own arithmetic, without
// the reference's engine, its file system or its device.

/// Padded to a multiple of `kTensorAlign` on each axis, centred, each side
/// rounded down to even.
QSize padToTensorAlignment(const QSize& size, int* padL, int* padT);

/// The same padding expressed as the total added on each axis.
QSize paddingExtent(const QSize& size);

/// `fps * num / den` in lowest terms, refused on overflow rather than wrapped.
std::optional<Rational> scaleCadence(const Rational& fps, int num, int den);

/// "1x3x1080x1920". Empty for a size this build refuses.
QString stageShapeString(int channels, const QSize& size);

/// True when `output` is exactly `input` scaled by a uniform positive integer.
std::optional<int> uniformIntegerScale(const QSize& input, const QSize& output);

/// A rectangle this build can multiply into a tensor shape.
bool isRefusableGeometry(const QSize& size);

/// The display geometry for a coded size, crop and rotation, or nothing when the
/// rotation is one this build does not model.
std::optional<QSize> displayGeometry(const std::optional<QSize>& codedSize,
                                     const std::optional<QRect>& crop,
                                     const std::optional<int>& rotationDegrees);

}  // namespace native
}  // namespace mplayer
