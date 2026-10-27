#include "SystemDefaultPlayerLaunch.hpp"

#ifdef Q_OS_WIN
#include <QtCore/qt_windows.h>
#include <shellapi.h>

namespace mikai::gui::detail {

bool launchDefaultAppsSettings(const wchar_t* uri, unsigned long* errorCode) {
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_FLAG_NO_UI;
    info.lpVerb = L"open";
    info.lpFile = uri;
    info.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&info)) return true;
    if (errorCode != nullptr) *errorCode = GetLastError();
    return false;
}

}  // namespace mikai::gui::detail
#elif defined(Q_OS_LINUX)
#include <QProcess>
#include <QFileInfo>
#include <QDir>
#include <QStandardPaths>
#include <QStringList>

namespace mikai::gui::detail {
bool launchDefaultAppsSettings() {
    const QString desktop = qEnvironmentVariable("XDG_CURRENT_DESKTOP").toLower();
    QStringList programAndArguments;
    if (desktop.contains(QStringLiteral("gnome")) || desktop.contains(QStringLiteral("unity"))) {
        programAndArguments = {QStringLiteral("gnome-control-center")};
        // Panel desktop launchers are version-dependent; if neither is
        // installed, open the overview rather than guessing a panel name.
        const QStringList dirs = QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation);
        for (const QString& dir : dirs) {
            if (QFileInfo::exists(QDir(dir).filePath(QStringLiteral("applications/gnome-default-apps-panel.desktop")))) {
                programAndArguments.append(QStringLiteral("default-apps"));
                break;
            }
            if (QFileInfo::exists(QDir(dir).filePath(QStringLiteral("applications/gnome-applications-panel.desktop")))) {
                programAndArguments.append(QStringLiteral("applications"));
                break;
            }
        }
    } else if (desktop.contains(QStringLiteral("kde"))) {
        programAndArguments = {QStringLiteral("systemsettings"), QStringLiteral("kcm_componentchooser")};
    } else if (desktop.contains(QStringLiteral("xfce"))) {
        programAndArguments = {QStringLiteral("xfce4-mime-settings")};
    } else {
        return false;
    }
    const QString executable = QStandardPaths::findExecutable(programAndArguments.takeFirst());
    return !executable.isEmpty() && QProcess::startDetached(executable, programAndArguments);
}
}  // namespace mikai::gui::detail
#endif
