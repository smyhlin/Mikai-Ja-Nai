#include "ClipSelection.hpp"
#include "../shell/ClipExport.hpp"

#include <algorithm>
#include <cmath>

namespace mplayer::quick {
namespace {
constexpr double kMinimumClipDuration = 0.1;
constexpr double kWindowRadius = 120.0;
}

bool ClipSelection::valid() const {
    return std::isfinite(windowStart) && std::isfinite(windowEnd) &&
           std::isfinite(start) && std::isfinite(end) && windowStart >= 0.0 &&
           start >= windowStart && end <= windowEnd &&
           length() >= kMinimumClipDuration - 1e-9 &&
           length() <= shell::kMaximumClipDuration + 1e-9;
}

ClipSelection initialClipSelection(double duration, double anchor) {
    if (!std::isfinite(duration) || duration < kMinimumClipDuration) return {};
    const double center = std::clamp(std::isfinite(anchor) ? anchor : 0.0, 0.0, duration);
    ClipSelection result;
    result.windowStart = std::max(0.0, center - kWindowRadius);
    result.windowEnd = std::min(duration, center + kWindowRadius);
    result.start = std::clamp(center - 5.0, result.windowStart,
                              result.windowEnd - kMinimumClipDuration);
    result.end = std::min(result.windowEnd, result.start + 10.0);
    return result;
}

ClipSelection boundedClipSelection(ClipSelection selection, double start, double end,
                                   ClipBoundary boundary) {
    if (!selection.valid() || !std::isfinite(start) || !std::isfinite(end)) return selection;
    const double first = selection.windowStart;
    const double last = selection.windowEnd;
    start = std::clamp(start, first, last - kMinimumClipDuration);
    end = std::clamp(end, first + kMinimumClipDuration, last);
    if (end - start < kMinimumClipDuration) {
        if (boundary == ClipBoundary::End) start = end - kMinimumClipDuration;
        else end = start + kMinimumClipDuration;
    }
    if (end - start > shell::kMaximumClipDuration) {
        if (boundary == ClipBoundary::End) start = end - shell::kMaximumClipDuration;
        else end = start + shell::kMaximumClipDuration;
    }
    selection.start = start;
    selection.end = end;
    return selection;
}

ClipSelection shiftedClipSelection(ClipSelection selection, double delta) {
    if (!selection.valid() || !std::isfinite(delta)) return selection;
    const double length = selection.length();
    selection.start = std::clamp(selection.start + delta, selection.windowStart,
                                 selection.windowEnd - length);
    selection.end = selection.start + length;
    return selection;
}

}
