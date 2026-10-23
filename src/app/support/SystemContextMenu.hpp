#pragma once

class QString;

namespace mikai::app {

bool systemContextMenuSupported();
/// True only when every supported video extension has this executable's verb.
/// Reads the current user's registry; no preference is used as a substitute.
bool systemContextMenuEnabled();
/// Changes only Mikai's per-user Explorer verb, never a default association.
/// On failure restores the previous owned values; any rollback failure is named.
bool setSystemContextMenuEnabled(bool enabled, QString* error = nullptr);

}  // namespace mikai::app
