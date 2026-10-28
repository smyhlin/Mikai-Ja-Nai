#pragma once

#include <QColor>
#include <QPushButton>
#include <QWindow>

namespace mikai::app {

/// A colour chip that paints itself. The settings cards carry a stylesheet
/// rule for their QPushButtons, and that rule outranks an inline sheet on a
/// single button, which is why the chips cannot simply set a background.
class SwatchButton : public QPushButton {
    Q_OBJECT

public:
    explicit SwatchButton(const QColor& color, QWidget* parent = nullptr);

    /// Replaces the fill. The custom chip uses a gradient instead and keeps
    /// this empty.
    void setColor(const QColor& color);

    /// Draws the two-pixel selection ring.
    void setSelected(bool selected);

    /// Paints the multi-stop "custom colour" gradient rather than a solid fill.
    void setSpectrum(bool spectrum);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QColor color_;
    bool selected_ = false;
    bool spectrum_ = false;
};

}  // namespace mikai::app
