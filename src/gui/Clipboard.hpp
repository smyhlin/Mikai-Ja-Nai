#pragma once

#include <QClipboard>
#include <QGuiApplication>
#include <QImage>
#include <QString>
#ifdef Q_OS_WIN
#include <QLibrary>
#endif

namespace mikai::gui {

#ifdef Q_OS_WIN
namespace detail {

inline bool windowsClipboardWritable() {
    using Open = int (__stdcall*)(void*);
    using Close = int (__stdcall*)();
    static const auto open = reinterpret_cast<Open>(
        QLibrary::resolve(QStringLiteral("user32"), "OpenClipboard"));
    static const auto close = reinterpret_cast<Close>(
        QLibrary::resolve(QStringLiteral("user32"), "CloseClipboard"));
    if (open == nullptr || close == nullptr || !open(nullptr)) return false;
    return close() != 0;
}

} // namespace detail
#endif

// A refused Qt Windows setter can invalidate its previous delayed data object.
inline bool copyText(const QString& text) {
    if (!QGuiApplication::instance()) return false;
    QClipboard* clipboard = QGuiApplication::clipboard();
    if (clipboard == nullptr) return false;
#ifdef Q_OS_WIN
    const bool windowsClipboard = QGuiApplication::platformName() == QLatin1String("windows");
#endif
    for (int attempt = 0; attempt < 2; ++attempt) {
#ifdef Q_OS_WIN
        if (windowsClipboard && !detail::windowsClipboardWritable()) continue;
#endif
        clipboard->setText(text);
#ifdef Q_OS_WIN
        // Windows ownership proves OleSetClipboard accepted this new data object.
        if (windowsClipboard && !clipboard->ownsClipboard()) continue;
#endif
        if (clipboard->text() == text) return true;
    }
    return false;
}

inline bool copyImage(const QImage& image) {
    if (image.isNull() || !QGuiApplication::instance()) return false;
    QClipboard* clipboard = QGuiApplication::clipboard();
    if (clipboard == nullptr) return false;
    const QImage expected = image.convertToFormat(QImage::Format_RGBA8888);
#ifdef Q_OS_WIN
    const bool windowsClipboard = QGuiApplication::platformName() == QLatin1String("windows");
#endif
    for (int attempt = 0; attempt < 2; ++attempt) {
#ifdef Q_OS_WIN
        if (windowsClipboard && !detail::windowsClipboardWritable()) continue;
#endif
        clipboard->setImage(image);
#ifdef Q_OS_WIN
        if (windowsClipboard && !clipboard->ownsClipboard()) continue;
#endif
        const QImage actual = clipboard->image();
        if (actual.size() == image.size() &&
            actual.convertToFormat(QImage::Format_RGBA8888) == expected) return true;
    }
    return false;
}

}
