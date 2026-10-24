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
#endif
