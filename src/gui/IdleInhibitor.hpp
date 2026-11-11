#pragma once

#include <memory>
#include <vector>

// Keeps the session from blanking the screen while something is playing.
//
// A surface owns a reason, not the inhibitor: the main player and a floating
// picture-in-picture window can both be running, and the session has to stay
// awake until the last of them pauses or closes.

namespace mikai::gui {

class IdleInhibitor {
public:
    IdleInhibitor();
    ~IdleInhibitor();

    IdleInhibitor(const IdleInhibitor&) = delete;
    IdleInhibitor& operator=(const IdleInhibitor&) = delete;

    // `owner` identifies the reason. Repeating the same state for the same owner
    // does nothing, and an owner may be released explicitly when it goes away.
    void setActiveFor(const void* owner, bool active);
    void release(const void* owner);

    bool active() const;

private:
    struct Platform;
    void apply(bool active);
    bool inhibitThroughLogind();
    bool inhibitThroughScreenSaver();
    void releaseScreenSaver();

    std::unique_ptr<Platform> platform_;
    // Unbounded: an owner is a reason, not a slot, so the 9th owner is
    // tracked exactly like the 2nd — the old fixed array silently dropped
    // it. Only 2 owners exist today (main player + PiP float).
    std::vector<const void*> owners_;
};

// One per process, so every playback surface shares the same session request.
IdleInhibitor& idleInhibitor();

}  // namespace mikai::gui
