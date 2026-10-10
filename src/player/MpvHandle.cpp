#include "MpvHandle.hpp"

#include <algorithm>
#include <atomic>
#include <memory>
#include <string>
#include <utility>

namespace mplayer {
namespace {

// Process-wide source of core identity tokens: monotonic and never reused, so
// the same request number from two cores can be told apart.
std::atomic<uint64_t> nextCoreIdentity{0};

// mpv delivers the format that was requested and nothing is coerced, so every
// reader has to be written against its caller's format.
void readInto(Property& out, const mpv_event_property* change) {
    out.format = change->format;
    out.text.clear();
    out.number = 0.0;
    out.flag = false;
    switch (change->format) {
        case MPV_FORMAT_DOUBLE:
            out.number = change->data ? *static_cast<const double*>(change->data) : 0.0;
            break;
        case MPV_FORMAT_INT64:
            out.number = change->data
                             ? static_cast<double>(*static_cast<const int64_t*>(change->data))
                             : 0.0;
            break;
        case MPV_FORMAT_FLAG:
            out.flag = change->data && *static_cast<const int*>(change->data) != 0;
            break;
        case MPV_FORMAT_STRING:
        case MPV_FORMAT_OSD_STRING: {
            const char* text = nullptr;
            if (change->data != nullptr) text = *static_cast<const char* const*>(change->data);
            if (text != nullptr) out.text.assign(text);
            break;
        }
        default:
            break;
    }
}

void copyNode(const mpv_node* source, NodeValue& out) {
    if (source == nullptr) return;
    out.format = source->format;
    switch (source->format) {
        case MPV_FORMAT_STRING:
        case MPV_FORMAT_OSD_STRING:
            if (source->u.string != nullptr) out.text.assign(source->u.string);
            break;
        case MPV_FORMAT_FLAG:
            out.flag = source->u.flag;
            break;
        case MPV_FORMAT_INT64:
            out.integer = source->u.int64;
            break;
        case MPV_FORMAT_DOUBLE:
            out.number = source->u.double_;
            break;
        case MPV_FORMAT_BYTE_ARRAY:
            // Copied into its own buffer, so the payload survives the next
            // wait_event without pointing into mpv storage.
            if (source->u.ba != nullptr && source->u.ba->size > 0) {
                out.bytes.assign(reinterpret_cast<const char*>(source->u.ba->data),
                                 static_cast<size_t>(source->u.ba->size));
            }
            break;
        case MPV_FORMAT_NODE_ARRAY:
            if (source->u.list != nullptr) {
                out.list.reserve(static_cast<size_t>(source->u.list->num));
                for (int i = 0; i < source->u.list->num; ++i) {
                    out.list.emplace_back();
                    copyNode(&source->u.list->values[i], out.list.back());
                }
            }
            break;
        case MPV_FORMAT_NODE_MAP:
            // Both node containers are the same struct: a map's keys live in
            // list->keys next to list->values, there is no separate map type.
            if (source->u.list != nullptr) {
                out.map.reserve(static_cast<size_t>(source->u.list->num));
                for (int i = 0; i < source->u.list->num; ++i) {
                    const char* key = source->u.list->keys != nullptr ? source->u.list->keys[i]
                                                                      : nullptr;
                    out.map.emplace_back(key != nullptr ? std::string(key) : std::string(),
                                         NodeValue{});
                    copyNode(&source->u.list->values[i], out.map.back().second);
                }
            }
            break;
        default:
            break;
    }
}

// The pointer a property reply carries depends on the requested format: NODE
// points at an mpv_node, while NODE_ARRAY/NODE_MAP point straight at the list.
// Wrapping that list in a throwaway node lets one copier read all three.
void copyReplyNode(const mpv_event_property* property, NodeValue& out) {
    if (property == nullptr || property->data == nullptr) return;
    switch (property->format) {
        case MPV_FORMAT_NODE:
            copyNode(static_cast<const mpv_node*>(property->data), out);
            break;
        case MPV_FORMAT_NODE_ARRAY:
        case MPV_FORMAT_NODE_MAP: {
            mpv_node wrapper{};
            wrapper.format = property->format;
            wrapper.u.list = static_cast<mpv_node_list*>(property->data);
            copyNode(&wrapper, out);
            break;
        }
        default:
            break;
    }
}

// mpv copies what it needs during the call, so this storage only has to outlive
// the call - hence plain new[] and never mpv_free_node_contents. One arena owns
// every string, key and container in the tree.
struct OutgoingNode {
    mpv_node node = {};
    std::vector<std::unique_ptr<char[]>> strings;
    std::vector<std::unique_ptr<char*[]>> keyArrays;
    std::vector<std::unique_ptr<mpv_node[]>> valueArrays;
    std::vector<std::unique_ptr<mpv_node_list>> lists;

    void build(const NodeValue& value, mpv_node& target) {
        target = mpv_node{};
        target.format = value.format;
        switch (value.format) {
            case MPV_FORMAT_STRING:
            case MPV_FORMAT_OSD_STRING: {
                auto block = std::make_unique<char[]>(value.text.size() + 1);
                std::copy(value.text.begin(), value.text.end(), block.get());
                block[value.text.size()] = '\0';
                target.u.string = block.get();
                strings.push_back(std::move(block));
                break;
            }
            case MPV_FORMAT_FLAG:
                target.u.flag = value.flag;
                break;
            case MPV_FORMAT_INT64:
                target.u.int64 = value.integer;
                break;
            case MPV_FORMAT_DOUBLE:
                target.u.double_ = value.number;
                break;
            case MPV_FORMAT_NODE_ARRAY: {
                auto values = std::make_unique<mpv_node[]>(value.list.size());
                for (size_t i = 0; i < value.list.size(); ++i) build(value.list[i], values[i]);
                auto list = std::make_unique<mpv_node_list>();
                list->num = static_cast<int>(value.list.size());
                list->values = value.list.empty() ? nullptr : values.get();
                target.u.list = list.get();
                valueArrays.push_back(std::move(values));
                lists.push_back(std::move(list));
                break;
            }
            case MPV_FORMAT_NODE_MAP: {
                auto values = std::make_unique<mpv_node[]>(value.map.size());
                auto keys = std::make_unique<char*[]>(value.map.size());
                for (size_t i = 0; i < value.map.size(); ++i) {
                    build(value.map[i].second, values[i]);
                    auto key = std::make_unique<char[]>(value.map[i].first.size() + 1);
                    std::copy(value.map[i].first.begin(), value.map[i].first.end(), key.get());
                    key[value.map[i].first.size()] = '\0';
                    keys[i] = key.get();
                    strings.push_back(std::move(key));
                }
                auto list = std::make_unique<mpv_node_list>();
                list->num = static_cast<int>(value.map.size());
                list->values = value.map.empty() ? nullptr : values.get();
                list->keys = value.map.empty() ? nullptr : keys.get();
                target.u.list = list.get();
                valueArrays.push_back(std::move(values));
                keyArrays.push_back(std::move(keys));
                lists.push_back(std::move(list));
                break;
            }
            default:
                target.format = MPV_FORMAT_NONE;
                break;
        }
    }

    mpv_node* buildRoot(const NodeValue& value) {
        build(value, node);
        return &node;
    }
};

}  // namespace

// ── construction ──────────────────────────────────────────────────────────

MpvHandle::MpvHandle(const std::vector<std::pair<std::string, std::string>>& extraOptions) {
    MpvApi& api = MpvApi::instance();
    if (!api.loaded()) {
        throw MpvError("no mpv runtime is selected: MpvApi::load has to run before a handle is "
                       "created (" +
                       api.error().toStdString() + ")");
    }
    // Cached once: the table is immutable for the life of the process, so this
    // pointer stays good.
    api_ = &api.functions();

    handle_ = api_->create();
    if (handle_ == nullptr) throw MpvError("mpv_create returned null");
    // A fresh core gets a fresh, never-reused identity before anything can be
    // requested against it.
    identity_ = ++nextCoreIdentity;
    for (const auto& [name, value] : extraOptions) {
        const int rc = api_->setOptionString(handle_, name.c_str(), value.c_str());
        if (rc < 0) {
            const std::string message = "mpv_set_option_string " + name + ": " + errorText(rc);
            api_->terminateDestroy(handle_);
            handle_ = nullptr;
            throw MpvError(message);
        }
    }
    // mpv_create only allocates; the core stays uninitialised until this call,
    // and every property read answers "core not initialized" before it.
    const int rc = api_->initialize(handle_);
    if (rc < 0) {
        const std::string message = "mpv_initialize: " + errorText(rc);
        api_->terminateDestroy(handle_);
        handle_ = nullptr;
        throw MpvError(message);
    }
    developerVerboseLogs_ = qEnvironmentVariableIntValue("MIKAI_MPV_VERBOSE") != 0;
    try {
        requestLogLevel(developerVerboseLogs_);
    } catch (...) {
        api_->terminateDestroy(handle_);
        handle_ = nullptr;
        throw;
    }
}

MpvHandle::~MpvHandle() {
    // A reply that arrives after this would find a destroyed callback target.
    cancelAllAsync();
    observers_.clear();
    if (handle_ != nullptr && api_ != nullptr) {
        api_->terminateDestroy(handle_);
        handle_ = nullptr;
    }
    invalidateAsyncEpoch();
    identity_ = 0;
}

MpvHandle::MpvHandle(MpvHandle&& other) noexcept
    : api_(other.api_),
      handle_(std::exchange(other.handle_, nullptr)),
      sink_(std::move(other.sink_)),
      observers_(std::move(other.observers_)),
      pending_(std::move(other.pending_)),
      nextObserverId_(other.nextObserverId_),
      nextRequestId_(other.nextRequestId_),
      // The identity travels with the core, so a request issued before the move
      // is still recognised as belonging to the handle that now owns it.
      identity_(other.identity_),
      developerVerboseLogs_(other.developerVerboseLogs_),
      requestedLogLevel_(other.requestedLogLevel_),
      epoch_(other.epoch_) {
    other.api_ = nullptr;
    other.sink_ = nullptr;
    other.observers_.clear();
    other.pending_.clear();
    other.identity_ = 0;
    other.developerVerboseLogs_ = false;
    other.requestedLogLevel_ = '\0';
    other.epoch_ = 0;
}

MpvHandle& MpvHandle::operator=(MpvHandle&& other) noexcept {
    if (this == &other) return *this;
    cancelAllAsync();
    observers_.clear();
    if (handle_ != nullptr && api_ != nullptr) {
        api_->terminateDestroy(handle_);
    }
    handle_ = nullptr;
    // The core that was here is gone: retire its epoch before adopting the
    // incoming one, so any reply still in flight for it is stale.
    invalidateAsyncEpoch();
    api_ = other.api_;
    handle_ = std::exchange(other.handle_, nullptr);
    sink_ = std::move(other.sink_);
    observers_ = std::move(other.observers_);
    pending_ = std::move(other.pending_);
    nextObserverId_ = other.nextObserverId_;
    nextRequestId_ = other.nextRequestId_;
    // Adopt the incoming core's identity and epoch: the moved requests were
    // issued against them, so they stay live rather than being dropped.
    identity_ = other.identity_;
    developerVerboseLogs_ = other.developerVerboseLogs_;
    requestedLogLevel_ = other.requestedLogLevel_;
    epoch_ = other.epoch_;
    other.api_ = nullptr;
    other.sink_ = nullptr;
    other.observers_.clear();
    other.pending_.clear();
    other.identity_ = 0;
    other.developerVerboseLogs_ = false;
    other.requestedLogLevel_ = '\0';
    other.epoch_ = 0;
    return *this;
}

// ── errors ────────────────────────────────────────────────────────────────

std::string MpvHandle::errorText(int error) const {
    if (api_ == nullptr) return "mpv error " + std::to_string(error);
    const char* text = api_->errorString(error);
    if (text != nullptr) return std::string(text);
    return "mpv error " + std::to_string(error);
}

void MpvHandle::require(int error, const char* what, const std::string& property) {
    if (error < 0) throw MpvError(std::string(what) + ": " + errorText(error), property);
}

// ── playback commands ──────────────────────────────────────────────────────

void MpvHandle::loadFile(const std::string& url) {
    // A new source: anything outstanding belongs to the old one, so its
    // callbacks are dropped before the source changes.
    invalidateAsyncEpoch();
    const char* cmd[] = {"loadfile", url.c_str(), nullptr};
    require(api_->command(handle_, cmd), "loadfile", url);
}

void MpvHandle::seek(double seconds) {
    const std::string at = std::to_string(seconds);
    // mpv_command takes a NULL-terminated array; without the terminator the
    // process dies on the first seek.
    const char* cmd[] = {"seek", at.c_str(), "absolute", nullptr};
    require(api_->command(handle_, cmd), "seek");
}

void MpvHandle::setPaused(bool paused) { setFlag("pause", paused); }
void MpvHandle::setSpeed(double speed) { setNumber("speed", speed); }
void MpvHandle::setVolume(double volume) { setNumber("volume", volume); }
void MpvHandle::setMuted(bool muted) { setFlag("mute", muted); }

void MpvHandle::setBackendLogObservationEnabled(bool enabled) {
    requestLogLevel(developerVerboseLogs_ || enabled);
}

void MpvHandle::requestLogLevel(bool verbose) {
    if (handle_ == nullptr || api_ == nullptr || api_->requestLogMessages == nullptr) {
        throw MpvError("mpv_request_log_messages: no live core");
    }
    const char level = verbose ? 'v' : 'i';
    if (requestedLogLevel_ == level) return;
    // mpv_request_log_messages returns void, so the only API-side failure
    // condition exposed here is an unavailable live function entry.
    api_->requestLogMessages(handle_, verbose ? "v" : "info");
    requestedLogLevel_ = level;
}

void MpvHandle::stop() {
    // Same source change as loadFile: retire outstanding callbacks first.
    invalidateAsyncEpoch();
    const char* cmd[] = {"stop", nullptr};
    require(api_->command(handle_, cmd), "stop");
}

// ── properties ─────────────────────────────────────────────────────────────

double MpvHandle::getNumber(const std::string& name) const {
    if (handle_ == nullptr || api_ == nullptr) throw MpvError("getNumber: no core", name);
    double value = 0.0;
    const int rc = api_->getProperty(handle_, name.c_str(), MPV_FORMAT_DOUBLE, &value);
    if (rc < 0) throw MpvError("getNumber: " + errorText(rc), name);
    return value;
}

bool MpvHandle::getFlag(const std::string& name) const {
    if (handle_ == nullptr || api_ == nullptr) throw MpvError("getFlag: no core", name);
    int value = 0;
    const int rc = api_->getProperty(handle_, name.c_str(), MPV_FORMAT_FLAG, &value);
    if (rc < 0) throw MpvError("getFlag: " + errorText(rc), name);
    return value != 0;
}

std::string MpvHandle::getString(const std::string& name) const {
    if (handle_ == nullptr || api_ == nullptr) throw MpvError("getString: no core", name);
    char* value = api_->getPropertyString(handle_, name.c_str());
    if (value == nullptr) throw MpvError("getString: null", name);
    std::string text(value);
    // The buffer is libmpv's, so libmpv frees it - mpv_free, never
    // mpv_free_node_contents, which frees the inside of a node.
    api_->free(value);
    return text;
}

void MpvHandle::setNumber(const std::string& name, double value) {
    const int rc = api_->setProperty(handle_, name.c_str(), MPV_FORMAT_DOUBLE, &value);
    if (rc < 0) throw MpvError("setNumber: " + errorText(rc), name);
}

void MpvHandle::setFlag(const std::string& name, bool value) {
    int flag = value ? 1 : 0;
    const int rc = api_->setProperty(handle_, name.c_str(), MPV_FORMAT_FLAG, &flag);
    if (rc < 0) throw MpvError("setFlag: " + errorText(rc), name);
}

void MpvHandle::setString(const std::string& name, const std::string& value) {
    const int rc = api_->setPropertyString(handle_, name.c_str(), value.c_str());
    if (rc < 0) throw MpvError("setString: " + errorText(rc), name);
}

// ── asynchronous transport ─────────────────────────────────────────────────

AsyncRequestId MpvHandle::registerAsync(std::function<void(const AsyncReply&)> callback) {
    if (handle_ == nullptr || api_ == nullptr || !callback) return AsyncRequestId{};
    AsyncRequestId id;
    // Never zero: mpv reserves reply_userdata 0 for events that are not
    // correlated with a request, and never reused otherwise.
    id.value = nextRequestId_++;
    id.identity = identity_;
    id.epoch = epoch_;
    Pending pending;
    pending.callback = std::move(callback);
    pending.identity = identity_;
    pending.epoch = epoch_;
    pending_.emplace(id.value, std::move(pending));
    return id;
}

AsyncRequestId MpvHandle::commandAsync(std::vector<std::string> args,
                                      std::function<void(const AsyncReply&)> callback) {
    if (handle_ == nullptr || api_ == nullptr || args.empty()) return AsyncRequestId{};
    AsyncRequestId id = registerAsync(std::move(callback));
    if (!id.valid()) return id;
    // mpv reads the argument vector during the call only, so the pointers are
    // built here and gone before this returns.
    std::vector<const char*> argv;
    argv.reserve(args.size() + 1);
    for (const std::string& arg : args) argv.push_back(arg.c_str());
    argv.push_back(nullptr);
    const int rc = api_->commandAsync(handle_, id.value, argv.data());
    if (rc < 0) {
        pending_.erase(id.value);
        return AsyncRequestId{};
    }
    return id;
}

AsyncRequestId MpvHandle::commandNodeAsync(const NodeValue& args,
                                          std::function<void(const AsyncReply&)> callback) {
    if (handle_ == nullptr || api_ == nullptr) return AsyncRequestId{};
    OutgoingNode outgoingNode;
    mpv_node* outgoing = outgoingNode.buildRoot(args);
    AsyncRequestId id = registerAsync(std::move(callback));
    if (!id.valid()) return id;
    const int rc = api_->commandNodeAsync(handle_, id.value, outgoing);
    if (rc < 0) {
        pending_.erase(id.value);
        return AsyncRequestId{};
    }
    return id;
}

AsyncRequestId MpvHandle::getPropertyAsync(const std::string& name, mpv_format format,
                                          std::function<void(const AsyncReply&)> callback) {
    if (handle_ == nullptr || api_ == nullptr) return AsyncRequestId{};
    AsyncRequestId id = registerAsync(std::move(callback));
    if (!id.valid()) return id;
    const int rc = api_->getPropertyAsync(handle_, id.value, name.c_str(), format);
    if (rc < 0) {
        pending_.erase(id.value);
        return AsyncRequestId{};
    }
    return id;
}

void MpvHandle::cancelAsync(AsyncRequestId id) {
    if (!id.valid()) return;
    // Only a request against this handle's *current* core and source can be
    // cancelled: a token from another core that shares the number, or one from
    // before a source change, must not erase an unrelated pending request.
    if (id.identity != identity_ || id.epoch != epoch_) return;
    // The reply still arrives and is discarded; unhooking an accepted request is
    // not worth it.
    pending_.erase(id.value);
}

void MpvHandle::cancelAllAsync() {
    // Same rule as cancelAsync, for every request at once: mpv still answers,
    // and the answers land on an empty table.
    pending_.clear();
}

void MpvHandle::invalidateAsyncEpoch() {
    // Retire the current epoch and drop every callback that belonged to it; a
    // reply already queued is discarded on arrival because its epoch no longer
    // matches.
    ++epoch_;
    pending_.clear();
}

size_t MpvHandle::pendingAsyncCount() const noexcept { return pending_.size(); }
std::string MpvHandle::describeError(int error) const { return errorText(error); }

void MpvHandle::dispatchAsyncReply(const mpv_event* event) {
    const auto it = pending_.find(event->reply_userdata);
    if (it == pending_.end()) return;
    // A reply for a core or source this handle no longer owns: its callback
    // would act on a player that is gone.
    if (it->second.identity != identity_ || it->second.epoch != epoch_) {
        pending_.erase(it);
        return;
    }
    Pending pending = std::move(it->second);
    pending_.erase(it);

    AsyncReply reply;
    reply.error = event->error;
    switch (event->event_id) {
        case MPV_EVENT_COMMAND_REPLY: {
            const auto* command = static_cast<const mpv_event_command*>(event->data);
            // The result belongs to the event, so it is copied out; mpv frees it,
            // never mpv_free_node_contents.
            if (command != nullptr) copyNode(&command->result, reply.node);
            reply.format = command != nullptr ? command->result.format : MPV_FORMAT_NONE;
            break;
        }
        case MPV_EVENT_GET_PROPERTY_REPLY: {
            const auto* property = static_cast<const mpv_event_property*>(event->data);
            if (property == nullptr) break;
            Property value;
            readInto(value, property);
            reply.format = value.format;
            reply.number = value.number;
            reply.flag = value.flag;
            reply.text = value.text;
            copyReplyNode(property, reply.node);
            break;
        }
        default:
            break;
    }
    if (pending.callback) pending.callback(reply);
}

// ── events ─────────────────────────────────────────────────────────────────

void MpvHandle::dispatchPropertyChange(const mpv_event_property* change,
                                       uint64_t replyUserData) {
    if (change == nullptr) return;
    // Property-change events carry the observer id in reply_userdata, so a
    // notification for an unobserved id is simply not ours.
    const auto it = observers_.find(replyUserData);
    if (it == observers_.end() || !it->second.fn) return;
    Property property;
    property.name = change->name != nullptr ? change->name : it->second.name;
    readInto(property, change);
    it->second.fn(property);
}

void MpvHandle::drainEvents() {
    if (handle_ == nullptr || api_ == nullptr) return;
    while (true) {
        mpv_event* event = api_->waitEvent(handle_, 0);
        if (event == nullptr || event->event_id == MPV_EVENT_NONE) break;
        if (sink_) sink_(event);
        switch (event->event_id) {
            case MPV_EVENT_PROPERTY_CHANGE:
                dispatchPropertyChange(static_cast<const mpv_event_property*>(event->data),
                                       event->reply_userdata);
                break;
            case MPV_EVENT_COMMAND_REPLY:
            case MPV_EVENT_GET_PROPERTY_REPLY:
                dispatchAsyncReply(event);
                break;
            default:
                break;
        }
    }
}

void MpvHandle::setEventSink(std::function<void(mpv_event*)> sink) {
    sink_ = std::move(sink);
}

uint64_t MpvHandle::observe(const std::string& name, mpv_format format,
                            std::function<void(const Property&)> fn) {
    if (handle_ == nullptr || api_ == nullptr || !fn) return 0;
    // Monotonic and never reused, so a notification in flight when an id was
    // unobserved cannot land on the observer that took the slot afterwards.
    const uint64_t id = nextObserverId_++;
    Observer observer;
    observer.name = name;
    observer.fn = std::move(fn);
    observers_.emplace(id, std::move(observer));
    const int rc = api_->observeProperty(handle_, id, name.c_str(), format);
    if (rc < 0) {
        observers_.erase(id);
        throw MpvError("observe " + name + ": " + errorText(rc), name);
    }
    return id;
}

void MpvHandle::unobserve(uint64_t id) {
    const auto it = observers_.find(id);
    if (it == observers_.end()) return;
    observers_.erase(it);
    // The real call, not just a local forget: leaving the registration in place
    // would keep mpv queueing changes for an observer that is already gone.
    if (handle_ != nullptr && api_ != nullptr) api_->unobserveProperty(handle_, id);
}

void MpvHandle::unobserveAll() {
    std::vector<uint64_t> ids;
    ids.reserve(observers_.size());
    for (const auto& [id, observer] : observers_) ids.push_back(id);
    for (const uint64_t id : ids) unobserve(id);
}

uint64_t MpvHandle::apiVersion() {
    MpvApi& api = MpvApi::instance();
    if (api.loaded()) return api.apiVersion();
    return MpvApi::headerApiVersion();
}

}  // namespace mplayer