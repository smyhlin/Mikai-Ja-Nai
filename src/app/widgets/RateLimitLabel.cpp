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
    timer_->start();
    refresh();
}

RateLimitLabel::~RateLimitLabel() = default;

void RateLimitLabel::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::LanguageChange) {
        // The words are re-rendered from the snapshot already on screen, not
        // re-read from the client: a language switch must not move the quota,
        // the reset window or the queue depth, and it must not ask the server
        // for anything. The timer keeps running on its own interval.
        renderState(state_);
    }
}

void RateLimitLabel::refresh() {
    state_ = client_.rateLimitState();
    renderState(state_);
}

void RateLimitLabel::renderState(const backend::RateLimitState& state) {
    if (!state.used.has_value() || !state.limit) {
        label_->setText(
            QStringLiteral("RPM —/%1").arg(state.limit > 0 ? QString::number(state.limit)
                                                           : QStringLiteral("—")));
        const QString detail = tr("No response with rate limit headers yet");
        label_->setToolTip(detail);
        label_->setAccessibleName(detail);
        return;
    }

    label_->setText(QStringLiteral("RPM %1/%2").arg(*state.used).arg(state.limit));
    QString detail = tr("Last API response: %1 of %2 requests used in the current window")
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
    label_->setToolTip(detail);
    label_->setAccessibleName(detail);
}

}  // namespace mikai::app
