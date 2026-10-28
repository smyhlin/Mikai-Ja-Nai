#include "VideoDropTarget.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QDropEvent>
#include <QKeyEvent>
#include <QFontMetrics>
#include <QMimeData>
#include <QPainter>
#include <QPalette>
#include <QStringView>
#include <QUrl>
#include <QWidget>

#include <algorithm>

namespace mikai::gui {
namespace {

constexpr int kMargin = 24;
constexpr int kPadding = 24;
constexpr int kGap = 12;
constexpr int kSmallGap = 4;
constexpr int kIconSize = 48;
constexpr int kCardWidth = 520;
constexpr int kRadius = 12;

class DropOverlay final : public QWidget {
public:
    DropOverlay(QWidget* host, VideoDropTarget* target) : QWidget(host), target_(target) {
        setObjectName(QStringLiteral("videoDropOverlay"));
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setAutoFillBackground(false);
        setFocusPolicy(Qt::NoFocus);
        hide();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        if (target_.isNull()) return;
        QPainter painter(this);
        target_->paintIndicator(painter, rect());
    }

private:
    QPointer<VideoDropTarget> target_;
};

} // namespace

VideoDropTarget::VideoDropTarget(QWidget* host, OverlayMode mode) : QObject(host), host_(host) {
    host->setAcceptDrops(true);
    host->window()->setAcceptDrops(true);
    if (mode == OverlayMode::Widget) overlay_ = new DropOverlay(host, this);
    leaveTimer_.setSingleShot(true);
    leaveTimer_.setInterval(0);
    connect(&leaveTimer_, &QTimer::timeout, this, [this] {
        if (leaveGeneration_ == dragGeneration_) clearIndicator();
    });
    qApp->installEventFilter(this);
}

VideoDropTarget::~VideoDropTarget() {
    if (qApp != nullptr) qApp->removeEventFilter(this);
    leaveTimer_.stop();
    delete overlay_.data();
}

QString VideoDropTarget::videoPath(const QMimeData* mime) {
    if (mime == nullptr || !mime->hasUrls()) return {};
    static constexpr QStringView suffixes[]{
        u"mp4", u"mkv", u"webm", u"avi", u"mov", u"m4v",
        u"ts", u"flv", u"m2ts", u"mpeg", u"mpg"};
    for (const QUrl& url : mime->urls()) {
        if (!url.isLocalFile()) continue;
        const QString path = url.toLocalFile();
        const QStringView view(path);
        const qsizetype separator = std::max(view.lastIndexOf(u'/'), view.lastIndexOf(u'\\'));
        const qsizetype dot = view.lastIndexOf(u'.');
        if (dot <= separator || dot + 1 == view.size()) continue;
        const QStringView suffix = view.sliced(dot + 1);
        for (const QStringView supported : suffixes) {
            if (suffix.compare(supported, Qt::CaseInsensitive) == 0) return path;
        }
    }
    return {};
}

void VideoDropTarget::refreshAppearance() {
    if (host_.isNull()) return;
    title_ = QCoreApplication::translate("VideoDropTarget", "Release to open video");
    hint_ = QCoreApplication::translate("VideoDropTarget", "Drop the file anywhere in this window");
    bodyFont_ = host_->font();
    titleFont_ = bodyFont_;
    titleFont_.setBold(true);
    if (titleFont_.pointSizeF() > 0.0) titleFont_.setPointSizeF(titleFont_.pointSizeF() + 4.0);
    else titleFont_.setPixelSize(titleFont_.pixelSize() + 6);
    const QPalette palette = host_->palette();
    background_ = palette.color(QPalette::Window);
    foreground_ = palette.color(QPalette::WindowText);
    border_ = foreground_;
    border_.setAlpha(140);
    muted_ = foreground_;
    muted_.setAlpha(180);
    scrim_ = background_;
    scrim_.setAlpha(210);
    titleHeight_ = QFontMetrics(titleFont_).height();
    bodyHeight_ = QFontMetrics(bodyFont_).height();
    textWidth_ = -1;
    if (!overlay_.isNull()) {
        overlay_->setAccessibleName(title_);
        overlay_->setAccessibleDescription(fileName_ + QStringLiteral(". ") + hint_);
        overlay_->update();
    }
}

void VideoDropTarget::beginIndicator(const QString& path) {
    ++dragGeneration_;
    leaveTimer_.stop();
    if (path_ == path) return;
    path_ = path;
    const qsizetype separator = std::max(path.lastIndexOf(u'/'), path.lastIndexOf(u'\\'));
    fileName_ = path.sliced(separator + 1);
    refreshAppearance();
    if (!overlay_.isNull()) {
        overlay_->setGeometry(host_->rect());
        overlay_->show();
        overlay_->raise();
    }
    emit indicatorChanged();
}

void VideoDropTarget::clearIndicator() {
    ++dragGeneration_;
    leaveTimer_.stop();
    if (path_.isEmpty()) return;
    path_.clear();
    if (!overlay_.isNull()) overlay_->hide();
    emit indicatorChanged();
}

bool VideoDropTarget::eventFilter(QObject* watched, QEvent* event) {
    if (host_.isNull()) return false;
    const auto type = event->type();
    if (watched == host_ && type == QEvent::Destroy) {
        leaveTimer_.stop();
        path_.clear();
        host_.clear();
        return false;
    }
    if (type == QEvent::ApplicationDeactivate ||
        (type == QEvent::KeyPress && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape)) {
        clearIndicator();
        return false;
    }
    QWidget* const window = host_->window();
    if (watched == window) {
        if (type == QEvent::WindowBlocked) windowBlocked_ = true;
        if (type == QEvent::WindowUnblocked) windowBlocked_ = false;
        if (type == QEvent::WindowBlocked || type == QEvent::WindowDeactivate ||
            type == QEvent::Hide) {
            clearIndicator();
            return false;
        }
    }
    if (watched == host_) {
        if (type == QEvent::Hide || type == QEvent::EnabledChange) {
            if (!host_->isVisible() || !host_->isEnabled()) clearIndicator();
            return false;
        }
        if (type == QEvent::ParentChange) {
            windowBlocked_ = false;
            host_->window()->setAcceptDrops(true);
            clearIndicator();
            return false;
        }
        if (type == QEvent::Resize && !overlay_.isNull()) overlay_->setGeometry(host_->rect());
        if (!path_.isEmpty() && (type == QEvent::LanguageChange || type == QEvent::FontChange ||
                                type == QEvent::PaletteChange || type == QEvent::StyleChange)) {
            refreshAppearance();
            emit indicatorChanged();
            return false;
        }
    }
    if (!path_.isEmpty() && !overlay_.isNull() && watched != overlay_ &&
        (type == QEvent::Show || type == QEvent::ZOrderChange)) {
        auto* child = qobject_cast<QWidget*>(watched);
        if (child != nullptr && host_->isAncestorOf(child)) overlay_->raise();
    }
    if (type != QEvent::DragEnter && type != QEvent::DragMove &&
        type != QEvent::DragLeave && type != QEvent::Drop) return false;
    auto* widget = qobject_cast<QWidget*>(watched);
    if (widget == nullptr) return false;
    QWidget* const modal = QApplication::activeModalWidget();
    if (!host_->isVisible() || !host_->isEnabled() || windowBlocked_ ||
        (modal != nullptr && modal->window() != window) ||
        widget->window() != window) {
        clearIndicator();
        return false;
    }
    if (type == QEvent::DragLeave) {
        if (!active()) return false;
        // Qt leaves the old child before entering its sibling; keep the indicator
        // until that handoff has had a chance to deliver another enter or move.
        leaveGeneration_ = dragGeneration_;
        leaveTimer_.start();
        event->accept();
        return true;
    }
    auto* drop = static_cast<QDropEvent*>(event);
    if (!drop->possibleActions().testFlag(Qt::CopyAction)) {
        clearIndicator();
        return false;
    }
    const QString path = videoPath(drop->mimeData());
    if (path.isEmpty()) {
        clearIndicator();
        return false;
    }
    drop->setDropAction(Qt::CopyAction);
    drop->accept();
    if (type != QEvent::Drop) {
        beginIndicator(path);
        return true;
    }
    QPointer<VideoDropTarget> guard(this);
    clearIndicator();
    if (!guard.isNull()) emit guard->videoDropped(path);
    return true;
}

void VideoDropTarget::paintIndicator(QPainter& painter, const QRect& bounds) const {
    if (path_.isEmpty() || bounds.isEmpty()) return;
    painter.save();
    painter.setClipRect(bounds, Qt::IntersectClip);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.fillRect(bounds, scrim_);
    const int margin = std::min({kMargin, bounds.width() / 8, bounds.height() / 8});
    const QRect available = bounds.adjusted(margin, margin, -margin, -margin);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(border_, 2, Qt::DashLine));
    painter.drawRoundedRect(QRectF(available), kRadius, kRadius);
    const int padding = std::min(kPadding, available.width() / 8);
    const int fullHeight = 2 * padding + kIconSize + 2 * kGap + kSmallGap +
                           titleHeight_ + 2 * bodyHeight_;
    const QSize size(std::min(kCardWidth, available.width()), std::min(fullHeight, available.height()));
    const QRect card(available.center() - QPoint(size.width() / 2, size.height() / 2), size);
    painter.setPen(QPen(border_, 1));
    painter.setBrush(background_);
    painter.drawRoundedRect(QRectF(card), kRadius, kRadius);
    const QRect content = card.adjusted(padding, padding, -padding, -padding);
    const int textWidth = content.width();
    if (textWidth <= 0 || content.height() < bodyHeight_) {
        painter.restore();
        return;
    }
    if (textWidth_ != textWidth) {
        textWidth_ = textWidth;
        displayedTitle_ = QFontMetrics(titleFont_).elidedText(title_, Qt::ElideRight, textWidth);
        const QFontMetrics bodyMetrics(bodyFont_);
        displayedFileName_ = bodyMetrics.elidedText(fileName_, Qt::ElideMiddle, textWidth);
        displayedHint_ = bodyMetrics.elidedText(hint_, Qt::ElideRight, textWidth);
    }
    const bool showTitle = content.height() >= titleHeight_ + kSmallGap + bodyHeight_;
    const bool showHint = content.height() >= titleHeight_ + kSmallGap + 2 * bodyHeight_ + kGap;
    const int textHeight = (showTitle ? titleHeight_ + kSmallGap : 0) + bodyHeight_ +
                           (showHint ? kGap + bodyHeight_ : 0);
    const bool showIcon = content.height() >= textHeight + kIconSize + kGap;
    const int totalHeight = textHeight + (showIcon ? kIconSize + kGap : 0);
    int y = content.top() + (content.height() - totalHeight) / 2;
    if (showIcon) {
        painter.save();
        painter.translate(card.center().x() - kIconSize / 2, y);
        painter.scale(kIconSize, kIconSize);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(foreground_, 0.04, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        constexpr QPointF file[]{QPointF(0.2, 0.08), QPointF(0.62, 0.08), QPointF(0.8, 0.27),
                                QPointF(0.8, 0.92), QPointF(0.2, 0.92)};
        constexpr QPointF fold[]{QPointF(0.62, 0.08), QPointF(0.62, 0.27), QPointF(0.8, 0.27)};
        constexpr QPointF play[]{QPointF(0.42, 0.42), QPointF(0.42, 0.72), QPointF(0.65, 0.57)};
        painter.drawPolygon(file, 5);
        painter.drawPolyline(fold, 3);
        painter.setBrush(foreground_);
        painter.drawPolygon(play, 3);
        painter.restore();
        y += kIconSize + kGap;
    }
    painter.setPen(foreground_);
    if (showTitle) {
        painter.setFont(titleFont_);
        painter.drawText(QRect(content.left(), y, textWidth, titleHeight_),
                         Qt::AlignCenter | Qt::TextSingleLine, displayedTitle_);
        y += titleHeight_ + kSmallGap;
    }
    painter.setFont(bodyFont_);
    painter.drawText(QRect(content.left(), y, textWidth, bodyHeight_),
                     Qt::AlignCenter | Qt::TextSingleLine, displayedFileName_);
    if (showHint) {
        painter.setPen(muted_);
        painter.drawText(QRect(content.left(), y + bodyHeight_ + kGap, textWidth, bodyHeight_),
                         Qt::AlignCenter | Qt::TextSingleLine, displayedHint_);
    }
    painter.restore();
}

} // namespace mikai::gui
