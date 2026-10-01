#include "dialogs/CoverViewer.hpp"

#include "theme/Colors.hpp"
#include "theme/Metrics.hpp"

#include <QGuiApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QScreen>
#include <QShowEvent>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>

namespace mikai::app {
namespace {

/// The blurred backdrop, kept small.
///
/// A downscale-then-upscale pass is the whole trick: Qt's smooth scaler is a
/// box filter, so two passes at different factors read as a real blur and cost
/// two C++ calls instead of a per-pixel loop. Both passes run at backdrop
/// resolution rather than screen resolution, and the last upscale is left to
/// the painter, so a resize costs two scalings over a buffer a fraction of the
/// screen instead of four over the whole of it.
QPixmap blurredBackdrop(const QPixmap& pixmap, const QSize& size) {
    if (pixmap.isNull() || size.isEmpty()) {
        return {};
    }
    QImage image = pixmap.toImage();
    if (image.isNull()) {
        return {};
    }
    const QSize small(qMax(1, size.width() / theme::kCoverBackdropDownscale),
                      qMax(1, size.height() / theme::kCoverBackdropDownscale));
    image = image.scaled(small, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation)
                .scaled(QSize(small.width() * 2, small.height() * 2),
                        Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    // Deliberately not fill(): that rewrites every channel, alpha included, so
    // it erases the blur instead of dimming it and the overlay ends up on a flat
    // black field.
    return QPixmap::fromImage(image);
}

}  // namespace

CoverLabel::CoverLabel(const QString& text, QWidget* parent) : QLabel(text, parent) {
    setAlignment(Qt::AlignCenter);
}

CoverLabel::~CoverLabel() = default;

void CoverLabel::setZoomable(bool zoomable) {
    zoomable_ = zoomable;
    setCursor(zoomable ? Qt::PointingHandCursor : Qt::ArrowCursor);
}

void CoverLabel::mousePressEvent(QMouseEvent* event) {
    if (zoomable_ && event->button() == Qt::LeftButton) {
        Q_EMIT activated();
    }
    QLabel::mousePressEvent(event);
}

/// The image surface: blurred backdrop behind, sharp image in front.
class CoverViewer::View : public QWidget {
    Q_OBJECT

public:
    explicit View(QWidget* parent) : QWidget(parent) {
        setObjectName(QStringLiteral("coverViewerView"));
        setMouseTracking(true);
        setFocusPolicy(Qt::StrongFocus);
        setCursor(Qt::OpenHandCursor);
        backdropTimer_ = new QTimer(this);
        backdropTimer_->setSingleShot(true);
        backdropTimer_->setInterval(0);
        connect(backdropTimer_, &QTimer::timeout, this, [this] {
            if (backdrop_.size() != size()) {
                backdrop_ = blurredBackdrop(image_, size());
                update();
            }
        });
    }

    void setImage(const QPixmap& pixmap) {
        image_ = pixmap;
        zoom_ = 1.0;
        pan_ = QPointF();
        backdrop_ = blurredBackdrop(image_, size());
        recomputeFit();
    }

    QSize sizeHint() const override { return targetSize(); }

protected:
    void paintEvent(QPaintEvent* event) override {
        Q_UNUSED(event);
        QPainter painter(this);
        painter.fillRect(rect(), Qt::black);
        if (!backdrop_.isNull()) {
            // The buffer is a fraction of the surface, so the upscale to fill it
            // is the painter's one scaling pass rather than a third and fourth
            // over the full resolution.
            painter.drawPixmap(rect(), backdrop_, QRectF(backdrop_.rect()));
            // An alpha dim, not a fill: filling would rewrite the blur away.
            painter.fillRect(rect(), QColor(0, 0, 0, theme::kCoverBackdropAlpha));
        }
        if (image_.isNull()) {
            return;
        }
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter.drawPixmap(imageRect(), image_, QRectF(image_.rect()));
    }

    void wheelEvent(QWheelEvent* event) override {
        if (image_.isNull()) {
            QWidget::wheelEvent(event);
            return;
        }
        const int delta = event->angleDelta().y();
        if (delta == 0) {
            QWidget::wheelEvent(event);
            return;
        }
        zoomAtCursor(delta > 0 ? theme::kCoverZoomStep : 1.0 / theme::kCoverZoomStep, event);
        event->accept();
    }

    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton || image_.isNull()) {
            QWidget::mousePressEvent(event);
            return;
        }
        if (!imageRect().contains(event->position().toPoint())) {
            Q_EMIT backgroundClicked();
            event->accept();
            return;
        }
        setFocus(Qt::MouseFocusReason);
        dragOrigin_ = event->position();
        panOrigin_ = pan_;
        hasDrag_ = true;
        setCursor(Qt::ClosedHandCursor);
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        if (!hasDrag_ || isFitted()) {
            return;
        }
        pan_ = QPointF(panOrigin_.x() + event->position().x() - dragOrigin_.x(),
                       panOrigin_.y() + event->position().y() - dragOrigin_.y());
        clampPan();
        update();
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        Q_UNUSED(event);
        hasDrag_ = false;
        setCursor(Qt::OpenHandCursor);
    }

    void mouseDoubleClickEvent(QMouseEvent* event) override {
        toggleActualSize();
        event->accept();
    }

    void resizeEvent(QResizeEvent* event) override {
        QWidget::resizeEvent(event);
        recomputeFit();
        scheduleBackdrop();
    }

    void keyPressEvent(QKeyEvent* event) override {
        switch (event->key()) {
            case Qt::Key_Escape:
                Q_EMIT backgroundClicked();
                return;
            case Qt::Key_Plus:
            case Qt::Key_Equal:
                setZoom(zoom_ * theme::kCoverZoomStep);
                return;
            case Qt::Key_Minus:
                setZoom(zoom_ / theme::kCoverZoomStep);
                return;
            case Qt::Key_0:
                setZoom(1.0);
                return;
            case Qt::Key_1:
                toggleActualSize();
                return;
            default:
                break;
        }
        QWidget::keyPressEvent(event);
    }

Q_SIGNALS:
    void backgroundClicked();
    void zoomChanged(double scale);

private:
    bool isFitted() const { return qAbs(zoom_ - 1.0) < 1e-6; }

    double scale() const { return fit_ * zoom_; }

    QSize targetSize() const {
        return QSize(qMax(1, qRound(image_.width() * scale())),
                     qMax(1, qRound(image_.height() * scale())));
    }

    QRectF imageRect() const {
        const QSize size = targetSize();
        const QPointF centre(width() / 2.0 + pan_.x(), height() / 2.0 + pan_.y());
        return QRectF(QPointF(qRound(centre.x() - size.width() / 2.0),
                             qRound(centre.y() - size.height() / 2.0)),
                      QSizeF(size));
    }

    void setZoom(double zoom) {
        const double target = qBound(theme::kCoverZoomMin, zoom, theme::kCoverZoomMax);
        if (qAbs(target - zoom_) < 1e-6) {
            return;
        }
        zoom_ = target;
        if (isFitted()) {
            pan_ = QPointF();
        }
        clampPan();
        update();
        Q_EMIT zoomChanged(scale());
    }

    /// Flip between fitting the surface and showing the image at 1:1 pixels.
    void toggleActualSize() {
        setZoom(fit_ > 0.0 && zoom_ >= 1.0 ? 1.0 / fit_ : 1.0);
    }

    /// Rebuild the blur once the resize settles.
    ///
    /// A drag raises a resize event per frame, and each one used to rebuild the
    /// blur before the next frame was even drawn. One coalescing timer turns the
    /// whole drag into a single rebuild. The stale buffer keeps painting in the
    /// meantime, which on a blur is invisible where a cleared one would flash.
    void scheduleBackdrop() {
        if (image_.isNull()) {
            return;
        }
        if (!backdropTimer_->isActive()) {
            backdropTimer_->start();
        }
    }

    void recomputeFit() {
        if (image_.isNull() || width() <= 0 || height() <= 0) {
            return;
        }
        // Never upscale past native resolution just to fill the surface.
        const double fit =
            qMin(1.0, qMin(double(width()) / image_.width(), double(height()) / image_.height()));
        if (qAbs(fit - fit_) < 1e-9) {
            return;
        }
        // Only reflow while fitted. Once zoomed the scale is the user's, and
        // re-fitting under them would fight that choice and never settle.
        if (isFitted()) {
            fit_ = fit;
        }
        clampPan();
        update();
    }

    /// Keep the image from drifting entirely off the surface.
    void clampPan() {
        if (isFitted()) {
            pan_ = QPointF();
            return;
        }
        const QRectF rect = imageRect();
        const double slackX = qMax(0.0, (rect.width() - width()) / 2.0);
        const double slackY = qMax(0.0, (rect.height() - height()) / 2.0);
        pan_.setX(qBound(-slackX, pan_.x(), slackX));
        pan_.setY(qBound(-slackY, pan_.y(), slackY));
    }

    /// Zoom while keeping the pixel under the pointer where it is.
    void zoomAtCursor(double factor, QWheelEvent* event) {
        const QPointF anchor = event->position();
        const QRectF before = imageRect();
        if (before.isEmpty()) {
            return;
        }
        const double fx = (anchor.x() - before.left()) / qMax(1.0, before.width());
        const double fy = (anchor.y() - before.top()) / qMax(1.0, before.height());
        setZoom(zoom_ * factor);
        const QRectF after = imageRect();
        if (after.isEmpty()) {
            return;
        }
        pan_.setX(pan_.x() + anchor.x() - (after.left() + fx * after.width()));
        pan_.setY(pan_.y() + anchor.y() - (after.top() + fy * after.height()));
        clampPan();
        update();
    }

    QPixmap image_;
    QPixmap backdrop_;
    QTimer* backdropTimer_ = nullptr;
    double zoom_ = 1.0;
    double fit_ = 1.0;
    QPointF pan_;
    QPointF dragOrigin_;
    QPointF panOrigin_;
    bool hasDrag_ = false;
};

CoverViewer::CoverViewer(const QPixmap& pixmap, const QString& caption, QWidget* parent)
    : QDialog(parent) {
    setObjectName(QStringLiteral("coverViewer"));
    setWindowTitle(caption.isEmpty() ? tr("Cover") : tr("Cover — %1").arg(caption));
    setWindowFlag(Qt::FramelessWindowHint, true);
    setWindowFlag(Qt::Dialog, true);

    view_ = new View(this);
    view_->setImage(pixmap);
    connect(view_, &View::backgroundClicked, this, &QDialog::close);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(view_);
}

CoverViewer::~CoverViewer() = default;

void CoverViewer::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    const QWidget* anchor = parentWidget() != nullptr ? parentWidget() : this;
    QScreen* screen = anchor->screen();
    if (screen == nullptr) {
        screen = QGuiApplication::primaryScreen();
    }
    if (screen != nullptr) {
        setGeometry(screen->availableGeometry());
    }
}

}  // namespace mikai::app

#include "CoverViewer.moc"
