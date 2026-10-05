// The whole-plan CPU reference lineage: every stage's real input bytes, in stage
// order, produced by running the *original* graphs this build already owns.
//
// A stage's input is not a resize of the captured frame. It is the actual output
// the previous stage's original graph returned on this machine, threaded forward
// unchanged unless the plan explicitly names a pre-resize for that stage. The
// only stage whose input is built from captured frames is the first one, and a
// temporal stage's eleven-channel input is built from the two actual frames (or
// from both frames' real chain outputs) with the original timestep/mesh/mul
// semantics. Nothing here fabricates a tensor, converts a dtype, weakens a
// threshold, or mutates a model: it writes the genuine fp16 input bytes into a
// caller-owned scratch directory and reports them as `BuilderStageReference`s,
// which is exactly what `runNativeBuild` re-runs its own oracle from.
//
// This module is QtCore-only and carries no ONNX Runtime type and no ORT include:
// it drives the existing `mikai_native_inspect` executable, the same real
// instrument the builder uses, and never links the runtime itself. A machine
// without the inspector refuses by name instead of half-building a lineage.

#pragma once

#include <optional>

#include <QtCore/QByteArray>
#include <QtCore/QSize>
#include <QtCore/QString>
#include <QtCore/QVector>

#include "NativeBuilder.hpp"
#include "NativeFrameFixture.hpp"
#include "NativeModelInventory.hpp"
#include "NativePreparationPlan.hpp"
#include "NativeTypes.hpp"

namespace mplayer {
namespace native {

/// One actual captured RGB frame plus the explicit statement of what has already
/// been done to it.
///
/// A `FrameFixture` alone cannot say whether mpv already applied the source's
/// crop and rotation. There is no default: `unprocessed == true` asserts these
/// are the coded bytes the plan's crop/rotation still apply to, and
/// `unprocessed == false` asserts the bytes are already display geometry, in
/// which case a size that is not the display size is refused rather than
/// stretched.
struct ReferenceFrame {
  FrameFixture fixture;
  bool unprocessed = false;
  /// 0 is the frame a stage chain starts from; 1 is the distinct second frame a
  /// temporal stage interpolates between. Two frames with the same index are
  /// refused: an interpolation between one frame and itself is not a pair.
  int sourceIndex = 0;
};

/// Everything one whole-plan lineage build needs. Nothing is defaulted: an
/// absent tool, an unowned scratch path, an unresolved plan, a missing owned row
/// or a missing frame refuses the whole request by name before any work starts.
struct PlanReferenceRequest {
  /// The plan the stage order and geometry come from. Must be resolved.
  PreparationPlan plan;
  /// The decoder's own report, used only to place a captured frame: its crop and
  /// rotation apply to a frame that says it is unprocessed.
  SourceGeometry source;
  /// The actual captured RGB frame(s). Frame 0 always; frame 1 when the plan
  /// carries a temporal stage.
  QVector<ReferenceFrame> frames;
  /// One owned row per stage, in stage order.
  QVector<StoredModel> models;
  /// The observed tool paths. Only `inspectExecutable` and `ortRuntimeDir` are
  /// used here; the whole struct is what the builder is handed afterwards.
  BuilderToolPaths tools;
  /// An owned scratch directory that already exists. This worker writes inside
  /// it and never creates, renames or removes it.
  QString scratchDirectory;
  /// Optional. Checked before every stage and while a helper runs.
  NativeBuildCancellation* cancellation = nullptr;
  /// Optional. Observed facts only; no synthesised percentages.
  BuilderProgressCallback progress;
};

/// Where one stage's real input bytes come from.
enum class StageInputSource {
  /// Frame 0, after the source's crop/rotation when it is unprocessed.
  CapturedFrameA,
  /// The previous stage's real CPU output, cropped back to its content geometry
  /// when that stage was a padded temporal one.
  PreviousStage,
  /// A temporal stage that runs before the spatial chain: the two captured
  /// frames, at the geometry the hoisted pre-resize leaves them at.
  CapturedPair,
  /// A temporal stage that runs after the spatial chain: frame 0's and frame 1's
  /// real chain outputs, at the chain's output geometry.
  ChainPair,
};

/// The pure transition one stage's input takes. Geometry only - no bytes - so a
/// test can hold the whole ordered chain to its boundaries without running a
/// graph.
struct StageInputPlan {
  StageInputSource source = StageInputSource::CapturedFrameA;
  /// The unpadded content geometry entering this stage.
  QSize contentGeometry;
  /// The geometry the graph actually binds: equal to `contentGeometry` for a
  /// spatial stage, padded to the tensor alignment for a temporal one.
  QSize tensorGeometry;
  int channels = 3;
  int padL = 0;
  int padT = 0;
  /// The pre-resize this stage names, if the plan names one. Identity (from ==
  /// to) when it names none, with `resizeNamed` false.
  bool resizeNamed = false;
  QSize resizeFrom;
  QSize resizeTo;
  /// The content geometry leaving this stage, which the next stage's source must
  /// equal (or equal the `from` of its own pre-resize).
  QSize outputContentGeometry;
};

/// The ordered transition plan for a resolved plan, or a named refusal. `frames`
/// is the number of captured frames supplied. Pure: no file, no process, no
/// device.
bool planStageInputs(const PreparationPlan& plan, int frameCount, QVector<StageInputPlan>* out,
                     QString* reasonKey = nullptr, QString* reason = nullptr);

/// The source's display geometry with `frame`'s crop and rotation applied, or a
/// named refusal. A processed frame is passed through unchanged only when it is
/// already the display size. Pure.
bool applySourceGeometryToFrame(const ReferenceFrame& frame, const SourceGeometry& source,
                                FrameFixture* out, QString* reasonKey = nullptr,
                                QString* reason = nullptr);

/// The original's Spline36 separable resample (zimg's `compute_filter`, the
/// VapourSynth resize the reference uses): double-precision weight tables,
/// kernel stretched by 1/scale on downscale, positions mirrored then clamped,
/// weights normalised, horizontal and vertical accumulation in float32, fp16 in
/// and out. Pure and deterministic.
bool resampleFp16Planar(const QByteArray& source, const QSize& from, const QSize& to, int channels,
                        QByteArray* out, QString* reasonKey = nullptr, QString* reason = nullptr);

/// The eleven-channel RIFE input the original builds: channels 0-2 are `a`, 3-5
/// are `b`, 6 is the timestep, 7/8 are `2x/(w-1)-1` / `2y/(h-1)-1` over the
/// padded plane, and 9/10 are the constant multipliers `2/(w-1)` / `2/(h-1)`.
/// `a` and `b` are three-channel planar fp16 at `content`; the result is eleven
/// planes at `padded`, with the content placed at (`padL`, `padT`) and the border
/// black, exactly the original's centred pad. Pure.
bool packRifeElevenChannel(const QByteArray& a, const QByteArray& b, const QSize& content,
                           int padL, int padT, const QSize& padded, double timestep,
                           QByteArray* out, QString* reasonKey = nullptr, QString* reason = nullptr);

/// Cuts the content window out of a padded planar tensor: the crop the original
/// applies to a temporal stage's output before the next stage sees it. Pure.
bool cropFp16Planar(const QByteArray& padded, const QSize& paddedSize, int padL, int padT,
                    const QSize& content, int channels, QByteArray* out, QString* reasonKey = nullptr,
                    QString* reason = nullptr);

/// The timestep of the first intermediate frame a temporal factor produces:
/// `den/num`, which is 0.5 for the 2x case the original documents. Refused
/// (absent) when the factor does not put one strictly inside (0, 1), because a
/// 1:1 "interpolation" has no intermediate frame to build.
std::optional<double> rifeFirstTimestep(const Rational& factor);

/// Builds the whole lineage by running the real `mikai_native_inspect` once per
/// stage, threading each stage's genuine output into the next stage's input.
///
/// `references` is written only on success, one entry per stage in stage order;
/// a failure leaves it untouched and names the cause in `reasonKey`/`reason`,
/// which are cleared on success. A cancelled run reports
/// `native.prepare.cancelled` and sends nothing. The scratch directory and
/// everything written into it are left for the caller to keep until the outer
/// builder has finished with the referenced files.
bool buildPlanReferenceLineage(const PlanReferenceRequest& request,
                               QVector<BuilderStageReference>* references,
                               QString* reasonKey = nullptr, QString* reason = nullptr);

}  // namespace native
}  // namespace mplayer
