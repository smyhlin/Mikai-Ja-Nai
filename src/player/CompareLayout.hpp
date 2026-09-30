// The comparison layout, as one pure function.
//
// Adapted from AnimeInterp's core/compare_modes and core/camera. The whole
// picture for a given (mode, size, split, opacity) is a value here, so the
// canvas only turns it into item positions and nothing else has to know how a
// mode works. QImage replaces the reference's BGR Frame, because the host
// already hands us pictures in Qt's own byte order.
#pragma once

#include <QImage>
#include <QSize>

namespace mplayer::shell {

enum class CompareMode {
    Original,
    Processed,
    SplitH,
    SplitV,
    SideBySide,
    Overlay,
    Flicker,
    Difference
};

const char* modeKey(CompareMode mode);
bool modeFromKey(const char* key, CompareMode& out);
const char* modeLabel(CompareMode mode);

/// The gap between the two panes in Side by side, in pixels.
inline constexpr int kSideBySideGap = 24;

struct ItemState {
    bool visible = false;
    int x = 0;
    int y = 0;
    double opacity = 1.0;
    int clipX = 0;
    int clipY = 0;
    int clipW = 0;
    int clipH = 0;
    bool hasClip = false;

    void clipFull(const QSize& s) {
        clipX = 0;
        clipY = 0;
        clipW = s.width();
        clipH = s.height();
        hasClip = true;
    }
};

struct DividerState {
    bool visible = false;
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;
};

struct CompareLayout {
    ItemState a;
    ItemState b;
    ItemState difference;
    DividerState divider;
    QSize scene;
    int heldSide = -1;
};

/// `heldSide` is the Space-hold override (-1 when not held): it shows one side
/// full and hides everything else, leaving the stored mode untouched so
/// releasing restores the view, zoom and pan included.
CompareLayout buildLayout(CompareMode mode, const QSize& size, double split, double opacity,
                          bool flickerShowA, int heldSide = -1);

/// OpenCV's JET ramp, computed rather than tabulated, so the difference view
/// runs dark blue -> cyan -> green -> yellow -> dark red.
void jetColor(double value, uchar rgb[3]);

/// |A - B| at gain 1, colour-mapped. The honest picture.
QImage differenceJet(const QImage& a, const QImage& b);
/// |A - B| amplified by `gain` and shown grey. The amplified picture.
QImage differenceGrayscale(const QImage& a, const QImage& b, int gain);
/// Mean absolute difference over the whole picture, 0-255: the one number an
/// "are these different at all" question needs.
double meanAbsoluteDifference(const QImage& a, const QImage& b);

/// The reference's bounds. Deliberately wider than the player's own zoom
/// (ShellTheme's kZoomMin/kZoomMax): this is a pixel-inspection tool, and
/// clamping a 480p frame at 5x would hide the very interpolation the
/// comparison exists to look at.
inline constexpr double kCompareZoomMin = 0.05;
inline constexpr double kCompareZoomMax = 32.0;
inline constexpr double kCompareWheelStep = 1.25;
inline constexpr int kDiffGainMin = 1;
inline constexpr int kDiffGainMax = 64;

/// The whole view state. Value semantics: copy it, store it, restore it.
struct Camera {
    double zoom = 1.0;
    double panX = 0.0;
    double panY = 0.0;
    double split = 0.5;
    double opacity = 0.5;
    double flickerHz = 1.0;
    int diffGain = 1;

    double clampedZoom() const;
    double clampedSplit() const;
    double clampedOpacity() const;
    double clampedFlickerHz() const;
    int clampedGain() const;

    /// Fit `content` into a viewport, never upscaling past 1:1: a small frame
    /// blown up to a large window shows interpolation, not detail.
    /// Zoom by `steps` notches about a scene anchor, so the point under the
    /// cursor stays put. The pan is derived from the anchor, never the reverse:
    /// the caller depends on (vw/2 - panX)/zoom == anchorSceneX afterwards.
    void zoomBySteps(int steps, double anchorSceneX, double anchorSceneY, double viewportWidth,
                     double viewportHeight);
    void zoomTo(double target, double anchorSceneX, double anchorSceneY, double viewportWidth,
                double viewportHeight);

    void fit(const QSize& content, double viewportWidth, double viewportHeight);
    void reset();
};

}  // namespace mplayer::shell
