// The raw playback numbers a core reported for the current source, and the two
// consumer-protocol parsers that turn its replies into them.
//
// Core-facing, not GUI: the POD and the parsers carry no widget, no Qt GUI and
// no surface state, so both can be read from a plain transport test as well as
// from VideoSurface. Everything here is pure - same reply in, same answer out.

#pragma once

#include <cstdint>
#include <optional>

namespace mplayer {

// Defined in MpvHandle.hpp; only referenced here.
struct AsyncReply;

/// Playback observations for the current source, separate from HUD values.
/// Core-reported numbers are present only while the core answered for them; the
/// render counter counts successful surface renders of new mpv output frames.
struct PlaybackDiagnostics {
    std::optional<double> estimatedVfFps;     ///< raw, no container fallback
    std::optional<int64_t> decoderDropCount;  ///< decoder-frame-drop-count
    std::optional<int64_t> voDropCount;       ///< frame-drop-count (VO drops)
    uint64_t renderedOutputFrames = 0;  ///< successful mpv-rendered new-frame updates
    /// nullopt = vf chain unread; true/false = what the live chain holds.
    std::optional<bool> nativeFilterPresent;
    std::optional<int> vfCount;  ///< chain length when read
};

/// The exact integer a NODE counter reply carries. Only a reply whose node is
/// MPV_FORMAT_INT64 avoids the double detour; an unavailable or otherwise-typed
/// reply is unknown, never a substituted zero.
std::optional<int64_t> exactInteger(const AsyncReply& reply);

/// Reads a live `vf` chain reply. Returns false when the reply's shape cannot be
/// read, so the caller keeps the chain unknown rather than claiming the filter
/// absent. On success `nativePresent` states whether the fork's filter name or
/// its label appears, and `chainLength` is the entry count (0 is a real, empty
/// chain and does prove absence).
bool summarizeVfChain(const AsyncReply& reply, bool* nativePresent, int* chainLength);

}  // namespace mplayer
