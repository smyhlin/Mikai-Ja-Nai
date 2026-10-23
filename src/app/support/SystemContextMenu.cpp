#include "SystemContextMenu.hpp"

#include <QCoreApplication>
#include <QString>

#ifdef Q_OS_WIN
#include <QByteArray>
#include <QDir>
#include <QList>
#include <QStringList>
#include <QtCore/qt_windows.h>
#include <shlobj.h>
#include <cstring>
#include <iterator>
#include <utility>

namespace {

// Default associations, UserChoice and other applications' verbs remain untouched.
constexpr const char* videoExtensions[] = {
    ".mkv", ".mp4", ".webm", ".avi", ".mov", ".m4v",
    ".ts", ".m2ts", ".mpeg", ".mpg", ".flv"
};

const wchar_t* wide(const QString& value) {
    return reinterpret_cast<const wchar_t*>(value.utf16());
}

struct RegistryKey {
    HKEY handle = nullptr;
    ~RegistryKey() { if (handle != nullptr) RegCloseKey(handle); }
};

struct OwnedValue {
    OwnedValue(QString valuePath, QString valueName, QString desiredValue)
        : path(std::move(valuePath)), name(std::move(valueName)),
          desired(std::move(desiredValue)) {}

    QString path;
    QString name;
    QString desired;
    bool keyExisted = false;
    bool existed = false;
    DWORD type = REG_NONE;
    QByteArray bytes;
};

QString failureText(const QString& path, LSTATUS status) {
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, status, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    const QString reason = length != 0 ? QString::fromWCharArray(buffer, length).trimmed()
                                      : QCoreApplication::translate(
                                            "SystemContextMenu", "Unknown Windows error");
    if (buffer != nullptr) LocalFree(buffer);
    return QCoreApplication::translate(
        "SystemContextMenu", "Registry operation failed for HKCU\\%1: %2 (Windows error %3).")
        .arg(path, reason).arg(status);
}

bool missing(LSTATUS status) {
    return status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND;
}

QList<OwnedValue> ownedValues() {
    const QString executable = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    const QString icon = QStringLiteral("\"%1\",0").arg(executable);
    // The file is a single literal argument, including when its name starts
    // with a dash. Do not invoke a command shell or leave either path unquoted.
    const QString command = QStringLiteral("\"%1\" --open-video \"%2\"")
                                .arg(executable, QStringLiteral("%1"));
    QList<OwnedValue> values;
    values.reserve(3 * static_cast<qsizetype>(std::size(videoExtensions)));
    for (const char* extension : videoExtensions) {
        const QString path = QStringLiteral(
            "Software\\Classes\\SystemFileAssociations\\%1\\shell\\Mikai.OpenVideo")
                                 .arg(QString::fromLatin1(extension));
        values.append({path, {}, QCoreApplication::translate("SystemContextMenu", "Open with Mikai")});
        values.append({path, QStringLiteral("Icon"), icon});
        values.append({path + QStringLiteral("\\command"), {}, command});
    }
    return values;
}

LSTATUS readValue(OwnedValue& value) {
    RegistryKey key;
    LSTATUS status = RegOpenKeyExW(HKEY_CURRENT_USER, wide(value.path), 0,
                                   KEY_QUERY_VALUE, &key.handle);
    if (missing(status)) return ERROR_SUCCESS;
    if (status != ERROR_SUCCESS) return status;
    value.keyExisted = true;
    DWORD bytes = 0;
    status = RegQueryValueExW(key.handle, wide(value.name), nullptr, &value.type, nullptr, &bytes);
    if (missing(status)) return ERROR_SUCCESS;
    if (status != ERROR_SUCCESS) return status;
    if (bytes > 64 * 1024) return ERROR_INVALID_DATA;
    value.bytes.resize(bytes);
    status = RegQueryValueExW(key.handle, wide(value.name), nullptr, &value.type,
                              reinterpret_cast<BYTE*>(value.bytes.data()), &bytes);
    if (status != ERROR_SUCCESS) return status;
    value.bytes.resize(bytes);
    value.existed = true;
    return ERROR_SUCCESS;
}

LSTATUS writeValue(const OwnedValue& value, bool present, DWORD type, const BYTE* bytes,
                  DWORD size) {
    RegistryKey key;
    LSTATUS status;
    if (present) {
        status = RegCreateKeyExW(HKEY_CURRENT_USER, wide(value.path), 0, nullptr,
                                REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr,
                                &key.handle, nullptr);
    } else {
        status = RegOpenKeyExW(HKEY_CURRENT_USER, wide(value.path), 0,
                               KEY_SET_VALUE, &key.handle);
        if (missing(status)) return ERROR_SUCCESS;
    }
    if (status != ERROR_SUCCESS) return status;
    if (present) return RegSetValueExW(key.handle, wide(value.name), 0, type, bytes, size);
    status = RegDeleteValueW(key.handle, wide(value.name));
    return missing(status) ? ERROR_SUCCESS : status;
}

// RegDeleteKey can discard values even when there are no subkeys, so check both.
// Foreign additions inside our verb are retained, not recursively erased.
LSTATUS removeEmptyKey(const QString& path) {
    RegistryKey key;
    LSTATUS status = RegOpenKeyExW(HKEY_CURRENT_USER, wide(path), 0, KEY_QUERY_VALUE,
                                   &key.handle);
    if (missing(status)) return ERROR_SUCCESS;
    if (status != ERROR_SUCCESS) return status;
    DWORD subkeys = 0;
    DWORD values = 0;
    status = RegQueryInfoKeyW(key.handle, nullptr, nullptr, nullptr, &subkeys, nullptr,
                              nullptr, &values, nullptr, nullptr, nullptr, nullptr);
    if (status != ERROR_SUCCESS || subkeys != 0 || values != 0) return status;
    status = RegDeleteKeyW(HKEY_CURRENT_USER, wide(path));
    return missing(status) ? ERROR_SUCCESS : status;
}

QStringList restoreValues(const QList<OwnedValue>& values) {
    QStringList errors;
    for (const OwnedValue& value : values) {
        // A failed disable may already have pruned an originally empty key.
        // Recreate that key too, not just the values that happened to exist.
        if (value.keyExisted) {
            RegistryKey key;
            const LSTATUS status = RegCreateKeyExW(
                HKEY_CURRENT_USER, wide(value.path), 0, nullptr, REG_OPTION_NON_VOLATILE,
                KEY_SET_VALUE, nullptr, &key.handle, nullptr);
            if (status != ERROR_SUCCESS) {
                errors.append(failureText(value.path, status));
                continue;
            }
        }
        const LSTATUS status = writeValue(value, value.existed, value.type,
            reinterpret_cast<const BYTE*>(value.bytes.constData()),
            static_cast<DWORD>(value.bytes.size()));
        if (status != ERROR_SUCCESS) errors.append(failureText(value.path, status));
    }
    // Command before verb. Only keys absent before this operation are pruned.
    for (auto it = values.crbegin(); it != values.crend(); ++it) {
        if (it->keyExisted) continue;
        const LSTATUS status = removeEmptyKey(it->path);
        if (status != ERROR_SUCCESS) errors.append(failureText(it->path, status));
    }
    return errors;
}

}  // namespace
#endif

namespace mikai::app {

bool systemContextMenuSupported() {
#ifdef Q_OS_WIN
    return true;
#else
    return false;
#endif
}

bool systemContextMenuEnabled() {
#ifdef Q_OS_WIN
    QList<OwnedValue> values = ownedValues();
    for (OwnedValue& value : values) {
        if (readValue(value) != ERROR_SUCCESS || !value.existed || value.type != REG_SZ
            || value.bytes.size() < static_cast<qsizetype>(sizeof(wchar_t))
            || value.bytes.size() % sizeof(wchar_t) != 0) return false;
        // Copy into aligned QString storage rather than reading QByteArray as
        // wchar_t. Registry strings are not required to carry a trailing NUL.
        QString actual(value.bytes.size() / sizeof(wchar_t), Qt::Uninitialized);
        std::memcpy(actual.data(), value.bytes.constData(), value.bytes.size());
        while (actual.endsWith(QChar(u'\0'))) actual.chop(1);
        // A prior interface language's label is still a valid registration.
        if (value.name.isEmpty() && !value.path.endsWith(QStringLiteral("\\command"))) {
            if (actual.isEmpty()) return false;
        } else if (actual != value.desired) {
            return false;
        }
    }
    return true;
#else
    return false;
#endif
}

bool setSystemContextMenuEnabled(bool enabled, QString* error) {
    if (error != nullptr) error->clear();
#ifdef Q_OS_WIN
    QList<OwnedValue> values = ownedValues();
    // Snapshot original bytes and types first. A failed write must not turn a
    // pre-existing partial registration into a falsely reported disabled state.
    for (OwnedValue& value : values) {
        const LSTATUS status = readValue(value);
        if (status != ERROR_SUCCESS) {
            if (error != nullptr) *error = failureText(value.path, status);
            return false;
        }
    }
    QString failure;
    for (const OwnedValue& value : values) {
        const LSTATUS status = writeValue(value, enabled, REG_SZ,
            reinterpret_cast<const BYTE*>(value.desired.utf16()),
            static_cast<DWORD>((value.desired.size() + 1) * sizeof(wchar_t)));
        if (status != ERROR_SUCCESS) {
            failure = failureText(value.path, status);
            break;
        }
    }
    if (failure.isEmpty() && !enabled) {
        for (auto it = values.crbegin(); it != values.crend(); ++it) {
            const LSTATUS status = removeEmptyKey(it->path);
            if (status != ERROR_SUCCESS) {
                failure = failureText(it->path, status);
                break;
            }
        }
    }
    if (!failure.isEmpty()) {
        const QStringList rollbackErrors = restoreValues(values);
        if (!rollbackErrors.isEmpty()) {
            failure += QLatin1Char('\n') + QCoreApplication::translate(
                "SystemContextMenu",
                "The previous Explorer registration could not be fully restored; some changes may remain:");
            failure += QLatin1Char('\n') + rollbackErrors.join(QLatin1Char('\n'));
        }
        if (error != nullptr) *error = failure;
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST | SHCNF_FLUSHNOWAIT, nullptr, nullptr);
        return false;
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST | SHCNF_FLUSHNOWAIT, nullptr, nullptr);
    return true;
#else
    Q_UNUSED(enabled);
    if (error != nullptr) {
        *error = QCoreApplication::translate("SystemContextMenu",
            "Explorer context menu integration is not supported on this platform.");
    }
    return false;
#endif
}

}  // namespace mikai::app
