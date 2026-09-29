#pragma once

#include <QDialog>
#include <QLabel>
#include <QPointF>
#include <QString>

class QPixmap;

namespace mikai::app {

/// Cover thumbnail that reports activation so the page can open it full size.
class CoverLabel : public QLabel {
    Q_OBJECT

public:
    explicit CoverLabel(const QString& text, QWidget* parent = nullptr);
    ~CoverLabel() override;

    void setZoomable(bool zoomable);
    bool isZoomable() const { return zoomable_; }

Q_SIGNALS:
    void activated();

protected:
    void mousePressEvent(QMouseEvent* event) override;

private:
    bool zoomable_ = false;
};

/// Frameless full-screen cover overlay: a sharp image floating over a blurred
/// copy of itself.
///
/// Deliberately chrome-free — no title bar, no buttons, no scrollbars. A click
/// on the empty background dismisses it, the wheel zooms under the cursor, and
/// dragging pans once zoomed past the fit scale.
class CoverViewer : public QDialog {
    Q_OBJECT

public:
    CoverViewer(const QPixmap& pixmap, const QString& caption, QWidget* parent = nullptr);
    ~CoverViewer() override;

private:
    class View;

    View* view_ = nullptr;
};

}  // namespace mikai::app
