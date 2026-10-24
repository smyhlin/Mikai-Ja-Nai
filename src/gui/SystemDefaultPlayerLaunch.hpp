#pragma once

#include <QtCore/qglobal.h>

#ifdef Q_OS_WIN
namespace mikai::gui::detail {

// The sole external-UI boundary of default-player registration.
// Returns false and stores the original Win32 error when launch fails.
bool launchDefaultAppsSettings(const wchar_t* uri, unsigned long* errorCode);

}  // namespace mikai::gui::detail
#endif
