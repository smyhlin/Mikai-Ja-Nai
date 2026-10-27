#pragma once

#include <QString>
#include <utility>

#ifdef Q_OS_WIN
#include <QtCore/qt_windows.h>

// T-407: the registry and Win32 error plumbing the Windows integration
// modules each carried a private copy of. The unit lives in src/gui because
// the dependency arrow runs app -> player -> backend: both app-layer
// integrations (SystemContextMenu, SystemStartup) and the gui-layer one
// (SystemDefaultPlayer) compile against the player target's sources, so this
// is the one spot all three can include without a layering inversion.
namespace mikai::gui::win32 {

// Reinterprets the QString's UTF-16 buffer as a wide C string, the form the
// registry and shell APIs take.
const wchar_t* wide(const QString& value);

// RAII registry key handle: closed exactly once, never copied.
struct RegistryKey {
    HKEY handle = nullptr;
    RegistryKey() = default;
    RegistryKey(const RegistryKey&) = delete;
    RegistryKey& operator=(const RegistryKey&) = delete;
    RegistryKey(RegistryKey&& other) noexcept : handle(std::exchange(other.handle, nullptr)) {}
    RegistryKey& operator=(RegistryKey&& other) noexcept {
        if (this != &other) {
            if (handle != nullptr) RegCloseKey(handle);
            handle = std::exchange(other.handle, nullptr);
        }
        return *this;
    }
    ~RegistryKey() {
        if (handle != nullptr) RegCloseKey(handle);
    }
};

// ERROR_FILE_NOT_FOUND and ERROR_PATH_NOT_FOUND: the two "not there" statuses
// a registry read can return, treated as absent rather than failed.
bool missing(LSTATUS status);

// The system's own message for a status code, formatted and trimmed. Empty
// when Windows has no text for the code; each caller keeps its translated
// sentence and its "Unknown Windows error" fallback in its own context.
QString systemMessage(DWORD status);

}  // namespace mikai::gui::win32
#endif
