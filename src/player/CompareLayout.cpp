#include "CompareLayout.hpp"

#include <QColor>
#include <cmath>
#include <cstring>

namespace mplayer::shell {

const char* modeKey(CompareMode mode) {
    switch (mode) {
        case CompareMode::SplitH: return "split_h";
        case CompareMode::SplitV: return "split_v";
        case CompareMode::SideBySide: return "side_by_side";
        case CompareMode::Flicker: return "flicker";
        case CompareMode::Difference: return "difference";
    }
    return "side_by_side";
}

bool modeFromKey(const char* key, CompareMode& out) {
    if (key == nullptr) return false;
    struct Entry {
        const char* key;
        CompareMode mode;
    };
    static const Entry table[] = {
        {"split_h", CompareMode::SplitH},
        {"split_v", CompareMode::SplitV},
        {"side_by_side", CompareMode::SideBySide},
        {"flicker", CompareMode::Flicker},
        {"difference", CompareMode::Difference},
    };
    for (const Entry& e : table) {
        if (std::strcmp(e.key, key) == 0) {
            out = e.mode;
            return true;
        }
    }
    return false;
}

const char* modeLabel(CompareMode mode) {
    switch (mode) {
        case CompareMode::SplitH: return "Split H";
        case CompareMode::SplitV: return "Split V";
        case CompareMode::SideBySide: return "Side-by-side";
        case CompareMode::Flicker: return "Flicker";
        case CompareMode::Difference: return "Difference";
    }
    return "Side-by-side";
}

CompareLayout buildLayout(CompareMode mode, const QSize& size, double split, bool flickerShowA,
                          int heldSide) {
    CompareLayout out;
    if (size.width() <= 0 || size.height() <= 0) return out;

    const double s = std::clamp(split, 0.0, 1.0);

    // Space hides the second side everywhere alike: A takes the whole frame and
    // the divider and the second name go with it. Geometry and the stored mode
    // are untouched, so releasing restores everything, zoom and pan included.
    if (heldSide >= 0) {
        out.a.visible = true;
        out.a.clipFull(size);
        out.heldSide = heldSide;
        out.scene = size;
        return out;
    }

    switch (mode) {
        case CompareMode::SplitH: {
            const int cut = static_cast<int>(size.height() * s);
            out.a.visible = true;
            out.a.clipX = 0;
            out.a.clipY = 0;
            out.a.clipW = size.width();
            out.a.clipH = cut;
            out.a.hasClip = true;
            out.b.visible = true;
            out.b.clipX = 0;
            out.b.clipY = cut;
            out.b.clipW = size.width();
            out.b.clipH = size.height() - cut;
            out.b.hasClip = true;
            out.divider.visible = true;
            out.divider.x0 = 0;
            out.divider.y0 = cut;
            out.divider.x1 = size.width();
            out.divider.y1 = cut;
            break;
        }

        case CompareMode::SplitV: {
            const int cut = static_cast<int>(size.width() * s);
            out.a.visible = true;
            out.a.clipX = 0;
            out.a.clipY = 0;
            out.a.clipW = cut;
            out.a.clipH = size.height();
            out.a.hasClip = true;
            out.b.visible = true;
            out.b.clipX = cut;
            out.b.clipY = 0;
            out.b.clipW = size.width() - cut;
            out.b.clipH = size.height();
            out.b.hasClip = true;
            out.divider.visible = true;
            out.divider.x0 = cut;
            out.divider.y0 = 0;
            out.divider.x1 = cut;
            out.divider.y1 = size.height();
            break;
        }

        case CompareMode::SideBySide:
            out.a.visible = true;
            out.a.clipFull(size);
            out.b.visible = true;
            out.b.clipFull(size);
            out.b.x = size.width() + kSideBySideGap;
            out.scene = QSize(size.width() * 2 + kSideBySideGap, size.height());
            return out;  // the scene is wider; do not overwrite it below

        case CompareMode::Flicker:
            // One side at full strength; the timer decides which.
            if (flickerShowA) {
                out.a.visible = true;
                out.a.clipFull(size);
            } else {
                out.b.visible = true;
                out.b.clipFull(size);
            }
            break;

        case CompareMode::Difference:
            out.difference.visible = true;
            break;
    }

    if (out.scene.isEmpty()) out.scene = size;
    return out;
}

void jetColor(double value, uchar rgb[3]) {
    const double v = std::clamp(value, 0.0, 1.0);
    const double r = std::clamp(1.5 - std::fabs(4.0 * v - 3.0), 0.0, 1.0);
    const double g = std::clamp(1.5 - std::fabs(4.0 * v - 2.0), 0.0, 1.0);
    const double b = std::clamp(1.5 - std::fabs(4.0 * v - 1.0), 0.0, 1.0);
    rgb[0] = static_cast<uchar>(std::lround(r * 255.0));
    rgb[1] = static_cast<uchar>(std::lround(g * 255.0));
    rgb[2] = static_cast<uchar>(std::lround(b * 255.0));
}

namespace {

/// The peak per-channel absolute difference at one pixel; the reference takes
/// the same peak, which is why a gain of 1 is the honest picture.
inline int peakDifference(const QImage& a, const QImage& b, int x, int y) {
    const QColor ca = a.pixelColor(x, y);
    const QColor cb = b.pixelColor(x, y);
    return std::max({std::abs(ca.red() - cb.red()), std::abs(ca.green() - cb.green()),
                     std::abs(ca.blue() - cb.blue())});
}

}  // namespace

QImage differenceJet(const QImage& a, const QImage& b) {
    if (a.isNull() || b.isNull() || a.size() != b.size()) return QImage();
    const QImage left = a.convertToFormat(QImage::Format_RGB888);
    const QImage right = b.convertToFormat(QImage::Format_RGB888);
    QImage out = left;
    for (int y = 0; y < out.height(); ++y) {
        for (int x = 0; x < out.width(); ++x) {
            uchar rgb[3];
            jetColor(peakDifference(left, right, x, y) / 255.0, rgb);
            out.setPixel(x, y, qRgb(rgb[0], rgb[1], rgb[2]));
        }
    }
    return out;
}

QImage differenceGrayscale(const QImage& a, const QImage& b, int gain) {
    if (a.isNull() || b.isNull() || a.size() != b.size()) return QImage();
    const QImage left = a.convertToFormat(QImage::Format_RGB888);
    const QImage right = b.convertToFormat(QImage::Format_RGB888);
    const int g = std::max(1, gain);
    QImage out = left;
    for (int y = 0; y < out.height(); ++y) {
        for (int x = 0; x < out.width(); ++x) {
            const uchar v = static_cast<uchar>(std::min(255, peakDifference(left, right, x, y) * g));
            out.setPixel(x, y, qRgb(v, v, v));
        }
    }
    return out;
}

double meanAbsoluteDifference(const QImage& a, const QImage& b) {
    if (a.isNull() || b.isNull() || a.size() != b.size()) return 0.0;
    const QImage left = a.convertToFormat(QImage::Format_RGB888);
    const QImage right = b.convertToFormat(QImage::Format_RGB888);
    double sum = 0.0;
    qint64 counted = 0;
    for (int y = 0; y < left.height(); ++y) {
        for (int x = 0; x < left.width(); ++x) {
            const QColor ca = left.pixelColor(x, y);
            const QColor cb = right.pixelColor(x, y);
            sum += std::abs(ca.red() - cb.red());
            sum += std::abs(ca.green() - cb.green());
            sum += std::abs(ca.blue() - cb.blue());
            counted += 3;
        }
    }
    return counted > 0 ? sum / static_cast<double>(counted) : 0.0;
}

double Camera::clampedZoom() const {
    if (!(zoom > 0.0)) return 1.0;
    return std::clamp(zoom, kCompareZoomMin, kCompareZoomMax);
}

double Camera::clampedSplit() const { return std::clamp(split, 0.0, 1.0); }
double Camera::clampedFlickerHz() const { return std::clamp(flickerHz, 1.0, 30.0); }
int Camera::clampedGain() const { return std::clamp(diffGain, kDiffGainMin, kDiffGainMax); }

void Camera::zoomTo(double target, double anchorSceneX, double anchorSceneY, double viewportWidth,
                    double viewportHeight) {
    const double after = std::clamp(target > 0.0 ? target : 1.0, kCompareZoomMin, kCompareZoomMax);
    zoom = after;
    if (viewportWidth <= 0.0 || viewportHeight <= 0.0) return;
    // The anchor is a scene coordinate and stays under the viewport centre
    // across the zoom, so the pan is *derived from* the anchor:
    //   (vw/2 - panX)/zoom == anchorSceneX, which is the invariant the caller
    // depends on.
    panX = viewportWidth / 2.0 - anchorSceneX * after;
    panY = viewportHeight / 2.0 - anchorSceneY * after;
}

void Camera::zoomBySteps(int steps, double anchorSceneX, double anchorSceneY, double viewportWidth,
                         double viewportHeight) {
    const double target = clampedZoom() * std::pow(kCompareWheelStep, steps);
    zoomTo(target, anchorSceneX, anchorSceneY, viewportWidth, viewportHeight);
}

void Camera::fit(const QSize& content, double viewportWidth, double viewportHeight) {
    if (content.width() <= 0 || content.height() <= 0 || viewportWidth <= 0.0 ||
        viewportHeight <= 0.0) {
        reset();
        return;
    }
    // Never upscale past 1:1: a small frame blown up to fill a large window
    // shows interpolation artefacts, not detail.
    const double scale =
        std::min(1.0, std::min(viewportWidth / content.width(), viewportHeight / content.height()));
    zoom = scale;
    panX = (viewportWidth - content.width() * scale) / 2.0;
    panY = (viewportHeight - content.height() * scale) / 2.0;
}

void Camera::reset() {
    zoom = 1.0;
    panX = 0.0;
    panY = 0.0;
    split = 0.5;
    flickerHz = 1.0;
    diffGain = 1;
}

}  // namespace mplayer::shell
