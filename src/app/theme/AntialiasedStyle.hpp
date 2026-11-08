#pragma once

#include <QProxyStyle>

namespace mikai::app::theme {

class AntialiasedStyle : public QProxyStyle {
public:
    explicit AntialiasedStyle(const QString& baseStyle);

    void drawPrimitive(PrimitiveElement element, const QStyleOption* option, QPainter* painter,
                       const QWidget* widget = nullptr) const override;
};

}  // namespace mikai::app::theme
