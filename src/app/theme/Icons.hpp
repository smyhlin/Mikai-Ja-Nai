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

}  // namespace mikai::app::theme
