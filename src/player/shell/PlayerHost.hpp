// The host bridge every player shell shares.
//
// These two types are the whole contract between a player window and whatever
// owns the catalogue, the credentials and the stream URL: the Widgets player
// and the native Quick player are interchangeable hosts from this header's
// point of view. They live here rather than beside one of the two shells so the
// shared types cannot drift into two copies, and so the native Quick controller
// never has to include the Widgets composition root to name them.

#pragma once

#include <QString>

namespace mplayer {
namespace shell {

/// One playable height. A quality map entry survives only when its url is a
/// non-blank string, which is what `usableQualities` enforces.
struct QualityEntry {
    int height = 0;
    QString url;
};

/// Everything a player shell cannot do for itself. Implemented by the C API
/// layer and non-owning: it must outlive the shell that holds it.
class PlayerHost {
public:
    virtual ~PlayerHost() = default;
    virtual void requestVariant(const QString& embed, const QString& label) = 0;
    virtual void requestEpisode(int index) = 0;
    virtual void requestTrackProbe(const QString& embed) = 0;
    virtual void requestCopyStreamUrl() = 0;
    virtual void playerClosed() = 0;
    /// The stream could not be played: the host re-extracts and reloads.
    virtual void playbackFailed() = 0;
};

}  // namespace shell
}  // namespace mplayer
