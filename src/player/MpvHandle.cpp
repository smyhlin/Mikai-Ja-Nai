#include "MpvHandle.hpp"

#include <stdexcept>
#include <utility>

namespace mplayer {

// ── construction ──────────────────────────────────────────────────────────

MpvHandle::MpvHandle(const std::vector<std::pair<std::string, std::string>>& extraOptions) {
    handle_ = mpv_create();
    if (handle_ == nullptr) throw MpvError("mpv_create returned null");
    for (const auto& [name, value] : extraOptions) {
        int rc = mpv_set_option_string(handle_, name.c_str(), value.c_str());
        if (rc < 0) {
            std::string err = mpv_error_string(rc);
            mpv_terminate_destroy(handle_);
            handle_ = nullptr;
            throw MpvError("mpv_set_option_string " + name + ": " + err);
        }
    }
    // mpv_create only allocates; the core stays uninitialised until this call,
    // and every property read answers "core not initialized" before it.
    int rc = mpv_initialize(handle_);
    if (rc < 0) {
        std::string err = mpv_error_string(rc);
        mpv_terminate_destroy(handle_);
        handle_ = nullptr;
        throw MpvError("mpv_initialize: " + err);
    }
    mpv_request_log_messages(handle_, "info");
}

MpvHandle::~MpvHandle() {
    if (handle_ != nullptr) mpv_terminate_destroy(handle_);
}

MpvHandle::MpvHandle(MpvHandle&& other) noexcept
    : handle_(std::exchange(other.handle_, nullptr)),
      sink_(std::move(other.sink_)),
      observers_(std::move(other.observers_)) {}

MpvHandle& MpvHandle::operator=(MpvHandle&& other) noexcept {
    if (this != &other) {
        if (handle_ != nullptr) mpv_terminate_destroy(handle_);
        handle_ = std::exchange(other.handle_, nullptr);
        sink_ = std::move(other.sink_);
        observers_ = std::move(other.observers_);
    }
    return *this;
}

// ── error helper ───────────────────────────────────────────────────────────

void MpvHandle::require(int error, const char* what, const std::string& property) {
    if (error < 0)
        throw MpvError(std::string(what) + ": " + mpv_error_string(error), property);
}

// ── playback commands ──────────────────────────────────────────────────────

void MpvHandle::loadFile(const std::string& url) {
    const char* cmd[] = {"loadfile", url.c_str(), nullptr};
    require(mpv_command(handle_, cmd), "loadfile", url);
}

void MpvHandle::seek(double seconds) {
    std::string at = std::to_string(seconds);
    // mpv_command takes a NULL-terminated array. Without the terminator mpv
    // reads whatever follows the last pointer as a fourth argument and the
    // process dies on the first seek.
    const char* cmd[] = {"seek", at.c_str(), "absolute", nullptr};
    require(mpv_command(handle_, cmd), "seek");
}

void MpvHandle::setPaused(bool paused)       { setFlag("pause", paused); }
void MpvHandle::setSpeed(double speed)       { setNumber("speed", speed); }
void MpvHandle::setVolume(double volume)     { setNumber("volume", volume); }
void MpvHandle::setMuted(bool muted)         { setFlag("mute", muted); }
void MpvHandle::stop() { const char* cmd[] = {"stop", nullptr}; mpv_command(handle_, cmd); }

// ── properties ─────────────────────────────────────────────────────────────

double MpvHandle::getNumber(const std::string& name) const {
    double v = 0.0;
    int rc = mpv_get_property(handle_, name.c_str(), MPV_FORMAT_DOUBLE, &v);
    if (rc < 0) throw MpvError("getNumber: " + std::string(mpv_error_string(rc)), name);
    return v;
}

bool MpvHandle::getFlag(const std::string& name) const {
    int v = 0;
    int rc = mpv_get_property(handle_, name.c_str(), MPV_FORMAT_FLAG, &v);
    if (rc < 0) throw MpvError("getFlag: " + std::string(mpv_error_string(rc)), name);
    return v != 0;
}

std::string MpvHandle::getString(const std::string& name) const {
    char* v = mpv_get_property_string(handle_, name.c_str());
    if (v == nullptr) throw MpvError("getString: null", name);
    std::string s(v);
    mpv_free(v);
    return s;
}

void MpvHandle::setNumber(const std::string& name, double value) {
    int rc = mpv_set_property(handle_, name.c_str(), MPV_FORMAT_DOUBLE, &value);
    if (rc < 0) throw MpvError("setNumber: " + std::string(mpv_error_string(rc)), name);
}

void MpvHandle::setFlag(const std::string& name, bool value) {
    int flag = value ? 1 : 0;
    int rc = mpv_set_property(handle_, name.c_str(), MPV_FORMAT_FLAG, &flag);
    if (rc < 0) throw MpvError("setFlag: " + std::string(mpv_error_string(rc)), name);
}

void MpvHandle::setString(const std::string& name, const std::string& value) {
    int rc = mpv_set_property_string(handle_, name.c_str(), value.c_str());
    if (rc < 0) throw MpvError("setString: " + std::string(mpv_error_string(rc)), name);
}

// ── events ─────────────────────────────────────────────────────────────────

std::vector<mpv_event*> MpvHandle::drainEvents() {
    std::vector<mpv_event*> events;
    while (true) {
        mpv_event* e = mpv_wait_event(handle_, 0);
        if (e == nullptr || e->event_id == MPV_EVENT_NONE) break;
        events.push_back(e);
        if (sink_) sink_(e);
        if (e->event_id == MPV_EVENT_PROPERTY_CHANGE) {
            auto* change = static_cast<mpv_event_property*>(e->data);
            if (change == nullptr || change->name == nullptr) continue;
            const std::string name = change->name;
            for (auto& [pname, fn] : observers_) {
                if (pname != name) continue;
                Property p;
                p.name = name;
                p.format = change->format;
                switch (p.format) {
                    case MPV_FORMAT_DOUBLE:
                        p.number = change->data ? *static_cast<double*>(change->data) : 0.0; break;
                    case MPV_FORMAT_INT64:
                        p.number = change->data ? static_cast<double>(*static_cast<int64_t*>(change->data)) : 0.0;
                        break;
                    case MPV_FORMAT_FLAG:
                        p.flag = change->data && *static_cast<int*>(change->data) != 0; break;
                    case MPV_FORMAT_STRING:
                        p.text = change->data && *static_cast<char**>(change->data)
                                     ? *static_cast<char**>(change->data)
                                     : "";
                        break;
                    default: break;
                }
                fn(p);
            }
        }
    }
    return events;
}

void MpvHandle::setEventSink(std::function<void(mpv_event*)> sink) {
    sink_ = std::move(sink);
}

void MpvHandle::observe(const std::string& name, std::function<void(const Property&)> fn) {
    observers_.emplace_back(name, std::move(fn));
    mpv_observe_property(handle_, observers_.size() - 1, name.c_str(), MPV_FORMAT_NODE_ARRAY);
}

void MpvHandle::unobserve(const std::string& name) {
    for (auto it = observers_.begin(); it != observers_.end(); ++it) {
        if (it->first == name) { observers_.erase(it); return; }
    }
}

int64_t MpvHandle::apiVersion() { return mpv_client_api_version(); }

}  // namespace mplayer
