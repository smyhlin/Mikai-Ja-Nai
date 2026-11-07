// A Fusion style whose small primitives are drawn by hand.
//
// Qt's Fusion paints the combo arrow and the checkbox tick from tiny rasters and
// integer polygons: at 1x they read as stair steps, and the settings pages are
// full of both. Everything else is handed to the base style, so this changes four
// primitives and nothing about layout, metrics or the sheet.

#pragma once

#include <QProxyStyle>

namespace mikai::app::theme {

class AntialiasedStyle : public QProxyStyle {
public:
    explicit AntialiasedStyle(const QString& baseStyle);

    void drawPrimitive(PrimitiveElement element, const QStyleOption* option, QPainter* painter,
                       const QWidget* widget = nullptr) const override;
    void drawControl(ControlElement element, const QStyleOption* option, QPainter* painter,
                     const QWidget* widget = nullptr) const override;
};

}  // namespace mikai::app::theme
