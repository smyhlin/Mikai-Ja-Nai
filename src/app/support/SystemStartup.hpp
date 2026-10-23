#pragma once

#include <QString>

namespace mikai::app {

bool systemStartupSupported();
bool systemStartupEnabled();
bool setSystemStartupEnabled(bool enabled, QString* error = nullptr);

}  // namespace mikai::app
