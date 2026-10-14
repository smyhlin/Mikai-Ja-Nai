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

/// The keyboard glyph for shortcuts list.
QIcon keyboardIcon(int size = 0, const QString& color = {});

/// The tuning sliders glyph for the processing pipeline tab.
QIcon slidersIcon(int size = 0, const QString& color = {});

/// The module cube glyph for the downloadable components tab.
QIcon packageIcon(int size = 0, const QString& color = {});

/// Crisp vector left/right chevrons for carousel navigation buttons.
QIcon chevronLeftIcon(int size = 0);
QIcon chevronRightIcon(int size = 0);

}  // namespace mikai::app::theme
