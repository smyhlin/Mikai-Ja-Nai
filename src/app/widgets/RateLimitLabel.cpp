#include "widgets/RateLimitLabel.hpp"

#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QTimer>

namespace mikai::app {

RateLimitLabel::RateLimitLabel(backend::MikaiClient& client, QWidget* parent)
    : QWidget(parent), client_(client) {
    label_ = new QLabel(this);
    label_->setObjectName(QStringLiteral("muted"));
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(label_);

    timer_ = new QTimer(this);
    timer_->setInterval(1000);
    connect(timer_, &QTimer::timeout, this, &RateLimitLabel::refresh);
    // T-412: the tick is armed by showEvent below rather than here, so an
    // indicator that is not on screen never burns a timer slot. The first
    // paint still lands immediately.
    refresh();
}

RateLimitLabel::~RateLimitLabel() = default;

void RateLimitLabel::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::LanguageChange) {
        // The words are re-rendered from the snapshot already on screen, not
        // re-read from the client: a language switch must not move the quota,
        // the reset window or the queue depth, and it must not ask the server
        // for anything. The rendered-cache comparison below notices the new
        // wording and repaints.
        renderState(state_);
    }
}

void RateLimitLabel::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    // T-412: a 1 Hz refresh only makes sense while the line can be seen. The
    // immediate refresh also means a widget that was hidden for minutes does
    // not display a stale quota for its first second back.
    refresh();
    timer_->start();
}

void RateLimitLabel::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    timer_->stop();
}

void RateLimitLabel::refresh() {
    state_ = client_.rateLimitState();
    renderState(state_);
}

void RateLimitLabel::renderState(const backend::RateLimitState& state) {
    QString text;
    QString detail;
    if (!state.used.has_value() || !state.limit) {
        text = QStringLiteral("RPM —/%1").arg(state.limit > 0 ? QString::number(state.limit)
                                                            : QStringLiteral("—"));
        detail = tr("No response with rate limit headers yet");
    } else {
        text = QStringLiteral("RPM %1/%2").arg(*state.used).arg(state.limit);
        detail = tr("Last API response: %1 of %2 requests used in the current window")
                     .arg(*state.used)
                     .arg(state.limit);
        if (state.remaining.has_value()) {
            detail += tr("; %1 remaining").arg(*state.remaining);
        }
        if (state.resetIn > 0) {
            detail += tr("; resets in %1 s").arg(state.resetIn);
        }
        if (state.queued > 0) {
            detail += tr("; %1 queued").arg(state.queued);
        }
    }
    // T-412: setText on an unchanged string still invalidates the label's
    // layout and schedules a repaint, so an identical line is skipped. The
    // cached pair is the rendered (translated) text, which is what lets a
    // LanguageChange through while every unchanged tick is a no-op.
    if (text == renderedText_ && detail == renderedDetail_) {
        return;
    }
    renderedText_ = text;
    renderedDetail_ = detail;
    label_->setText(text);
    label_->setToolTip(detail);
    label_->setAccessibleName(detail);
}

}  // namespace mikai::app
