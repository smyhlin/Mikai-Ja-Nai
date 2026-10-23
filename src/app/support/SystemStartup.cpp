#include "SystemStartup.hpp"

#include <QCoreApplication>
#include <QDir>

#ifdef Q_OS_WIN
#include <QtCore/qt_windows.h>
#endif

namespace mikai::app {
namespace {

#ifdef Q_OS_WIN
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"Mikai";

struct RegistryKey {
    HKEY handle = nullptr;
    ~RegistryKey() {
        if (handle != nullptr) RegCloseKey(handle);
    }
};

QString windowsError(LSTATUS code) {
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(code), 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    const QString detail = length != 0
                               ? QString::fromWCharArray(buffer, static_cast<int>(length)).trimmed()
                               : QCoreApplication::translate("SystemStartup", "Unknown Windows error");
    if (buffer != nullptr) LocalFree(buffer);
    return QCoreApplication::translate("SystemStartup", "%1 (Windows error %2)").arg(detail).arg(code);
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
    return ERROR_SUCCESS;
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
    if (result == ERROR_FILE_NOT_FOUND) {
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
            *error = QCoreApplication::translate("SystemStartup", "The Mikai startup entry points to another executable and was left unchanged: %1")
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
