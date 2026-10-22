#pragma once

#include "theme/Appearance.hpp"

#include <QString>

namespace mikai::app::theme {

/// The application-wide Qt stylesheet for one appearance.
QString buildStylesheet(const Appearance& appearance);

/// The appearance the application ships with: dark bed, violet accent, MD
/// corners.
QString buildStylesheet();

/// Installs the palette Qt draws its own sub-controls from (combo arrows,
/// menu indicators, checkbox ticks) so they match the sheet in every theme.
void applyPalette(const Appearance& appearance);

}  // namespace mikai::app::theme
