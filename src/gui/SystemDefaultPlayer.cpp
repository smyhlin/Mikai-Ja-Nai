#include "SystemDefaultPlayer.hpp"

#include <QCoreApplication>

#ifdef Q_OS_WIN
#include <QDir>
#include <QOperatingSystemVersion>
#include <QtCore/qt_windows.h>
#include <shellapi.h>
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
    return ERROR_SUCCESS;
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

bool writeString(const QString& path, const wchar_t* name, const QString& value,
                 QString* error) {
    RegistryKey key;
    LSTATUS status = RegCreateKeyExW(HKEY_CURRENT_USER, wide(path), 0, nullptr,
                                    REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr,
                                    &key.handle, nullptr);
    if (status == ERROR_SUCCESS) {
        status = RegSetValueExW(key.handle, name, 0, REG_SZ,
            reinterpret_cast<const BYTE*>(value.utf16()),
            static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    }
    return status == ERROR_SUCCESS || registryFailure(path, status, error);
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
    const QString icon = QStringLiteral("\"%1\",0").arg(executable);
    // Literal file argument, including spaces and leading dashes; never a shell.
    const QString command = QStringLiteral("\"%1\" --open-video \"%2\"")
        .arg(executable, QStringLiteral("%1"));
    const QString owner = QString::fromWCharArray(ownerId);
    if (!writeString(app, ownerValue, owner, error)
        || !writeString(progId, ownerValue, owner, error)
        || !writeString(progId, nullptr, QCoreApplication::translate(
            "SystemDefaultPlayer", "Mikai video file"), error)
        || !writeString(progId + QStringLiteral("\\DefaultIcon"), nullptr, icon, error)
        || !writeString(progId + QStringLiteral("\\shell\\open\\command"), nullptr, command, error)
        || !writeString(capabilities, L"ApplicationName", QString::fromWCharArray(applicationName), error)
        || !writeString(capabilities, L"ApplicationDescription", QCoreApplication::translate(
            "SystemDefaultPlayer", "Play local video files with Mikai."), error)
        || !writeString(capabilities, L"ApplicationIcon", icon, error)) return false;
    const QString associations = capabilities + QStringLiteral("\\FileAssociations");
    for (const char* extension : videoExtensions) {
        const QString name = QString::fromLatin1(extension);
        if (!writeString(associations, wide(name), QStringLiteral("Mikai.Video"), error)) return false;
    }
    // Publish only after all executable and capabilities metadata is present.
    if (!writeString(QString::fromWCharArray(registeredApplicationsKey), applicationName,
                     capabilities, error)) return false;
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
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
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_FLAG_NO_UI;
    info.lpVerb = L"open";
    info.lpFile = uri;
    info.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&info)) return true;
    const DWORD status = GetLastError();
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
