#pragma once

#include <QIcon>
#include <QString>

namespace mikai::app::theme {

/// The settings glyph, painted rather than shipped as an asset.
///
/// The app carries no icon files: the glyph is a QPainterPath drawn once and
/// cached, so it stays crisp at any device pixel ratio and adds nothing to the
/// build.
QIcon gearIcon(int size = 0, const QString& color = {});
QIcon outlineGearIcon(int size = 0, const QString& color = {}, int trailingGap = 0);

/// The keyboard glyph for shortcuts list.
QIcon keyboardIcon(int size = 0, const QString& color = {}, int trailingGap = 0);

/// The tuning sliders glyph for the processing pipeline tab.
QIcon slidersIcon(int size = 0, const QString& color = {});

/// The module cube glyph for the downloadable components tab.
QIcon packageIcon(int size = 0, const QString& color = {});

// The main page header's own glyphs. `trailingGap` widens the glyph's box with
// transparent pixels on its trailing side: a widget that draws a button's icon
// and label itself owns the spacing between them, and this is how the header
// brings the style's fixed gap up to the 8 px its design asks for.
QIcon searchIcon(int size = 0, const QString& color = {}, int trailingGap = 0);
QIcon homeIcon(int size = 0, const QString& color = {}, int trailingGap = 0);
QIcon filterIcon(int size = 0, const QString& color = {}, int trailingGap = 0);
QIcon teamsIcon(int size = 0, const QString& color = {}, int trailingGap = 0);
QIcon playIcon(int size = 0, const QString& color = {}, int trailingGap = 0);

/// Crisp vector left/right chevrons for carousel navigation buttons.
QIcon chevronLeftIcon(int size = 0);
QIcon chevronRightIcon(int size = 0);

}  // namespace mikai::app::theme
