#include "widgets/RateLimitLabel.hpp"

#include <backend/MikaiClient.hpp>

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

void RateLimitLabel::refresh() {
    const backend::RateLimitState state = client_.rateLimitState();
    if (!state.used.has_value() || !state.limit) {
        label_->setText(
            QStringLiteral("RPM —/%1").arg(state.limit > 0 ? QString::number(state.limit)
                                                           : QStringLiteral("—")));
        label_->setToolTip(tr("No response with rate limit headers yet"));
    } else {
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
    }
}

}  // namespace mikai::app
