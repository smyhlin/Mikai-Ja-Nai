#include "TestSystemIntegration.hpp"

#include "app/support/SystemContextMenu.hpp"
#include "app/support/SystemStartup.hpp"
#include "gui/SystemDefaultPlayer.hpp"
#include "gui/SystemDefaultPlayerLaunch.hpp"

#include <QUuid>
#include <QString>

#ifdef Q_OS_WIN
#include <QtCore/qt_windows.h>
#include <aclapi.h>
#include <vector>
#endif

namespace {

#ifdef Q_OS_WIN
bool settingsLaunchAttempted = false;

void resetSettingsLaunchAttempt() {
  settingsLaunchAttempted = false;
}
#endif

#ifdef Q_OS_WIN
const wchar_t* wide(const QString& value) {
  return reinterpret_cast<const wchar_t*>(value.utf16());
}

class CurrentUserRegistrySandbox {
public:
  CurrentUserRegistrySandbox() {
    const QString guid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    path_ = QStringLiteral("Software\\MikaiSystemIntegrationTests\\%1").arg(guid);
    if (RegCreateKeyExW(HKEY_CURRENT_USER, wide(path_), 0, nullptr,
                        REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS, nullptr,
                        &root_, nullptr) != ERROR_SUCCESS) return;
    if (RegOverridePredefKey(HKEY_CURRENT_USER, root_) == ERROR_SUCCESS) active_ = true;
  }

  CurrentUserRegistrySandbox(const CurrentUserRegistrySandbox&) = delete;
  CurrentUserRegistrySandbox& operator=(const CurrentUserRegistrySandbox&) = delete;

  ~CurrentUserRegistrySandbox() {
    if (active_) makeRestrictedKeysRemovable();
    if (active_) RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
    if (root_ != nullptr) RegCloseKey(root_);
    if (!path_.isEmpty()) {
      RegDeleteTreeW(HKEY_CURRENT_USER, wide(path_));
      RegDeleteKeyW(HKEY_CURRENT_USER, wide(path_));
    }
  }

  bool active() const { return active_; }
  void makeRestrictedKeysRemovable() {
    for (const QString& path : restrictedPaths_) {
      HKEY key = nullptr;
      if (RegOpenKeyExW(root_, wide(path), 0, WRITE_DAC, &key) != ERROR_SUCCESS) continue;
      SetSecurityInfo(key, SE_REGISTRY_KEY,
          DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION,
          nullptr, nullptr, nullptr, nullptr);
      RegCloseKey(key);
    }
  }
  void markRestrictedKey(const QString& path) { restrictedPaths_.push_back(path); }

private:
  QString path_;
  HKEY root_ = nullptr;
  bool active_ = false;
  std::vector<QString> restrictedPaths_;
};

bool createKey(const QString& path, REGSAM access = KEY_ALL_ACCESS, HKEY* result = nullptr) {
  HKEY key = nullptr;
  const LSTATUS status = RegCreateKeyExW(HKEY_CURRENT_USER, wide(path), 0, nullptr,
                                         REG_OPTION_NON_VOLATILE, access, nullptr,
                                         &key, nullptr);
  if (result != nullptr) *result = key;
  else if (key != nullptr) RegCloseKey(key);
  return status == ERROR_SUCCESS;
}

bool setString(const QString& path, const QString& name, const QString& value) {
  HKEY key = nullptr;
  if (!createKey(path, KEY_SET_VALUE, &key)) return false;
  const LSTATUS status = RegSetValueExW(key, name.isEmpty() ? nullptr : wide(name), 0, REG_SZ,
      reinterpret_cast<const BYTE*>(value.utf16()),
      static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
  RegCloseKey(key);
  return status == ERROR_SUCCESS;
}

bool readString(const QString& path, const QString& name, QString* value) {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, wide(path), 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
    return false;
  DWORD type = 0;
  DWORD bytes = 0;
  LSTATUS status = RegQueryValueExW(key, name.isEmpty() ? nullptr : wide(name), nullptr,
                                    &type, nullptr, &bytes);
  if (status == ERROR_SUCCESS && type == REG_SZ && bytes % sizeof(wchar_t) == 0) {
    value->resize(bytes / sizeof(wchar_t));
    status = RegQueryValueExW(key, name.isEmpty() ? nullptr : wide(name), nullptr,
        &type, reinterpret_cast<BYTE*>(value->data()), &bytes);
    if (status == ERROR_SUCCESS) {
      value->resize(bytes / sizeof(wchar_t));
      while (value->endsWith(QChar(u'\0'))) value->chop(1);
    }
  }
  RegCloseKey(key);
  return status == ERROR_SUCCESS && type == REG_SZ;
}

bool keyExists(const QString& path) {
  HKEY key = nullptr;
  const LSTATUS status = RegOpenKeyExW(HKEY_CURRENT_USER, wide(path), 0, KEY_QUERY_VALUE, &key);
  if (key != nullptr) RegCloseKey(key);
  return status == ERROR_SUCCESS;
}

bool denyCurrentUserSetValue(HKEY key) {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
  DWORD needed = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
  std::vector<BYTE> tokenData(needed);
  const bool gotUser = needed != 0 && GetTokenInformation(
      token, TokenUser, tokenData.data(), needed, &needed) != 0;
  CloseHandle(token);
  if (!gotUser) return false;
  const auto* user = reinterpret_cast<const TOKEN_USER*>(tokenData.data());
  const DWORD sidLength = GetLengthSid(user->User.Sid);
  const DWORD aclBytes = sizeof(ACL) +
      2 * (sizeof(ACCESS_DENIED_ACE) - sizeof(DWORD) + sidLength);
  std::vector<BYTE> aclData(aclBytes);
  auto* acl = reinterpret_cast<PACL>(aclData.data());
  if (!InitializeAcl(acl, aclBytes, ACL_REVISION) ||
      !AddAccessDeniedAce(acl, ACL_REVISION, KEY_SET_VALUE, user->User.Sid) ||
      !AddAccessAllowedAce(acl, ACL_REVISION, KEY_READ | WRITE_DAC, user->User.Sid)) return false;
  return SetSecurityInfo(key, SE_REGISTRY_KEY,
      DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
      nullptr, nullptr, acl, nullptr) == ERROR_SUCCESS;
}

bool createWriteDeniedKey(const QString& path) {
  HKEY key = nullptr;
  if (!createKey(path, KEY_ALL_ACCESS, &key)) return false;
  const bool denied = denyCurrentUserSetValue(key);
  RegCloseKey(key);
  return denied;
}
#endif

}  // namespace

#ifdef Q_OS_WIN
namespace mikai::gui::detail {
bool launchDefaultAppsSettings(const wchar_t*, unsigned long* errorCode) {
  settingsLaunchAttempted = true;
  if (errorCode != nullptr) *errorCode = ERROR_ACCESS_DENIED;
  return false;
}
}  // namespace mikai::gui::detail
#endif

void TestSystemIntegration::startupRegistrationRoundTripsInIsolatedRegistry() {
#ifndef Q_OS_WIN
  QSKIP("Windows registry integration is covered only on Windows.");
#else
  CurrentUserRegistrySandbox sandbox;
  QVERIFY(sandbox.active());
  QVERIFY(!mikai::app::systemStartupEnabled());
  QString error;
  QVERIFY2(mikai::app::setSystemStartupEnabled(true, &error), qPrintable(error));
  QVERIFY(mikai::app::systemStartupEnabled());
  QVERIFY2(mikai::app::setSystemStartupEnabled(false, &error), qPrintable(error));
  QVERIFY(!mikai::app::systemStartupEnabled());
#endif
}

void TestSystemIntegration::startupRefusesAndPreservesForeignEntry() {
#ifndef Q_OS_WIN
  QSKIP("Windows registry integration is covered only on Windows.");
#else
  CurrentUserRegistrySandbox sandbox;
  QVERIFY(sandbox.active());
  const QString foreign = QStringLiteral("\"C:\\Foreign\\Mikai.exe\"");
  QVERIFY(setString(QStringLiteral("Software\\Microsoft\\Windows\\CurrentVersion\\Run"),
                    QStringLiteral("Mikai"), foreign));
  QString error;
  QVERIFY(!mikai::app::setSystemStartupEnabled(false, &error));
  QVERIFY(!error.isEmpty());
  QString actual;
  QVERIFY(readString(QStringLiteral("Software\\Microsoft\\Windows\\CurrentVersion\\Run"),
                     QStringLiteral("Mikai"), &actual));
  QCOMPARE(actual, foreign);
#endif
}

void TestSystemIntegration::contextMenuRegistrationRoundTripsInIsolatedRegistry() {
#ifndef Q_OS_WIN
  QSKIP("Windows registry integration is covered only on Windows.");
#else
  CurrentUserRegistrySandbox sandbox;
  QVERIFY(sandbox.active());
  QVERIFY(!mikai::app::systemContextMenuEnabled());
  QString error;
  QVERIFY2(mikai::app::setSystemContextMenuEnabled(true, &error), qPrintable(error));
  QVERIFY(mikai::app::systemContextMenuEnabled());
  QVERIFY2(mikai::app::setSystemContextMenuEnabled(false, &error), qPrintable(error));
  QVERIFY(!mikai::app::systemContextMenuEnabled());
#endif
}

void TestSystemIntegration::contextMenuRefusesForeignVerbWithoutChangingIt() {
#ifndef Q_OS_WIN
  QSKIP("Windows registry integration is covered only on Windows.");
#else
  CurrentUserRegistrySandbox sandbox;
  QVERIFY(sandbox.active());
  const QString foreignPath = QStringLiteral(
      "Software\\Classes\\SystemFileAssociations\\.mkv\\shell\\Mikai.OpenVideo");
  const QString sentinel = QStringLiteral("owned by another app");
  QVERIFY(setString(foreignPath, QString(), sentinel));
  QVERIFY(setString(foreignPath, QStringLiteral("ForeignValue"), QStringLiteral("keep")));
  QString error;
  QVERIFY(!mikai::app::setSystemContextMenuEnabled(true, &error));
  QVERIFY(!error.isEmpty());
  QString actual;
  QVERIFY(readString(foreignPath, QString(), &actual));
  QCOMPARE(actual, sentinel);
  QVERIFY(readString(foreignPath, QStringLiteral("ForeignValue"), &actual));
  QCOMPARE(actual, QStringLiteral("keep"));
  QVERIFY(!mikai::app::systemContextMenuEnabled());
#endif
}

void TestSystemIntegration::contextMenuPartialFailureRestoresPriorValues() {
#ifndef Q_OS_WIN
  QSKIP("Windows registry integration is covered only on Windows.");
#else
  CurrentUserRegistrySandbox sandbox;
  QVERIFY(sandbox.active());
  const QString blocked = QStringLiteral(
      "Software\\Classes\\SystemFileAssociations\\.flv\\shell\\Mikai.OpenVideo");
  const QString earlier = QStringLiteral(
      "Software\\Classes\\SystemFileAssociations\\.mkv\\shell\\Mikai.OpenVideo");
  QVERIFY(setString(earlier, QStringLiteral("MikaiRegistrationOwner"),
                    QStringLiteral("{7D1829D9-6F44-47BA-B79B-871D57145C86}")));
  QVERIFY(setString(earlier, QStringLiteral("ForeignValue"), QStringLiteral("keep")));
  QVERIFY(createWriteDeniedKey(blocked));
  sandbox.markRestrictedKey(blocked);
  QString error;
  QVERIFY(!mikai::app::setSystemContextMenuEnabled(true, &error));
  QVERIFY(!error.isEmpty());
  QVERIFY(!mikai::app::systemContextMenuEnabled());
  QString actual;
  QVERIFY(readString(earlier, QStringLiteral("MikaiRegistrationOwner"), &actual));
  QCOMPARE(actual, QStringLiteral("{7D1829D9-6F44-47BA-B79B-871D57145C86}"));
  QVERIFY(readString(earlier, QStringLiteral("ForeignValue"), &actual));
  QCOMPARE(actual, QStringLiteral("keep"));
  QVERIFY(!keyExists(QStringLiteral(
      "Software\\Classes\\SystemFileAssociations\\.mp4\\shell\\Mikai.OpenVideo")));
#endif
}

void TestSystemIntegration::defaultPlayerRefusesForeignOwnerWithoutOpeningSettings() {
#ifndef Q_OS_WIN
  QSKIP("Windows registry integration is covered only on Windows.");
#else
  CurrentUserRegistrySandbox sandbox;
  QVERIFY(sandbox.active());
  const QString appKey = QStringLiteral("Software\\Mikai\\DefaultVideoPlayer");
  QVERIFY(setString(appKey, QStringLiteral("MikaiRegistrationOwner"),
                    QStringLiteral("{FOREIGN-OWNER}")));
  QVERIFY(setString(appKey, QStringLiteral("ForeignValue"), QStringLiteral("preserve")));
  resetSettingsLaunchAttempt();
  QString error;
  QVERIFY(!mikai::gui::requestSystemDefaultPlayer(&error));
  QVERIFY(!settingsLaunchAttempted);
  QVERIFY(!error.isEmpty());
  QString actual;
  QVERIFY(readString(appKey, QStringLiteral("MikaiRegistrationOwner"), &actual));
  QCOMPARE(actual, QStringLiteral("{FOREIGN-OWNER}"));
  QVERIFY(readString(appKey, QStringLiteral("ForeignValue"), &actual));
  QCOMPARE(actual, QStringLiteral("preserve"));
#endif
}

void TestSystemIntegration::defaultPlayerPartialFailureRollsBackOwnedValues() {
#ifndef Q_OS_WIN
  QSKIP("Windows registry integration is covered only on Windows.");
#else
  CurrentUserRegistrySandbox sandbox;
  QVERIFY(sandbox.active());
  const QString registered = QStringLiteral("Software\\RegisteredApplications");
  const QString appKey = QStringLiteral("Software\\Mikai\\DefaultVideoPlayer");
  QVERIFY(setString(appKey, QStringLiteral("MikaiRegistrationOwner"),
                    QStringLiteral("{7D1829D9-6F44-47BA-B79B-871D57145C86}")));
  QVERIFY(setString(appKey, QStringLiteral("ForeignValue"), QStringLiteral("keep")));
  QVERIFY(setString(registered, QStringLiteral("OtherApplication"), QStringLiteral("KeepThis")));
  HKEY key = nullptr;
  QVERIFY(createKey(registered, KEY_ALL_ACCESS, &key));
  const bool denied = denyCurrentUserSetValue(key);
  RegCloseKey(key);
  QVERIFY(denied);
  sandbox.markRestrictedKey(registered);

  resetSettingsLaunchAttempt();
  QString error;
  QVERIFY(!mikai::gui::requestSystemDefaultPlayer(&error));
  QVERIFY(!settingsLaunchAttempted);
  QVERIFY(!error.isEmpty());
  QVERIFY(keyExists(appKey));
  QVERIFY(!keyExists(QStringLiteral("Software\\Classes\\Mikai.Video")));
  QString actual;
  QVERIFY(readString(appKey, QStringLiteral("MikaiRegistrationOwner"), &actual));
  QCOMPARE(actual, QStringLiteral("{7D1829D9-6F44-47BA-B79B-871D57145C86}"));
  QVERIFY(readString(appKey, QStringLiteral("ForeignValue"), &actual));
  QCOMPARE(actual, QStringLiteral("keep"));
  QVERIFY(readString(registered, QStringLiteral("OtherApplication"), &actual));
  QCOMPARE(actual, QStringLiteral("KeepThis"));
  QVERIFY(!readString(registered, QStringLiteral("Mikai"), &actual));
#endif
}

void TestSystemIntegration::defaultPlayerSettingsLaunchFailureRetainsRegistration() {
#ifndef Q_OS_WIN
  QSKIP("Windows registry integration is covered only on Windows.");
#else
  CurrentUserRegistrySandbox sandbox;
  QVERIFY(sandbox.active());
  resetSettingsLaunchAttempt();
  QString error;
  QVERIFY(!mikai::gui::requestSystemDefaultPlayer(&error));
  QVERIFY(settingsLaunchAttempted);
  QVERIFY(!error.isEmpty());

  QString actual;
  QVERIFY(readString(QStringLiteral("Software\\RegisteredApplications"),
                     QStringLiteral("Mikai"), &actual));
  QCOMPARE(actual, QStringLiteral("Software\\Mikai\\DefaultVideoPlayer\\Capabilities"));
  QVERIFY(readString(QStringLiteral("Software\\Mikai\\DefaultVideoPlayer"),
                     QStringLiteral("MikaiRegistrationOwner"), &actual));
  QCOMPARE(actual, QStringLiteral("{7D1829D9-6F44-47BA-B79B-871D57145C86}"));
#endif
}
