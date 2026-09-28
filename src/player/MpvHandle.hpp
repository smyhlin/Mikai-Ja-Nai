// RAII wrapper around a libmpv client handle.
//
// Every mpv entry point that can fail is checked here, so the rest of the
// player never sees a raw handle. Event delivery is edge-triggered through
// mpv_set_wakeup_callback: mpv signals "something changed", we drain
// mpv_wait_event(0) on the thread that owns the window.

#pragma once

#include <mpv/client.h>

#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace mplayer {

// A property value as mpv hands it back, flattened to something a view can
// read without knowing mpv's tagged union.
struct Property {
    std::string name;
    // Type tag from mpv_format; kept so a double is never read as a string.
    mpv_format format = MPV_FORMAT_NONE;
    double number = 0.0;
    bool flag = false;
    std::string text;

    bool isNone() const { return format == MPV_FORMAT_NONE; }
    bool isNumber() const { return format == MPV_FORMAT_DOUBLE || format == MPV_FORMAT_INT64; }
    bool isFlag() const { return format == MPV_FORMAT_FLAG; }
    bool isText() const { return format == MPV_FORMAT_STRING || format == MPV_FORMAT_OSD_STRING; }
};

// Thrown when mpv reports a failure. Carries the property name because an
// error is almost always "this property could not be set".
class MpvError : public std::runtime_error {
public:
    MpvError(std::string what, std::string property = {})
        : std::runtime_error(std::move(what)), property_(std::move(property)) {}

    const std::string& property() const noexcept { return property_; }

private:
    std::string property_;
};

class MpvHandle {
public:
    // optionString is passed verbatim to mpv_create; empty means defaults.
    // extraOptions are applied BEFORE mpv_initialize, which is where mpv
    // must learn vo=libmpv or it will pick a default Video Output instead of
    // feeding the render context frames.
    explicit MpvHandle(const std::vector<std::pair<std::string, std::string>>&
                           extraOptions = {});
    ~MpvHandle();

    MpvHandle(const MpvHandle&) = delete;
    MpvHandle& operator=(const MpvHandle&) = delete;
    MpvHandle(MpvHandle&& other) noexcept;
    MpvHandle& operator=(MpvHandle&& other) noexcept;

    mpv_handle* get() const noexcept { return handle_; }
    explicit operator bool() const noexcept { return handle_ != nullptr; }

    // The raw handle, for mpv_render_context_create. Not owned here.
    mpv_handle* raw() const noexcept { return handle_; }

    void loadFile(const std::string& url);
    void seek(double seconds);
    void setPaused(bool paused);
    void setSpeed(double speed);
    void setVolume(double volume);
    void setMuted(bool muted);
    void stop();

    double getNumber(const std::string& name) const;
    bool getFlag(const std::string& name) const;
    std::string getString(const std::string& name) const;
    void setNumber(const std::string& name, double value);
    void setFlag(const std::string& name, bool value);
    void setString(const std::string& name, const std::string& value);

    // Drain the event queue. Safe to call often; returns the events handled.
    std::vector<mpv_event*> drainEvents();

    // Called for every event the queue yields, before it is dropped.
    void setEventSink(std::function<void(mpv_event*)> sink);

    // Property observers fire during drainEvents, on the calling thread.
    void observe(const std::string& name, std::function<void(const Property&)> fn);
    void unobserve(const std::string& name);

    static int64_t apiVersion();

private:
    // mpv_command is synchronous and returns an mpv_error directly; only the
    // async variant hands back an event, so there is no event to inspect here.
    void require(int error, const char* what, const std::string& property = {});

    mpv_handle* handle_ = nullptr;
    std::function<void(mpv_event*)> sink_;
    std::vector<std::pair<std::string, std::function<void(const Property&)>>> observers_;
};

}  // namespace mplayer
