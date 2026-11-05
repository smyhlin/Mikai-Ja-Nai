#include "dialogs/CoverViewer.hpp"

#include "theme/Colors.hpp"
#include "theme/Metrics.hpp"

#include <QEvent>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QRadialGradient>
#include <QScreen>
#include <QShowEvent>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <vector>

namespace mikai::app {
namespace {

/// Fast 3-pass separable box blur (converges to a smooth Gaussian kernel) on an
/// ARGB32_Premultiplied working buffer so the fullscreen cover backdrop has zero
/// blocky pixelation or Mach banding.
void boxBlurHorizontal(const QRgb* src, QRgb* dst, int width, int height, int radius) {
    const int diameter = radius * 2 + 1;
    for (int y = 0; y < height; ++y) {
        const QRgb* rowIn = src + y * width;
        QRgb* rowOut = dst + y * width;
        int sumR = 0, sumG = 0, sumB = 0, sumA = 0;
        for (int i = -radius; i <= radius; ++i) {
            const QRgb p = rowIn[std::clamp(i, 0, width - 1)];
            sumR += qRed(p);
            sumG += qGreen(p);
            sumB += qBlue(p);
            sumA += qAlpha(p);
        }
        for (int x = 0; x < width; ++x) {
            rowOut[x] = qRgba(sumR / diameter, sumG / diameter, sumB / diameter, sumA / diameter);
            const QRgb removeP = rowIn[std::max(0, x - radius)];
            const QRgb addP = rowIn[std::min(width - 1, x + radius + 1)];
            sumR += qRed(addP) - qRed(removeP);
            sumG += qGreen(addP) - qGreen(removeP);
            sumB += qBlue(addP) - qBlue(removeP);
            sumA += qAlpha(addP) - qAlpha(removeP);
        }
    }
}

void boxBlurVertical(const QRgb* src, QRgb* dst, int width, int height, int radius) {
    const int diameter = radius * 2 + 1;
    for (int x = 0; x < width; ++x) {
        int sumR = 0, sumG = 0, sumB = 0, sumA = 0;
        for (int i = -radius; i <= radius; ++i) {
            const QRgb p = src[std::clamp(i, 0, height - 1) * width + x];
            sumR += qRed(p);
            sumG += qGreen(p);
            sumB += qBlue(p);
            sumA += qAlpha(p);
        }
        for (int y = 0; y < height; ++y) {
            dst[y * width + x] =
                qRgba(sumR / diameter, sumG / diameter, sumB / diameter, sumA / diameter);
            const QRgb removeP = src[std::max(0, y - radius) * width + x];
            const QRgb addP = src[std::min(height - 1, y + radius + 1) * width + x];
            sumR += qRed(addP) - qRed(removeP);
            sumG += qGreen(addP) - qGreen(removeP);
            sumB += qBlue(addP) - qBlue(removeP);
            sumA += qAlpha(addP) - qAlpha(removeP);
        }
    }
}

QPixmap blurredBackdrop(const QPixmap& pixmap, const QSize& size) {
    if (pixmap.isNull() || size.isEmpty()) {
        return {};
    }
    constexpr int kWorkingExtent = 320;
    const double aspect = static_cast<double>(size.width()) / std::max(1, size.height());
    const QSize workSize =
        aspect >= 1.0
            ? QSize(kWorkingExtent, std::max(1, qRound(kWorkingExtent / aspect)))
            : QSize(std::max(1, qRound(kWorkingExtent * aspect)), kWorkingExtent);

    QImage scaled = pixmap.toImage().scaled(workSize, Qt::KeepAspectRatioByExpanding,
                                            Qt::SmoothTransformation);
    if (scaled.isNull()) {
        return {};
    }
    const int cropX = std::max(0, (scaled.width() - workSize.width()) / 2);
    const int cropY = std::max(0, (scaled.height() - workSize.height()) / 2);
    QImage work = scaled.copy(cropX, cropY, workSize.width(), workSize.height())
                      .convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const int w = work.width();
    const int h = work.height();
    if (w > 2 && h > 2) {
        const int radius = std::clamp(std::min(w, h) / 10, 8, 22);
        std::vector<QRgb> temp(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
        auto* pixels = reinterpret_cast<QRgb*>(work.bits());
        for (int pass = 0; pass < 3; ++pass) {
            boxBlurHorizontal(pixels, temp.data(), w, h, radius);
            boxBlurVertical(temp.data(), pixels, w, h, radius);
        }
    }
    return QPixmap::fromImage(
        work.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
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
        backdropTimer_->setInterval(16);
        connect(backdropTimer_, &QTimer::timeout, this, [this] {
            if (backdrop_.size() != size()) {
                backdrop_ = blurredBackdrop(image_, size());
                update();
            }
        });
    }

    void setImage(const QPixmap& pixmap) {
        image_ = pixmap;
        // The poster is measured in its own pixels - the view itself maps them
        // onto device pixels - so an incoming ratio tag must not rescale the
        // source rect of the blit when the window sits on another screen. Only
        // a tagged pixmap is retagged; the common untagged cover is not copied.
        if (image_.devicePixelRatio() != 1.0) {
            image_.setDevicePixelRatio(1.0);
        }
        zoom_ = 1.0;
        pan_ = QPointF();
        backdrop_ = blurredBackdrop(image_, size());
        recomputeFit();
    }

    QSize sizeHint() const override { return targetSize().toSize(); }

protected:
    void paintEvent(QPaintEvent* event) override {
        Q_UNUSED(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter.fillRect(rect(), Qt::black);
        if (!backdrop_.isNull()) {
            painter.drawPixmap(rect(), backdrop_, QRectF(backdrop_.rect()));
            painter.fillRect(rect(), QColor(10, 10, 16, theme::kCoverBackdropAlpha));
            QRadialGradient vignette(rect().center(),
                                     std::max(width(), height()) * 0.72);
            vignette.setColorAt(0.0, QColor(0, 0, 0, 0));
            vignette.setColorAt(0.65, QColor(5, 5, 10, 45));
            vignette.setColorAt(1.0, QColor(5, 5, 10, 140));
            painter.fillRect(rect(), vignette);
        }
        if (image_.isNull()) {
            return;
        }
        const QRectF target = imageRect();
        // Subtle ambient shadow behind the poster so it lifts off the blurred field.
        for (int step = 3; step >= 1; --step) {
            const qreal spread = step * 5.0;
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(0, 0, 0, 32));
            painter.drawRoundedRect(target.adjusted(-spread, -spread + 4.0, spread, spread + 4.0),
                                    theme::kRadiusSmall + spread * 0.5,
                                    theme::kRadiusSmall + spread * 0.5);
        }
        painter.drawPixmap(target, image_, QRectF(image_.rect()));
        painter.setPen(QPen(QColor(255, 255, 255, 28), 1.0));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(target.adjusted(0.5, 0.5, -0.5, -0.5));
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

    bool event(QEvent* event) override {
        // Qt consumes both of these in QWidget::event() and never forwards them
        // to changeEvent(), so the re-measure has to be driven from here.
        const bool ratioChanged = event->type() == QEvent::DevicePixelRatioChange ||
                                  event->type() == QEvent::ScreenChangeInternal;
        const bool result = QWidget::event(event);
        if (ratioChanged) {
            // Fit, 1:1 and the whole-pixel geometry are all expressed in device
            // pixels, so a window that lands on a screen with a different ratio
            // re-measures them instead of keeping the previous screen's answer.
            recomputeFit();
            update();
        }
        return result;
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

    /// Screen pixels per logical pixel of this window. Everything the poster is
    /// measured against is a bitmap in device pixels, so the ratio belongs in
    /// every geometry decision.
    qreal ratio() const {
        const qreal value = devicePixelRatioF();
        return value > 0.0 ? value : 1.0;
    }

    double scale() const { return fit_ * zoom_; }

    /// Drawn size in logical pixels, rounded to whole device pixels: an
    /// unzoomed poster then lands on the pixel grid instead of being resampled
    /// between two of them.
    QSizeF targetSize() const {
        const qreal dpr = ratio();
        return QSizeF(qMax(1, qRound(image_.width() * scale() * dpr)) / dpr,
                      qMax(1, qRound(image_.height() * scale() * dpr)) / dpr);
    }

    QRectF imageRect() const {
        const QSizeF size = targetSize();
        const QPointF centre(width() / 2.0 + pan_.x(), height() / 2.0 + pan_.y());
        const qreal dpr = ratio();
        // Whole device pixels for the origin as well: a fractionally translated
        // bitmap is interpolated even when it is not scaled.
        return QRectF(QPointF(qRound((centre.x() - size.width() / 2.0) * dpr) / dpr,
                              qRound((centre.y() - size.height() / 2.0) * dpr) / dpr),
                      size);
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

    /// Flip between fitting the surface and one source pixel per device pixel.
    void toggleActualSize() {
        // 1:1 is not a zoom of 1/fit on a scaled screen: the poster also has to
        // clear the device ratio before its pixels map one to one. A poster
        // that already fits at native size has nowhere to toggle to.
        const double actual = 1.0 / qMax(1e-9, fit_ * ratio());
        setZoom(qAbs(zoom_ - actual) < 1e-6 ? 1.0 : actual);
    }

    /// Rebuild the blur once the resize settles.
    void scheduleBackdrop() {
        if (image_.isNull()) {
            return;
        }
        if (!backdropTimer_->isActive()) {
            backdropTimer_->start();
        }
    }

    /// True when the poster currently maps one source pixel to one device pixel.
    bool atActualSize() const {
        return !isFitted() && qAbs(zoom_ - 1.0 / qMax(1e-9, fit_ * ratio_)) < 1e-6;
    }

    void recomputeFit() {
        if (image_.isNull() || width() <= 0 || height() <= 0) {
            return;
        }
        const double dpr = ratio();
        // Never upscale past native resolution just to fill the surface. The
        // surface is measured in logical units while the ceiling is the
        // poster's own device pixels, so the ratio caps the fit scale at 1/ratio.
        const double fit =
            qMin(1.0 / dpr, qMin(double(width()) / image_.width(),
                                 double(height()) / image_.height()));
        if (qAbs(fit - fit_) < 1e-9 && qFuzzyCompare(dpr, ratio_)) {
            return;
        }
        if (atActualSize()) {
            // 1:1 is a device-pixel state, not a logical zoom: re-derive it
            // from the new ratio instead of carrying the old multiple, which
            // would leave the poster resampled off its native mapping.
            zoom_ = qBound(theme::kCoverZoomMin, 1.0 / qMax(1e-9, fit * dpr),
                           theme::kCoverZoomMax);
        } else if (!isFitted()) {
            // Any other zoom the user set is a multiple of whatever the fit is
            // now; the poster keeps the size it had while the fit takes over.
            zoom_ = qBound(theme::kCoverZoomMin, zoom_ * fit_ / qMax(1e-9, fit),
                           theme::kCoverZoomMax);
        }
        fit_ = fit;
        ratio_ = dpr;
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
    double ratio_ = 1.0;
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
