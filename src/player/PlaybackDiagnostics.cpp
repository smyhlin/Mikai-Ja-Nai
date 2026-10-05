#include "PlaybackDiagnostics.hpp"

#include "MpvHandle.hpp"
#include "native/NativeFilterCommand.hpp"
#include "native/NativeStatus.hpp"

namespace mplayer {

std::optional<int64_t> exactInteger(const AsyncReply& reply) {
    if (!reply.ok() || reply.node.format != MPV_FORMAT_INT64) return std::nullopt;
    return reply.node.integer;
}

bool summarizeVfChain(const AsyncReply& reply, bool* present, int* count) {
    if (!reply.ok() || reply.node.format != MPV_FORMAT_NODE_ARRAY) return false;
    *present = false;
    *count = static_cast<int>(reply.node.list.size());
    for (const NodeValue& filter : reply.node.list) {
        if (filter.format != MPV_FORMAT_NODE_MAP) return false;
        const NodeValue* name = nullptr;
        const NodeValue* label = nullptr;
        for (const auto& entry : filter.map) {
            if (!name && entry.first == "name") {
                name = &entry.second;
            } else if (!label && entry.first == "label") {
                label = &entry.second;
            } else if (entry.first == "name" || entry.first == "label") {
                return false;
            }
        }
        if (name == nullptr || name->format != MPV_FORMAT_STRING) return false;
        if (label != nullptr && label->format != MPV_FORMAT_STRING) return false;
        if (name->text == native::kNativeFilterName) *present = true;
        if (label != nullptr && label->text == native::kNativeStatusLabel) *present = true;
    }
    return true;
}

}  // namespace mplayer
