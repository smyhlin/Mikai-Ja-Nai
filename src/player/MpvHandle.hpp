// All calls go through the process-wide table MpvApi selected, cached once at
// construction, so the module cannot be swapped underneath a live handle.

#pragma once

#include <mpv/client.h>

#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "MpvApi.hpp"

namespace mplayer {

// Flattened out of mpv's tagged union so a view can read it directly.
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

// Carries the property name: an error here is almost always "this property
// could not be set".
class MpvError : public std::runtime_error {
public:
    MpvError(std::string what, std::string property = {})
        : std::runtime_error(std::move(what)), property_(std::move(property)) {}

    const std::string& property() const noexcept { return property_; }

private:
    std::string property_;
};

// mpv reclaims reply storage on the next wait_event, so a caller must keep a
// deep copy; a hand-built node must never go to mpv_free_node_contents.
struct NodeValue {
    mpv_format format = MPV_FORMAT_NONE;
    bool flag = false;
    int64_t integer = 0;
    double number = 0.0;
    std::string text;
    // Raw payload of a MPV_FORMAT_BYTE_ARRAY member. Kept apart from `text`
    // because a screenshot's pixels are not a string and must not be decoded as
    // one: the size is the byte count mpv declared.
    std::string bytes;
    std::vector<NodeValue> list;
    std::vector<std::pair<std::string, NodeValue>> map;
};

// Every payload field is an owned copy: nothing points into mpv storage and
// nothing has to be freed by the caller.
struct AsyncReply {
    int error = 0;
    mpv_format format = MPV_FORMAT_NONE;
    bool flag = false;
    int64_t integer = 0;
    double number = 0.0;
    std::string text;
    NodeValue node;

    bool ok() const noexcept { return error == 0; }
};

// value is the reply_userdata token, unique within one core - but two cores
// both start at 1, so it cannot name a request alone. identity is the
// process-wide core token and epoch the source at issue, so a cancel or a late
// reply is recognised as belonging to this handle's *current* core and source.
struct AsyncRequestId {
    uint64_t value = 0;
    uint64_t identity = 0;
    uint64_t epoch = 0;
    bool valid() const noexcept { return value != 0; }
    friend bool operator==(AsyncRequestId, AsyncRequestId) = default;
};

class MpvHandle {
public:
    // extraOptions are applied BEFORE mpv_initialize: that is where mpv must
    // learn vo=libmpv or it picks a default Video Output and never feeds the
    // render context. Throws if no runtime was selected through MpvApi.
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

    // Synchronous reads take the mpv core lock: nothing that must stay
    // responsive while native work holds that lock may call them. Native status
    // arrives through getPropertyAsync and the observers below.
    double getNumber(const std::string& name) const;
    bool getFlag(const std::string& name) const;
    std::string getString(const std::string& name) const;

    void setNumber(const std::string& name, double value);
    void setFlag(const std::string& name, bool value);
    void setString(const std::string& name, const std::string& value);

    // ── asynchronous transport ───────────────────────────────────────────

    // The callback runs on the thread that calls drainEvents. Returns an invalid
    // id if the request could not be issued.
    AsyncRequestId commandAsync(std::vector<std::string> args,
                                std::function<void(const AsyncReply&)> callback);
    // The node is marshalled into a plain arena for the duration of the call;
    // mpv copies what it needs.
    AsyncRequestId commandNodeAsync(const NodeValue& args,
                                    std::function<void(const AsyncReply&)> callback);
    // Does not block on the core lock. NODE/NODE_ARRAY/NODE_MAP replies arrive
    // as a deep copy owned by the AsyncReply.
    AsyncRequestId getPropertyAsync(const std::string& name, mpv_format format,
                                    std::function<void(const AsyncReply&)> callback);
    // Only an id minted by this handle against its current core and source is
    // accepted; one from another core or a superseded source cannot retire an
    // unrelated request. The reply still lands and is discarded.
    void cancelAsync(AsyncRequestId id);
    // A caller that supersedes the source or core cancels here rather than
    // relying on the destructor, so a late reply cannot run against moved-on state.
    void cancelAllAsync();
    size_t pendingAsyncCount() const noexcept;
    // Human-readable text for an mpv error code. A refused asynchronous command
    // carries only its code, so a caller that must name the refusal rather than
    // report a generic one has no other way to read the table's own text.
    std::string describeError(int error) const;

    // Monotonic, never reused and preserved through moves, so two handles can
    // never be confused even when their request numbers coincide.
    uint64_t coreIdentity() const noexcept { return identity_; }
    // Bumped whenever the source or core changes, so a reply carrying the old
    // value is recognised as stale rather than delivered.
    uint64_t sourceEpoch() const noexcept { return epoch_; }
    // Advances the source epoch and drops every outstanding callback: those
    // requests belonged to the source being replaced. loadFile and stop call
    // this before touching the source; others call it when they replace the core.
    void invalidateAsyncEpoch();

    // Drains the event queue, dispatching to the sink and observers as events
    // arrive. Safe to call often.
    void drainEvents();

    // The pointer is valid only for the duration of the call: mpv reclaims the
    // event and any node it owns as soon as the next waitEvent runs.
    void setEventSink(std::function<void(mpv_event*)> sink);

    // Fires during drainEvents on the calling thread. format must be the one the
    // callback is written against, because mpv delivers exactly what was asked
    // for. The id is never reused, so a late event cannot reach a new observer.
    uint64_t observe(const std::string& name, mpv_format format,
                     std::function<void(const Property&)> fn);
    // Unknown ids are ignored, which is what a double unobserve looks like.
    void unobserve(uint64_t id);
    void unobserveAll();

    static uint64_t apiVersion();

private:
    // mpv_command is synchronous and returns an mpv_error directly, so there is
    // no event to inspect here.
    void require(int error, const char* what, const std::string& property = {});
    std::string errorText(int error) const;

    // Drops anything whose correlation key does not name a request this handle
    // still owns.
    void dispatchAsyncReply(const mpv_event* event);
    AsyncRequestId registerAsync(std::function<void(const AsyncReply&)> callback);
    void dispatchPropertyChange(const mpv_event_property* change, uint64_t replyUserData);

    struct Observer {
        std::string name;
        std::function<void(const Property&)> fn;
    };
    struct Pending {
        std::function<void(const AsyncReply&)> callback;
        uint64_t identity = 0;
        uint64_t epoch = 0;
    };

    // Never null for a constructed handle; set once and never re-read.
    const MpvApiInstance* api_ = nullptr;
    mpv_handle* handle_ = nullptr;
    std::function<void(mpv_event*)> sink_;
    std::unordered_map<uint64_t, Observer> observers_;
    std::unordered_map<uint64_t, Pending> pending_;
    uint64_t nextObserverId_ = 1;
    uint64_t nextRequestId_ = 1;
    // Zero only for a moved-from handle; a live handle's token is never reused.
    uint64_t identity_ = 0;
    // Bumped when the source or core changes or is replaced, so a reply for the
    // previous core or source is recognised as stale.
    uint64_t epoch_ = 1;
};

}  // namespace mplayer