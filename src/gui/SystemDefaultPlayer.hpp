#pragma once

#include <QString>

namespace mikai::gui {

bool systemDefaultPlayerSupported();

// Explicit user action only: register this executable as a per-user candidate,
// then open Windows Default Apps. Never changes the user's selected defaults.
bool requestSystemDefaultPlayer(QString* error = nullptr);

}  // namespace mikai::gui
