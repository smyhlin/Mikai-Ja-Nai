#include "SystemDefaultPlayer.hpp"
#include "SystemDefaultPlayerLaunch.hpp"

#include <QCoreApplication>

#ifdef Q_OS_WIN
#include <QByteArray>
#include <QDir>
#include <QList>
#include <QOperatingSystemVersion>
#include <QStringList>
#include <algorithm>
#include <utility>
#include <QtCore/qt_windows.h>
#include <shlobj.h>

namespace {

// These are application identities, not translated display labels. In
// particular, registeredAppUser must match the RegisteredApplications value.
constexpr auto applicationName = L"Mikai";
constexpr auto applicationKey = L"Software\\Mikai\\DefaultVideoPlayer";
constexpr auto capabilitiesKey = L"Software\\Mikai\\DefaultVideoPlayer\\Capabilities";
constexpr auto progIdKey = L"Software\\Classes\\Mikai.Video";
constexpr auto registeredApplicationsKey = L"Software\\RegisteredApplications";
constexpr auto ownerValue = L"MikaiRegistrationOwner";
constexpr auto ownerId = L"{7D1829D9-6F44-47BA-B79B-871D57145C86}";
constexpr const char* videoExtensions[] = {
    ".mkv", ".mp4", ".webm", ".avi", ".mov", ".m4v",
    ".ts", ".m2ts", ".mpeg", ".mpg", ".flv"
};

const wchar_t* wide(const QString& value) {
    return reinterpret_cast<const wchar_t*>(value.utf16());
}

struct RegistryKey {
    HKEY handle = nullptr;
    RegistryKey() = default;
    RegistryKey(const RegistryKey&) = delete;
    RegistryKey& operator=(const RegistryKey&) = delete;
    ~RegistryKey() { if (handle != nullptr) RegCloseKey(handle); }
};

bool missing(LSTATUS status) {
    return status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND;
}

QString windowsError(DWORD status) {
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, status, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    const QString reason = length != 0 ? QString::fromWCharArray(buffer, length).trimmed()
                                      : QCoreApplication::translate(
                                            "SystemDefaultPlayer", "Unknown Windows error");
    if (buffer != nullptr) LocalFree(buffer);
    return QCoreApplication::translate("SystemDefaultPlayer", "%1 (Windows error %2).")
        .arg(reason).arg(status);
}

bool registryFailure(const QString& path, LSTATUS status, QString* error) {
    if (error != nullptr) {
        *error = QCoreApplication::translate(
            "SystemDefaultPlayer", "Could not register Mikai at HKCU\\%1: %2")
            .arg(path, windowsError(status));
    }
    return false;
}

bool collision(const QString& path, QString* error) {
    if (error != nullptr) {
        *error = QCoreApplication::translate(
            "SystemDefaultPlayer",
            "Mikai could not be registered because HKCU\\%1 contains a registration "
            "not owned by this application. Nothing there was replaced.").arg(path);
    }
    return false;
}

// Read into aligned QString storage; registry strings need not be terminated.
LSTATUS readString(HKEY key, const wchar_t* name, QString* value, bool* exists) {
    *exists = false;
    DWORD type = REG_NONE;
    DWORD size = 0;
    LSTATUS status = RegQueryValueExW(key, name, nullptr, &type, nullptr, &size);
    if (missing(status)) return ERROR_SUCCESS;
    if (status != ERROR_SUCCESS) return status;
    *exists = true;
    if (type != REG_SZ || size > 64 * 1024 || size % sizeof(wchar_t) != 0)
        return ERROR_INVALID_DATA;
    value->resize(size / sizeof(wchar_t));
    status = RegQueryValueExW(key, name, nullptr, &type,
                             reinterpret_cast<BYTE*>(value->data()), &size);
    if (status != ERROR_SUCCESS) return status;
    if (type != REG_SZ || size % sizeof(wchar_t) != 0) return ERROR_INVALID_DATA;
    value->resize(size / sizeof(wchar_t));
    while (value->endsWith(QChar(u'\0'))) value->chop(1);
    return value->contains(QChar(u'\0')) ? ERROR_INVALID_DATA : ERROR_SUCCESS;
}

// Do not adopt an existing foreign namespace just because it has our name.
// Once claimed, only our documented fields are updated; extra values/subkeys
// are never deleted. This also allows portable installs to move executables.
bool checkOwnedKey(const wchar_t* path, QString* error) {
    RegistryKey key;
    const QString text = QString::fromWCharArray(path);
    LSTATUS status = RegOpenKeyExW(HKEY_CURRENT_USER, path, 0, KEY_READ, &key.handle);
    if (missing(status)) return true;
    if (status != ERROR_SUCCESS) return registryFailure(text, status, error);
    QString owner;
    bool exists = false;
    status = readString(key.handle, ownerValue, &owner, &exists);
    if (status == ERROR_INVALID_DATA) return collision(text, error);
    if (status != ERROR_SUCCESS) return registryFailure(text, status, error);
    if (exists) return owner == QString::fromWCharArray(ownerId) || collision(text, error);
    DWORD subkeys = 0;
    DWORD values = 0;
    status = RegQueryInfoKeyW(key.handle, nullptr, nullptr, nullptr, &subkeys, nullptr,
                             nullptr, &values, nullptr, nullptr, nullptr, nullptr);
    if (status != ERROR_SUCCESS) return registryFailure(text, status, error);
    return (subkeys == 0 && values == 0) || collision(text, error);
}

bool checkRegisteredApplication(QString* error) {
    RegistryKey key;
    LSTATUS status = RegOpenKeyExW(HKEY_CURRENT_USER, registeredApplicationsKey, 0,
                                  KEY_QUERY_VALUE, &key.handle);
    if (missing(status)) return true;
    const QString path = QString::fromWCharArray(registeredApplicationsKey);
    if (status != ERROR_SUCCESS) return registryFailure(path, status, error);
    QString existing;
    bool exists = false;
    status = readString(key.handle, applicationName, &existing, &exists);
    if (status == ERROR_INVALID_DATA) return collision(path + QStringLiteral("\\Mikai"), error);
    if (status != ERROR_SUCCESS) return registryFailure(path, status, error);
    return !exists || existing.compare(QString::fromWCharArray(capabilitiesKey),
                                      Qt::CaseInsensitive) == 0
        || collision(path + QStringLiteral("\\Mikai"), error);
}

struct RegistrationValue {
    RegistrationValue(QString valuePath, QString valueName, QString desiredValue)
        : path(std::move(valuePath)), name(std::move(valueName)), desired(std::move(desiredValue)) {}

    QString path;
    QString name;
    QString desired;
    bool existed = false;
    DWORD type = REG_NONE;
    QByteArray bytes;
};

LSTATUS readValue(RegistrationValue& value) {
    RegistryKey key;
    LSTATUS status = RegOpenKeyExW(HKEY_CURRENT_USER, wide(value.path), 0,
                                   KEY_QUERY_VALUE, &key.handle);
    if (missing(status)) return ERROR_SUCCESS;
    if (status != ERROR_SUCCESS) return status;
    DWORD size = 0;
    status = RegQueryValueExW(key.handle, wide(value.name), nullptr, &value.type, nullptr, &size);
    if (missing(status)) return ERROR_SUCCESS;
    if (status != ERROR_SUCCESS) return status;
    if (size > 64 * 1024) return ERROR_INVALID_DATA;
    value.bytes.resize(size);
    status = RegQueryValueExW(key.handle, wide(value.name), nullptr, &value.type,
                             reinterpret_cast<BYTE*>(value.bytes.data()), &size);
    if (status != ERROR_SUCCESS) return status;
    value.bytes.resize(size);
    value.existed = true;
    return ERROR_SUCCESS;
}

LSTATUS writeValue(const RegistrationValue& value, bool restore) {
    RegistryKey key;
    LSTATUS status;
    if (restore && !value.existed) {
        status = RegOpenKeyExW(HKEY_CURRENT_USER, wide(value.path), 0, KEY_SET_VALUE, &key.handle);
        if (missing(status)) return ERROR_SUCCESS;
    } else {
        status = RegCreateKeyExW(HKEY_CURRENT_USER, wide(value.path), 0, nullptr,
                                REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr,
                                &key.handle, nullptr);
    }
    if (status != ERROR_SUCCESS) return status;
    if (restore && !value.existed) {
        status = RegDeleteValueW(key.handle, wide(value.name));
        return missing(status) ? ERROR_SUCCESS : status;
    }
    const BYTE* bytes = restore
        ? reinterpret_cast<const BYTE*>(value.bytes.constData())
        : reinterpret_cast<const BYTE*>(value.desired.utf16());
    const DWORD size = restore ? static_cast<DWORD>(value.bytes.size())
        : static_cast<DWORD>((value.desired.size() + 1) * sizeof(wchar_t));
    return RegSetValueExW(key.handle, wide(value.name), 0, restore ? value.type : REG_SZ, bytes, size);
}

// Snapshot every owned field before touching anything. Preserve prior bytes,
// types and foreign additions, including when a portable copy moves folders.
bool writeRegistration(QList<RegistrationValue>& values, QString* error) {
    QStringList newKeys;
    for (RegistrationValue& value : values) {
        const LSTATUS status = readValue(value);
        if (status != ERROR_SUCCESS) return registryFailure(value.path, status, error);
        // Include intermediate private keys so a failed first registration does
        // not leave newly-created shell/Capabilities scaffolding behind.
        QString path = value.path;
        const QString root = path.startsWith(QString::fromWCharArray(applicationKey))
            ? QString::fromWCharArray(applicationKey)
            : path.startsWith(QString::fromWCharArray(progIdKey))
                ? QString::fromWCharArray(progIdKey) : path;
        for (;;) {
            RegistryKey key;
            const LSTATUS keyStatus = RegOpenKeyExW(HKEY_CURRENT_USER, wide(path), 0,
                                                    KEY_QUERY_VALUE, &key.handle);
            if (missing(keyStatus)) {
                if (!newKeys.contains(path)) newKeys.append(path);
            } else if (keyStatus != ERROR_SUCCESS) {
                return registryFailure(path, keyStatus, error);
            }
            if (path == root) break;
            path.truncate(path.lastIndexOf(QLatin1Char('\\')));
        }
    }
    for (qsizetype index = 0; index < values.size(); ++index) {
        const LSTATUS status = writeValue(values.at(index), false);
        if (status == ERROR_SUCCESS) continue;
        registryFailure(values.at(index).path, status, error);
        QStringList failures;
        for (qsizetype undo = index + 1; undo > 0; --undo) {
            const RegistrationValue& value = values.at(undo - 1);
            const LSTATUS restored = writeValue(value, true);
            if (restored != ERROR_SUCCESS) {
                failures.append(QStringLiteral("HKCU\\%1: %2").arg(value.path, windowsError(restored)));
            }
        }
        std::sort(newKeys.begin(), newKeys.end(), [](const QString& a, const QString& b) {
            return a.size() > b.size();
        });
        for (const QString& path : std::as_const(newKeys)) {
            RegistryKey key;
            LSTATUS pruned = RegOpenKeyExW(HKEY_CURRENT_USER, wide(path), 0,
                                           KEY_QUERY_VALUE, &key.handle);
            if (missing(pruned)) continue;
            if (pruned == ERROR_SUCCESS) {
                DWORD subkeys = 0;
                DWORD count = 0;
                pruned = RegQueryInfoKeyW(key.handle, nullptr, nullptr, nullptr, &subkeys, nullptr,
                                          nullptr, &count, nullptr, nullptr, nullptr, nullptr);
                // RegDeleteKey would discard foreign values even if there are
                // no child keys. Never recursively delete a private namespace.
                if (pruned == ERROR_SUCCESS && subkeys == 0 && count == 0) {
                    pruned = RegDeleteKeyW(HKEY_CURRENT_USER, wide(path));
                }
            }
            if (pruned != ERROR_SUCCESS && !missing(pruned)) {
                failures.append(QStringLiteral("HKCU\\%1: %2").arg(path, windowsError(pruned)));
            }
        }
        if (error != nullptr && !failures.isEmpty()) {
            *error += QLatin1Char('\n') + QCoreApplication::translate(
                "SystemDefaultPlayer",
                "The previous registration could not be fully restored; some changes may remain:")
                + QLatin1Char('\n') + failures.join(QLatin1Char('\n'));
        }
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST | SHCNF_FLUSHNOWAIT, nullptr, nullptr);
        return false;
    }
    return true;
}

bool registerApplication(QString* error) {
    // Preflight both private namespaces and the one shared named value before
    // writing anything. No extension defaults, UserChoice, hashes, OpenWith
    // lists, machine keys, or other applications' registrations are touched.
    if (!checkOwnedKey(applicationKey, error) || !checkOwnedKey(progIdKey, error)
        || !checkRegisteredApplication(error)) return false;

    const QString app = QString::fromWCharArray(applicationKey);
    const QString capabilities = QString::fromWCharArray(capabilitiesKey);
    const QString progId = QString::fromWCharArray(progIdKey);
    const QString executable = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    if (executable.isEmpty() || executable.contains(QLatin1Char('"'))) {
        if (error != nullptr) {
            *error = QCoreApplication::translate(
                "SystemDefaultPlayer", "The application executable path is unavailable or invalid.");
        }
        return false;
    }
    const QString icon = QStringLiteral("\"%1\",0").arg(executable);
    // Literal file argument, including spaces and leading dashes; never a shell.
    const QString command = QStringLiteral("\"%1\" --open-video \"%2\"")
        .arg(executable, QStringLiteral("%1"));
    const QString owner = QString::fromWCharArray(ownerId);
    QList<RegistrationValue> values;
    values.append({app, QString::fromWCharArray(ownerValue), owner});
    values.append({progId, QString::fromWCharArray(ownerValue), owner});
    values.append({progId, {}, QCoreApplication::translate(
        "SystemDefaultPlayer", "Mikai video file")});
    values.append({progId + QStringLiteral("\\DefaultIcon"), {}, icon});
    values.append({progId + QStringLiteral("\\shell\\open\\command"), {}, command});
    values.append({capabilities, QStringLiteral("ApplicationName"), QString::fromWCharArray(applicationName)});
    values.append({capabilities, QStringLiteral("ApplicationDescription"), QCoreApplication::translate(
        "SystemDefaultPlayer", "Play local video files with Mikai.")});
    values.append({capabilities, QStringLiteral("ApplicationIcon"), icon});
    const QString associations = capabilities + QStringLiteral("\\FileAssociations");
    for (const char* extension : videoExtensions) {
        values.append({associations, QString::fromLatin1(extension), QStringLiteral("Mikai.Video")});
    }
    // Publish last; if any step fails, restore the exact previous registration.
    values.append({QString::fromWCharArray(registeredApplicationsKey),
                   QString::fromWCharArray(applicationName), capabilities});
    if (!writeRegistration(values, error)) return false;
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST | SHCNF_FLUSHNOWAIT, nullptr, nullptr);
    return true;
}

}  // namespace
#endif

namespace mikai::gui {

bool systemDefaultPlayerSupported() {
#ifdef Q_OS_WIN
    return true;
#else
    return false;
#endif
}

bool requestSystemDefaultPlayer(QString* error) {
    if (error != nullptr) error->clear();
#ifdef Q_OS_WIN
    if (!registerApplication(error)) return false;
    // Microsoft documents registeredAppUser as the named HKCU registration,
    // not a ProgID or executable. Patched Windows 11 supports this deep link;
    // older Windows versions receive the general Default Apps page instead.
    // https://learn.microsoft.com/windows/apps/develop/launch/launch-default-apps-settings
    const bool windows11 = QOperatingSystemVersion::current()
        >= QOperatingSystemVersion(QOperatingSystemVersion::Windows, 10, 0, 22000);
    const wchar_t* uri = windows11 ? L"ms-settings:defaultapps?registeredAppUser=Mikai"
                                  : L"ms-settings:defaultapps";
    DWORD status = ERROR_SUCCESS;
    if (detail::launchDefaultAppsSettings(uri, &status)) return true;
    if (error != nullptr) {
        *error = QCoreApplication::translate(
            "SystemDefaultPlayer",
            "Mikai is registered as an available video player, but Windows Default Apps "
            "could not be opened: %1\n\nOpen Windows Settings > Apps > Default apps and "
            "search for Mikai. Your current defaults have not been changed.")
            .arg(windowsError(status));
    }
    return false;
#else
    if (error != nullptr) {
        *error = QCoreApplication::translate(
            "SystemDefaultPlayer", "Setting the default video player is only supported on Windows.");
    }
    return false;
#endif
}

}  // namespace mikai::gui
