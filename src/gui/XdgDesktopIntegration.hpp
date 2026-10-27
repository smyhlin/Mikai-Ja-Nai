#pragma once

#include <QString>

namespace mikai::gui::xdg {
// Both Open With and default-player selection use the same desktop ID.
enum class Entry { Startup, VideoPlayer };
QString entryPath(Entry entry);
QString desktopContents(Entry entry, const QString& executable);
bool enabled(Entry entry);
bool setEnabled(Entry entry, bool enabled, QString* error);
bool registerDefaultCandidate(QString* error);
}
