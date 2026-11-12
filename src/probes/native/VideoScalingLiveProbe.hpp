#pragma once

#include <QString>

namespace mikai::backend {
class MikaiClient;
}

namespace mikai::app {
class Runner;
}

namespace mikai::probes {

/// Inputs the live video-scaling smoke needs from the probe entry point: a real
/// media file, the settings store this process already owns, its own executable
/// (for the fresh-process store read) and an optional visible hold.
struct VideoScalingLiveOptions {
    QString mediaPath;
    QString settingsRoot;
    QString selfPath;
    /// Absolute path of the persistent evidence JSON. Screenshots are written
    /// beside it, because the launcher deletes the probe's working directory.
    QString evidencePath;
    /// Keep the finished player visible this long so a human (or a read-only
    /// desktop capture) can observe it. 0 ends the run immediately.
    int holdMs = 0;
};

/// Runs a visible check through the real PlayerController, PlayerWindow and
/// VideoSession: Settings Save/Cancel, in-scene player flyout rows, paused
/// adoption, same-window/same-session PiP and a fresh-process store read. It
/// shows only this process's own windows and never activates another
/// application, moves the pointer or sends global input.
///
/// Returns 0 only when every check passed and the evidence file was written.
int runVideoScalingLiveSmoke(mikai::backend::MikaiClient& client,
                             mikai::app::Runner& runner,
                             const VideoScalingLiveOptions& options);

}  // namespace mikai::probes
