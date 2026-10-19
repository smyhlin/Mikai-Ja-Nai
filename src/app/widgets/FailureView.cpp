#include "widgets/FailureView.hpp"

#include "support/I18n.hpp"
#include "theme/Metrics.hpp"

#include <QAccessible>
#include <QEvent>
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

void FailureView::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
}

void FailureView::retranslateUi() {
    retry_->setText(tr("Retry"));
    if (!hasFailure_) {
        return;
    }
    // Only a line the app itself worded from a known code is re-localized. A
    // backend line that happened to arrive under a known code, and the
    // caller's context clause, are not guessed at: they stay exactly as they
    // arrived, so a 522's origin detail is never replaced by generic wording.
    QString line;
    if (failure_.message.isEmpty()) {
        // Nothing to preserve, so the known code's wording is derived safely.
        const QString localized = localizedApiError(failure_.code);
        line = localized.isEmpty() ? failure_.code : localized;
    } else if (messageIsCanonical_) {
        const QString localized = localizedApiError(failure_.code);
        line = localized.isEmpty() ? failure_.message : localized;
    } else {
        line = failure_.message;
    }
    message_->setText(context_.isEmpty() ? line : tr("%1: %2").arg(context_, line));

    const QString code = backend::failureCodeLabel(failure_);
    code_->setText(code);
    code_->setVisible(!code.isEmpty());
}

void FailureView::showFailure(const backend::Failure& failure, const QString& context) {
    failure_ = failure;
    context_ = context;
    // Decide here, while the language that produced `failure.message` is still
    // active, whether the app owns that line. Runner fills the message from
    // localizedApiError for every known code, but a caller may forward the
    // server's own detail under the same code; only the former may be re-worded
    // on a later language switch.
    const QString canonical = localizedApiError(failure.code);
    messageIsCanonical_ =
        !failure.message.isEmpty() && !canonical.isEmpty() && failure.message == canonical;
    hasFailure_ = true;
    retranslateUi();

    show();
    // A failure the viewer may not be looking at still has to announce itself,
    // the same way a refused action does.
    QAccessibleEvent event(message_, QAccessible::Alert);
    QAccessible::updateAccessibility(&event);
}

void FailureView::setContext(const QString& context) {
    context_ = context;
    retranslateUi();
}
void FailureView::setRetryVisible(bool visible) { retry_->setVisible(visible); }

void FailureView::clear() {
    failure_ = {};
    context_.clear();
    hasFailure_ = false;
    messageIsCanonical_ = false;
    message_->clear();
    code_->clear();
    code_->hide();
    hide();
}

QString FailureView::codeText() const { return code_->text(); }

QString FailureView::messageText() const { return message_->text(); }

}  // namespace mikai::app
