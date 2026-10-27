#include "SystemStartup.hpp"

#include <QCoreApplication>
#include <QDir>

#ifdef Q_OS_WIN
#include "gui/Win32Support.hpp"
#include <QtCore/qt_windows.h>
#endif

namespace mikai::app {
namespace {

#ifdef Q_OS_WIN
// T-407: the shared registry plumbing lives in gui/Win32Support.hpp; these
// names bring it into this translation unit unqualified, so every call site
// below reads exactly as it did when the helpers were private copies.
using mikai::gui::win32::RegistryKey;
using mikai::gui::win32::missing;

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"Mikai";

QString windowsError(LSTATUS code) {
    // T-407: the FormatMessageW plumbing is shared; this module keeps its own
    // translated sentence and fallback so no user-facing string moved context.
    const QString detail = mikai::gui::win32::systemMessage(static_cast<DWORD>(code));
    return QCoreApplication::translate("SystemStartup", "%1 (Windows error %2)")
        .arg(detail.isEmpty()
                 ? QCoreApplication::translate("SystemStartup", "Unknown Windows error")
                 : detail)
        .arg(code);
}

bool fail(QString* error, const QString& operation, LSTATUS code) {
    if (error != nullptr) *error = operation + QStringLiteral(": ") + windowsError(code);
    return false;
}

QString startupCommand() {
    const QString path = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    if (path.isEmpty() || path.contains(QLatin1Char('"'))) return {};
    return QLatin1Char('"') + path + QLatin1Char('"');
}

// Read only this application's value, without creating the shared Run key.
LSTATUS readCommand(HKEY key, QString* command, bool* exists) {
    *exists = false;
    command->clear();
    DWORD type = 0;
    DWORD bytes = 0;
    LSTATUS result = RegQueryValueExW(key, kRunValue, nullptr, &type, nullptr, &bytes);
    if (result == ERROR_FILE_NOT_FOUND) return ERROR_SUCCESS;
    if (result != ERROR_SUCCESS) return result;
    *exists = true;
    if (type != REG_SZ || bytes < sizeof(wchar_t) ||
        bytes > 261 * sizeof(wchar_t) || bytes % sizeof(wchar_t) != 0) {
        return ERROR_INVALID_DATA;
    }
    command->resize(static_cast<qsizetype>(bytes / sizeof(wchar_t)));
    result = RegQueryValueExW(key, kRunValue, nullptr, &type,
                             reinterpret_cast<BYTE*>(command->data()), &bytes);
    if (result != ERROR_SUCCESS) return result;
    if (type != REG_SZ || bytes < sizeof(wchar_t) || bytes % sizeof(wchar_t) != 0) {
        return ERROR_INVALID_DATA;
    }
    command->resize(static_cast<qsizetype>(bytes / sizeof(wchar_t)));
    if (!command->endsWith(QChar::Null)) return ERROR_INVALID_DATA;
    command->chop(1);
    return command->contains(QChar::Null) ? ERROR_INVALID_DATA : ERROR_SUCCESS;
}
#endif

}  // namespace

bool systemStartupSupported() {
#ifdef Q_OS_WIN
    return true;
#else
    return false;
#endif
}

bool systemStartupEnabled() {
#ifdef Q_OS_WIN
    const QString expected = startupCommand();
    if (expected.isEmpty() || expected.size() > 260) return false;
    RegistryKey key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key.handle) != ERROR_SUCCESS) {
        return false;
    }
    QString command;
    bool exists = false;
    return readCommand(key.handle, &command, &exists) == ERROR_SUCCESS && exists &&
           command.compare(expected, Qt::CaseInsensitive) == 0;
#else
    return false;
#endif
}

bool setSystemStartupEnabled(bool enabled, QString* error) {
    if (error != nullptr) error->clear();
#ifdef Q_OS_WIN
    const QString expected = startupCommand();
    if (expected.isEmpty()) {
        if (error != nullptr) *error = QCoreApplication::translate("SystemStartup", "The application executable path is unavailable or invalid.");
        return false;
    }
    // Windows documents a 260-character limit for Run command lines.
    if (enabled && expected.size() > 260) {
        if (error != nullptr) {
            *error = QCoreApplication::translate("SystemStartup", "The executable path is too long for Windows automatic startup (260-character command limit).");
        }
        return false;
    }
    RegistryKey key;
    LSTATUS result = RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0,
                                   KEY_QUERY_VALUE | KEY_SET_VALUE, &key.handle);
    if (missing(result)) {
        if (!enabled) return true;
        result = RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0,
                                 KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &key.handle, nullptr);
    }
    if (result != ERROR_SUCCESS) {
        return fail(error, QCoreApplication::translate("SystemStartup", "Could not open the current user's Windows startup key"), result);
    }
    QString command;
    bool exists = false;
    result = readCommand(key.handle, &command, &exists);
    if (result != ERROR_SUCCESS) {
        return fail(error, QCoreApplication::translate("SystemStartup", "Could not read the Mikai startup entry; it was left unchanged"), result);
    }
    // A same-named foreign registration is not ours to replace or remove.
    if (exists && command.compare(expected, Qt::CaseInsensitive) != 0) {
        if (error != nullptr) {
            *error = QCoreApplication::translate(
                "SystemStartup",
                "The Mikai startup entry points to another executable and was left unchanged: %1\n"
                "If a portable folder was moved, disable startup from the old location before "
                "moving it, or remove the stale Mikai entry in Windows before enabling it here.")
                         .arg(command);
        }
        return false;
    }
    if (enabled) {
        if (exists) return true;
        result = RegSetValueExW(key.handle, kRunValue, 0, REG_SZ,
                                reinterpret_cast<const BYTE*>(expected.utf16()),
                                static_cast<DWORD>((expected.size() + 1) * sizeof(wchar_t)));
    } else {
        if (!exists) return true;
        result = RegDeleteValueW(key.handle, kRunValue);
        if (result == ERROR_FILE_NOT_FOUND) return true;
    }
    if (result != ERROR_SUCCESS) {
        return fail(error, QCoreApplication::translate("SystemStartup", "Could not change the Mikai startup entry"), result);
    }
    return true;
#else
    Q_UNUSED(enabled);
    if (error != nullptr) *error = QCoreApplication::translate("SystemStartup", "Automatic startup is only available on Windows.");
    return false;
#endif
}

}  // namespace mikai::app
