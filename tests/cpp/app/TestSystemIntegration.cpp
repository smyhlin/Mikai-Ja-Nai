#include "TestSystemIntegration.hpp"

#include "app/support/SystemContextMenu.hpp"
#include "app/support/SystemStartup.hpp"
#include "gui/SystemDefaultPlayer.hpp"
#include "gui/SystemDefaultPlayerLaunch.hpp"
#include "gui/XdgDesktopIntegration.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

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

#ifdef Q_OS_LINUX
namespace {
class XdgSandbox {
public:
  XdgSandbox() : config(qgetenv("XDG_CONFIG_HOME")), data(qgetenv("XDG_DATA_HOME")),
                 desktop(qgetenv("XDG_CURRENT_DESKTOP")), path(qgetenv("PATH")),
                 hadConfig(qEnvironmentVariableIsSet("XDG_CONFIG_HOME")),
                 hadData(qEnvironmentVariableIsSet("XDG_DATA_HOME")),
                 hadDesktop(qEnvironmentVariableIsSet("XDG_CURRENT_DESKTOP")),
                 hadPath(qEnvironmentVariableIsSet("PATH")) {
    qputenv("XDG_CONFIG_HOME", (dir.path() + QStringLiteral("/config space")).toUtf8());
    qputenv("XDG_DATA_HOME", (dir.path() + QStringLiteral("/data space")).toUtf8());
    qputenv("XDG_CURRENT_DESKTOP", "MikaiTestUnknownDesktop");
    qputenv("PATH", dir.path().toUtf8());
  }
  ~XdgSandbox() {
    restore("XDG_CONFIG_HOME", config, hadConfig);
    restore("XDG_DATA_HOME", data, hadData);
    restore("XDG_CURRENT_DESKTOP", desktop, hadDesktop);
    restore("PATH", path, hadPath);
  }
  QTemporaryDir dir;
private:
  static void restore(const char* name, const QByteArray& value, bool existed) {
    if (existed) qputenv(name, value); else qunsetenv(name);
  }
  QByteArray config, data, desktop, path;
  bool hadConfig, hadData, hadDesktop, hadPath;
};
}
#endif

void TestSystemIntegration::xdgRegistrationsRoundTrip() {
#ifndef Q_OS_LINUX
  QSKIP("XDG integration is available on Linux.");
#else
  XdgSandbox sandbox;
  QVERIFY(sandbox.dir.isValid());
  QString error;
  QVERIFY(mikai::app::systemStartupSupported());
  QVERIFY(mikai::app::systemContextMenuSupported());
  QVERIFY2(mikai::app::setSystemStartupEnabled(true, &error), qPrintable(error));
  QVERIFY(mikai::app::systemStartupEnabled());
  QVERIFY2(mikai::app::setSystemContextMenuEnabled(true, &error), qPrintable(error));
  QVERIFY(mikai::app::systemContextMenuEnabled());
  QVERIFY2(mikai::app::setSystemStartupEnabled(false, &error), qPrintable(error));
  QVERIFY2(mikai::app::setSystemContextMenuEnabled(false, &error), qPrintable(error));
  QVERIFY(!mikai::app::systemStartupEnabled());
  QVERIFY(!mikai::app::systemContextMenuEnabled());
#endif
}

void TestSystemIntegration::xdgForeignEntriesArePreserved() {
#ifndef Q_OS_LINUX
  QSKIP("XDG integration is available on Linux.");
#else
  XdgSandbox sandbox;
  QVERIFY(sandbox.dir.isValid());
  using namespace mikai::gui::xdg;
  for (const Entry entry : {Entry::Startup, Entry::VideoPlayer}) {
    const QString path = entryPath(entry);
    QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write("foreign"), qint64(7));
    file.close();
    QString error;
    QVERIFY(!setEnabled(entry, true, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!setEnabled(entry, false, &error));
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), QByteArray("foreign"));
    file.close();
    QVERIFY(file.remove());
    const QString target = sandbox.dir.path() + QStringLiteral("/target");
    QFile targetFile(target);
    if (!targetFile.exists()) {
      QVERIFY(targetFile.open(QIODevice::WriteOnly));
      targetFile.close();
    }
    QVERIFY(QFile::link(target, path));
    QVERIFY(!setEnabled(entry, true, &error));
    QVERIFY(!setEnabled(entry, false, &error));
    QVERIFY(QFile::remove(path));
  }
#endif
}

void TestSystemIntegration::xdgExecEscapingAndDefaultChoice() {
#ifndef Q_OS_LINUX
  QSKIP("XDG integration is available on Linux.");
#else
  XdgSandbox sandbox;
  QVERIFY(sandbox.dir.isValid());
  using namespace mikai::gui::xdg;
  const QString text = desktopContents(Entry::VideoPlayer, QString::fromUtf8("/tmp/a b/日本%$`\\\"\\\\mikai"));
  QVERIFY(text.contains(QString::fromUtf8("日本%%")));
  QVERIFY(text.contains(QStringLiteral("\\\\$")));
  QVERIFY(text.contains(QStringLiteral("\\\\`")));
  QVERIFY(text.contains(QStringLiteral("\\\\\"")));
  QVERIFY(text.contains(QStringLiteral("\\\\\\\\mikai")));
  QVERIFY(text.contains(QStringLiteral(" --open-video %f\n")));
  QVERIFY(!text.contains(QStringLiteral("Exec=/tmp")));
  QVERIFY(desktopContents(Entry::Startup, QStringLiteral("relative/path")).isEmpty());
  QVERIFY(desktopContents(Entry::Startup, QStringLiteral("/tmp/invalid=name")).isEmpty());
  QString error;
  QVERIFY(!mikai::gui::requestSystemDefaultPlayer(&error));
  QVERIFY(!error.isEmpty());
  QVERIFY(mikai::app::systemContextMenuEnabled());
  const QString candidate = entryPath(Entry::VideoPlayer);
  QVERIFY(QFile::exists(candidate));
  const QString defaults = qEnvironmentVariable("XDG_CONFIG_HOME") + QStringLiteral("/mimeapps.list");
  QVERIFY(QDir().mkpath(QFileInfo(defaults).absolutePath()));
  QFile mimeapps(defaults);
  QVERIFY(mimeapps.open(QIODevice::WriteOnly));
  const QByteArray choice("[Default Applications]\nvideo/mp4=mikai.desktop;\n");
  QCOMPARE(mimeapps.write(choice), qint64(choice.size()));
  mimeapps.close();
  QVERIFY2(mikai::app::setSystemContextMenuEnabled(false, &error), qPrintable(error));
  QVERIFY(!mikai::app::systemContextMenuEnabled());
  QFile retained(candidate);
  QVERIFY(retained.open(QIODevice::ReadOnly));
  const QByteArray contents = retained.readAll();
  QVERIFY(contents.contains("Exec="));
  QVERIFY(contents.contains("NoDisplay=true\n"));
  QVERIFY(!contents.contains("MimeType="));
  QVERIFY(mimeapps.open(QIODevice::ReadOnly));
  QCOMPARE(mimeapps.readAll(), choice);
  QVERIFY2(mikai::app::setSystemContextMenuEnabled(true, &error), qPrintable(error));
  QVERIFY(mikai::app::systemContextMenuEnabled());
#endif
}

void TestSystemIntegration::xdgReusesVerifiedPackageEntry() {
#ifndef Q_OS_LINUX
  QSKIP("XDG integration is available on Linux.");
#else
  XdgSandbox sandbox;
  QVERIFY(sandbox.dir.isValid());
  const QString executableLink = sandbox.dir.path() + QStringLiteral("/mikai");
  QVERIFY(QFile::link(QCoreApplication::applicationFilePath(), executableLink));
  const QString path = mikai::gui::xdg::entryPath(mikai::gui::xdg::Entry::VideoPlayer);
  QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
  QFile source(QStringLiteral(MIKAI_SOURCE_DIR "/packaging/linux/mikai.desktop"));
  QVERIFY(source.open(QIODevice::ReadOnly));
  const QByteArray original = source.readAll();
  QFile packaged(path);
  QVERIFY(packaged.open(QIODevice::WriteOnly));
  QCOMPARE(packaged.write(original), qint64(original.size()));
  packaged.close();
  QString error;
  QVERIFY(mikai::app::systemContextMenuEnabled());
  QVERIFY2(mikai::gui::xdg::registerDefaultCandidate(&error), qPrintable(error));
  QVERIFY(!mikai::app::setSystemContextMenuEnabled(false, &error));
  QVERIFY(packaged.open(QIODevice::ReadOnly));
  QCOMPARE(packaged.readAll(), original);
  packaged.close();
  QVERIFY(QFile::remove(executableLink));
  QVERIFY(!mikai::gui::xdg::registerDefaultCandidate(&error));
  QVERIFY(packaged.open(QIODevice::ReadOnly));
  QCOMPARE(packaged.readAll(), original);
#endif
}

void TestSystemIntegration::xdgRefreshesOnlyMissingOwnedInstallations() {
#ifndef Q_OS_LINUX
  QSKIP("XDG integration is available on Linux.");
#else
  XdgSandbox sandbox;
  QVERIFY(sandbox.dir.isValid());
  using namespace mikai::gui::xdg;
  const QString oldExecutable = sandbox.dir.path() + QString::fromUtf8("/old path/日本%old");
  QVERIFY(QDir().mkpath(QFileInfo(oldExecutable).absolutePath()));
  QFile oldFile(oldExecutable);
  QVERIFY(oldFile.open(QIODevice::WriteOnly));
  oldFile.close();

  for (const Entry entry : {Entry::Startup, Entry::VideoPlayer}) {
    const QString path = entryPath(entry);
    QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
    QByteArray old = desktopContents(entry, oldExecutable).toUtf8();
    if (entry == Entry::VideoPlayer) old += "X-Mikai-DefaultRegistration=true\n";
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(file.write(old), qint64(old.size()));
    file.close();
    QString error;
    QVERIFY(!setEnabled(entry, true, &error));
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), old);
    file.close();
  }
  QVERIFY(oldFile.remove());

  for (const Entry entry : {Entry::Startup, Entry::VideoPlayer}) {
    const QString path = entryPath(entry);
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray old = file.readAll();
    file.close();
    QVERIFY(file.open(QIODevice::Append));
    QCOMPARE(file.write("# foreign addition\n"), qint64(19));
    file.close();
    QString error;
    QVERIFY(!setEnabled(entry, true, &error));
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(file.write(old), qint64(old.size()));
    file.close();
    QVERIFY(!setEnabled(entry, false, &error));
    QVERIFY2(setEnabled(entry, true, &error), qPrintable(error));
    QVERIFY(enabled(entry));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray refreshed = file.readAll();
    file.close();
    QVERIFY(refreshed != old);
    if (entry == Entry::VideoPlayer) {
      QVERIFY(refreshed.contains("X-Mikai-DefaultRegistration=true\n"));
      QVERIFY2(setEnabled(entry, false, &error), qPrintable(error));
      QVERIFY(QFile::exists(path));
      QVERIFY(!enabled(entry));
    }
  }
#endif
}

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
