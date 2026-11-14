#pragma once

namespace mplayer::quick {

struct ClipSelection {
    double windowStart = 0.0;
    double windowEnd = 0.0;
    double start = 0.0;
    double end = 0.0;

    bool valid() const;
    double length() const { return end - start; }
};

enum class ClipBoundary { Both, Start, End };

ClipSelection initialClipSelection(double duration, double anchor);
ClipSelection boundedClipSelection(ClipSelection selection, double start, double end,
                                   ClipBoundary boundary = ClipBoundary::Both);
ClipSelection shiftedClipSelection(ClipSelection selection, double delta);

}
