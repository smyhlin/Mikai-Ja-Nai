#include "widgets/FailureView.hpp"

#include "theme/Metrics.hpp"

#include <QAccessible>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>

namespace mikai::app {

FailureView::FailureView(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("failureView"));

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(theme::kSpacingSmall);

    message_ = new QLabel(this);
    message_->setObjectName(QStringLiteral("failureMessage"));
    message_->setWordWrap(true);
    layout->addWidget(message_, 1);

    // The chip is what makes the failure quotable. It is not translated: a code
    // a report can be matched against is the point of showing it at all.
    code_ = new QLabel(this);
    code_->setObjectName(QStringLiteral("failureCode"));
    code_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(code_, 0, Qt::AlignVCenter);

    retry_ = new QPushButton(tr("Retry"), this);
    retry_->setObjectName(QStringLiteral("textAction"));
    connect(retry_, &QPushButton::clicked, this, &FailureView::retryRequested);
    layout->addWidget(retry_, 0, Qt::AlignVCenter);

    clear();
}

void FailureView::showFailure(const backend::Failure& failure, const QString& context) {
    const QString line = failure.message.isEmpty() ? failure.code : failure.message;
    message_->setText(context.isEmpty() ? line : tr("%1: %2").arg(context, line));

    const QString code = backend::failureCodeLabel(failure);
    code_->setText(code);
    code_->setVisible(!code.isEmpty());

    show();
    // A failure the viewer may not be looking at still has to announce itself,
    // the same way a refused action does.
    QAccessibleEvent event(message_, QAccessible::Alert);
    QAccessible::updateAccessibility(&event);
}

void FailureView::setRetryVisible(bool visible) { retry_->setVisible(visible); }

void FailureView::clear() {
    message_->clear();
    code_->clear();
    code_->hide();
    hide();
}

QString FailureView::codeText() const { return code_->text(); }

QString FailureView::messageText() const { return message_->text(); }

}  // namespace mikai::app
