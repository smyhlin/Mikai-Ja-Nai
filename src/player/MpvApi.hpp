// The table is immutable once a load succeeds, so the module is never
// unloaded: that is what lets every wrapper cache a table pointer.

#pragma once

#include <QList>
#include <QMutex>
#include <QString>
#include <QStringList>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
using HMODULE = void*;
#endif

#include <mpv/client.h>
#include <mpv/render_gl.h>

#include <cstdint>

namespace mplayer {

// root/modulePath are absolute and canonical; deps are canonical dependency
// directories inside root. System DLLs are the only dependency that may live
// outside root and are found by the restricted search itself.
//
// Nothing here names a particular file: the module is whatever path the
// selection names, in whatever directory it names it, under whatever file name
// it carries. That is what lets the stock module and a native fork - same
// loader, same table, same entry points - take one path.
struct MpvBundle {
    QString root;
    QString modulePath;
    QString descriptorId;
    int protocolVersion = 0;
    QStringList deps;
};

// decltype'd from the headers, so a signature drift is a compile error here
// instead of a reinterpret_cast that faults at the first call.
struct MpvApiInstance {
    decltype(&::mpv_create) create = nullptr;
    decltype(&::mpv_set_option_string) setOptionString = nullptr;
    decltype(&::mpv_initialize) initialize = nullptr;
    decltype(&::mpv_request_log_messages) requestLogMessages = nullptr;
    decltype(&::mpv_terminate_destroy) terminateDestroy = nullptr;
    decltype(&::mpv_command) command = nullptr;
    decltype(&::mpv_command_async) commandAsync = nullptr;
    decltype(&::mpv_command_node_async) commandNodeAsync = nullptr;
    decltype(&::mpv_get_property) getProperty = nullptr;
    decltype(&::mpv_get_property_async) getPropertyAsync = nullptr;
    decltype(&::mpv_get_property_string) getPropertyString = nullptr;
    decltype(&::mpv_free) free = nullptr;
    decltype(&::mpv_free_node_contents) freeNodeContents = nullptr;
    decltype(&::mpv_set_property) setProperty = nullptr;
    decltype(&::mpv_set_property_string) setPropertyString = nullptr;
    decltype(&::mpv_observe_property) observeProperty = nullptr;
    decltype(&::mpv_unobserve_property) unobserveProperty = nullptr;
    decltype(&::mpv_wait_event) waitEvent = nullptr;
    decltype(&::mpv_client_api_version) clientApiVersion = nullptr;
    decltype(&::mpv_error_string) errorString = nullptr;
    decltype(&::mpv_render_context_create) renderContextCreate = nullptr;
    decltype(&::mpv_render_context_set_update_callback) renderContextSetUpdateCallback = nullptr;
    decltype(&::mpv_render_context_update) renderContextUpdate = nullptr;
    decltype(&::mpv_render_context_report_swap) renderContextReportSwap = nullptr;
    decltype(&::mpv_render_context_render) renderContextRender = nullptr;
    decltype(&::mpv_render_context_free) renderContextFree = nullptr;
    decltype(&::mpv_render_context_get_info) renderContextGetInfo = nullptr;
};

// Process-wide selected core module: one module, many mpv_handle objects.
// Selection is mutex-guarded; once loaded the table is stable.
class MpvApi {
public:
    static MpvApi& instance();

    MpvApi(const MpvApi&) = delete;
    MpvApi& operator=(const MpvApi&) = delete;

    // Failure selects nothing, so a later call can still succeed. A different
    // module is refused by name: swapping it would unmap code live handles use.
    // A dependency directory that cannot be registered is refused rather than
    // skipped, so a module never loads against a closure other than the one its
    // descriptor declared. What load() records is the descriptor as resolved:
    // canonical root, module path and the dependency directories actually
    // registered, so bundle() reports what the search uses.
    bool load(const MpvBundle& bundle, QString* reason = nullptr);

    bool loaded() const;
    const MpvBundle& bundle() const;
    // Valid only while loaded(); hold the reference, never copy it per frame.
    const MpvApiInstance& functions() const;
    HMODULE module() const noexcept;
    quint64 apiVersion() const;
    static quint64 headerApiVersion();
    QString error() const;

private:
    MpvApi() = default;

    struct State {
        HMODULE module = nullptr;
        MpvApiInstance funcs = {};
        MpvBundle bundle = {};
        quint64 apiVersion = 0;
        bool loaded = false;
        // Held while the module is selected so the restricted search keeps
        // resolving the module's imports from the owned directories.
        QList<void*> userDirs = {};
        QString error = {};
    };

    mutable QMutex mutex_;
    State state_;
};

}  // namespace mplayer