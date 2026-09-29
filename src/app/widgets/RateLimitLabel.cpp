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
        label_->setToolTip(tr("Ще немає відповіді із заголовками ліміту"));
    } else {
        label_->setText(QStringLiteral("RPM %1/%2").arg(*state.used).arg(state.limit));
        QString detail = tr("Остання відповідь API: використано %1 із %2 запитів за поточне вікно")
                             .arg(*state.used)
                             .arg(state.limit);
        if (state.remaining.has_value()) {
            detail += tr("; доступно %1").arg(*state.remaining);
        }
        if (state.resetIn > 0) {
            detail += tr("; оновлення через %1 с").arg(state.resetIn);
        }
        if (state.queued > 0) {
            detail += tr("; у черзі: %1 запитів").arg(state.queued);
        }
        label_->setToolTip(detail);
    }
}

}  // namespace mikai::app
