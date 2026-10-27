#include "Win32Support.hpp"

#ifdef Q_OS_WIN

namespace mikai::gui::win32 {

const wchar_t* wide(const QString& value) {
    return reinterpret_cast<const wchar_t*>(value.utf16());
}

bool missing(LSTATUS status) {
    return status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND;
}

QString systemMessage(DWORD status) {
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, status, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    const QString reason =
        length != 0 ? QString::fromWCharArray(buffer, length).trimmed() : QString{};
    if (buffer != nullptr) LocalFree(buffer);
    return reason;
}

}  // namespace mikai::gui::win32
#endif
