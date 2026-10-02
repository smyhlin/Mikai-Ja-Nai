#pragma once

#include <cstdio>

#include "native/NativeRuntimeSelection.hpp"

namespace mplayer::probes {

inline bool selectStandardRuntime() {
    const auto selection = native::selectStandardRuntime();
    if (selection.loaded) {
        return true;
    }
    // The refusal names whichever check rejected the module - a check that is
    // one message away from being silently read as "no mpv on this machine".
    std::fprintf(stderr, "Standard runtime unavailable: %s\n", qPrintable(selection.reason));
    return false;
}

}
