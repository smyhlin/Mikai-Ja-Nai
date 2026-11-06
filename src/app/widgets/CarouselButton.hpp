// The catalogue's carousel pager arrow.
//
// A stylesheet rounded rect is painted by QStyleSheetStyle without antialiasing
// on a raster surface, which leaves stair steps on every corner of the pager; the
// arrow itself was a rasterised pixmap. This control paints both by hand at the
// widget's own device ratio, so the corners and the chevron are smooth at any
// scale, and it keeps the `carouselControl` object name the sheets size.

#pragma once

#include <QPushButton>

namespace mikai::app {

class CarouselButton final : public QPushButton {
public:
    enum class Direction { Left, Right };

    explicit CarouselButton(Direction direction, QWidget* parent = nullptr);

    Direction direction() const { return direction_; }

protected:
    bool event(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    Direction direction_ = Direction::Left;
};

}  // namespace mikai::app
