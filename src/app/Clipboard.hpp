#pragma once

#include <QClipboard>
#include <QGuiApplication>
#include <QString>

namespace mikai::app {

/// Copy `text` and report whether it actually landed on the system clipboard.
///
/// Windows can refuse an OLE clipboard write and the failure is silent: the
/// board keeps its previous content while every caller reports "copied". Read
/// the value back, retry once, and let the caller say what really happened.
inline bool copyText(const QString& text) {
    if (!QGuiApplication::instance()) {
        return false;
    }
    QClipboard* clipboard = QGuiApplication::clipboard();
    if (!clipboard) {
        return false;
    }
    for (int attempt = 0; attempt < 2; ++attempt) {
        clipboard->setText(text);
        if (clipboard->text() == text) {
            return true;
        }
    }
    return false;
}

}  // namespace mikai::app
