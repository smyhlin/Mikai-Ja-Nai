#pragma once

#include <QImage>

/// One implementation of the comparison's pixel measurements, in one unit:
/// intensity levels, 0-255. The Compare window's gate and the probes read the
/// same numbers, so a probe verdict and a shipped verdict cannot disagree about
/// what "the picture changed" means.
namespace mplayer::shell {

/// Mean absolute per-channel difference inside the picture band.
/// Returns -1 when either image is null or the geometry differs: two pictures
/// of different sizes are not comparable, and silently returning 0 turned that
/// into "the model did nothing".
double meanDifference(const QImage& a, const QImage& b);

/// Mean of |Laplacian| / 4 over the picture band, in levels. A near-uniform
/// picture scores well under one level, and no model effect can be measured
/// there: the difference of two edge energies is bounded by twice the mean
/// luma difference, so a flat reference turns every model into "no change".
double edgeEnergy(const QImage& image);

/// `edgeEnergy` of a copy scaled down to `kDetailReferenceEdge` on its longest
/// side, so the measure describes the picture rather than the canvas it was
/// rendered into.
double referenceDetail(const QImage& image);

/// Smallest reference detail a verdict may rest on, measured on a copy whose
/// longest side is at most `kDetailReferenceEdge`. Edge energy is a per-pixel
/// gradient: upscaling a frame to a 4x canvas halves it, so an absolute floor on
/// the canvas would refuse every real picture at 4x while accepting none of the
/// 1x ones. Normalising the size first makes the measure comparable across
/// canvases.
inline constexpr int kDetailReferenceEdge = 1024;
inline constexpr double kMinimumReferenceDetail = 0.5;
/// Smallest reference contrast a verdict may rest on (levels). A black or
/// near-uniform card still carries about one level of dither noise, which is
/// enough laplacian energy to pass the edge floor: the range is what separates
/// "a picture" from "a flat card with dither".
inline constexpr int kMinimumReferenceRange = 24;

/// True when the picture band holds no image at all: an all-black or all-white
/// frame, or one whose values never leave a couple of levels.
bool carriesNoPicture(const QImage& image);

/// Highest minus lowest level inside the picture band, over all channels: the
/// contrast a verdict's detail rests on, independent of dither noise.
int pictureRange(const QImage& image);
/// Smallest model effect that is a picture change rather than a colour-matrix
/// round trip (levels). An NV12 round trip plus one resample measures ~0.02.
inline constexpr double kMinimumModelEffectLevels = 0.25;
/// A measured same-pipeline noise floor is amplified before it is compared
/// against, so a render that is only barely repeatable cannot pass.
inline constexpr double kNoiseFloorMultiplier = 4.0;

struct EffectVerdict {
    enum class Kind {
        Ok,
        GeometryMismatch,
        FlatFrame,
        /// The model's own output carries no picture: an inference failure that
        /// emits black must never be shown as a result.
        BlankModel,
        NoEffect,
        NoBaseline,
    };
    Kind kind = Kind::NoBaseline;
    /// Mean difference between the baseline and the model picture, levels.
    double effect = -1.0;
    /// The threshold the effect had to exceed.
    double floor = -1.0;
    /// Detail of the baseline, measured by `referenceDetail` (scale-normalised).
    double referenceEdges = -1.0;
};

/// Judges a model picture against a baseline rendered through the *same*
/// pipeline with the stage removed. `noiseFloor` is the difference between two
/// renders of that baseline; pass 0 when it was not measured.
EffectVerdict judgeEffect(const QImage& baseline, const QImage& model,
                          double noiseFloor = 0.0);

}  // namespace mplayer::shell
