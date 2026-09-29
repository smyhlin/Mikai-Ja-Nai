#pragma once

#include <QString>

namespace mikai::app::theme {

/// The one application-wide Qt stylesheet, built from the token headers.
QString buildStylesheet();

}  // namespace mikai::app::theme
