#include "widgets/RateLimitLabel.hpp"

#include "theme/Metrics.hpp"

#include <QEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QStyle>
#include <QTimer>

namespace mikai::app {
namespace {

/// Re-applies a property selector after a dynamic property changed; Qt does not
/// re-evaluate a widget's rules on its own.
void repolish(QWidget* widget) {
    if (widget == nullptr || widget->style() == nullptr) return;
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->update();
}

}  // namespace

RateLimitLabel::RateLimitLabel(backend::MikaiClient& client, QWidget* parent)
    : QWidget(parent), client_(client) {
    label_ = new QLabel(this);
    label_->setObjectName(QStringLiteral("muted"));
    label_->installEventFilter(this);
    // A shortcut, not decoration: the chip opens the page that owns the key the
    // quota belongs to, so it advertises that with the pointer and the keyboard.
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);
    setAccessibleDescription(tr("Open the API access settings"));

    layout_ = new QHBoxLayout(this);
    layout_->setContentsMargins(0, 0, 0, 0);
    layout_->addWidget(label_, 0, Qt::AlignVCenter);

    // The pill's status disc. It is created after the text label on purpose:
    // the shared surface's `findChild<QLabel*>()` must keep resolving to the
    // line it has always resolved to.
    dot_ = new QLabel(this);
    dot_->setObjectName(QStringLiteral("rateDot"));
    dot_->setAccessibleName(QString());
    dot_->setFocusPolicy(Qt::NoFocus);
    dot_->setAttribute(Qt::WA_TransparentForMouseEvents);
    dot_->setProperty("tone", QStringLiteral("unknown"));
    dot_->setFixedSize(theme::kSearchDotSize, theme::kSearchDotSize);
    dot_->hide();
    layout_->addWidget(dot_, 0, Qt::AlignVCenter);

    applyPresentationMargins();

    timer_ = new QTimer(this);
    timer_->setInterval(1000);
    connect(timer_, &QTimer::timeout, this, &RateLimitLabel::refresh);
    // T-412: the tick is armed by showEvent below rather than here, so an
    // indicator that is not on screen never burns a timer slot. The first
    // paint still lands immediately.
    refresh();
}

RateLimitLabel::~RateLimitLabel() = default;

void RateLimitLabel::setPresentation(Presentation presentation) {
    if (presentation_ == presentation) return;
    presentation_ = presentation;
    applyPresentationMargins();
    // The pill's surface, hairline and radius are the sheet's work, and a plain
    // QWidget has to ask the style to paint its background. Inline draws no
    // surface, so it must not ask.
    setAttribute(Qt::WA_StyledBackground, presentation_ == Presentation::Pill);
    repolish(this);
    if (dot_ != nullptr) {
        dot_->setVisible(presentation_ == Presentation::Pill);
    }
    if (presentation_ == Presentation::Inline) {
        setToolTip(QString());
    }
    // Restated from the snapshot already rendered: switching presentation never
    // asks the server for anything, and the quota facts stay where they were.
    renderedText_.clear();
    renderedDetail_.clear();
    renderState(state_);
}

void RateLimitLabel::applyPresentationMargins() {
    if (layout_ == nullptr) return;
    if (presentation_ == Presentation::Pill) {
        layout_->setContentsMargins(theme::kSearchPillPaddingLeft, 0,
                                    theme::kSearchPillPaddingRight, 0);
        layout_->setSpacing(theme::kSearchDotSpacing);
    } else {
        layout_->setContentsMargins(0, 0, 0, 0);
        layout_->setSpacing(0);
    }
}

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

bool RateLimitLabel::eventFilter(QObject* watched, QEvent* event) {
    if (watched == label_ && event->type() == QEvent::FontChange &&
        presentation_ == Presentation::Pill) {
        applyTabularNumerals();
    }
    return QWidget::eventFilter(watched, event);
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

QString RateLimitLabel::dotTone(const backend::RateLimitState& state) const {
    // No answer with quota headers is not health: the dot stays neutral even
    // though the client has a default limit to report.
    if (!state.used.has_value() || state.limit <= 0) {
        return QStringLiteral("unknown");
    }
    const int used = *state.used;
    const int remaining = state.remaining.value_or(qMax(0, state.limit - used));
    if (remaining <= 0 || used >= state.limit) {
        return QStringLiteral("exhausted");
    }
    if (remaining <= qMax(1, state.limit / 10)) {
        return QStringLiteral("low");
    }
    return QStringLiteral("healthy");
}

void RateLimitLabel::applyDotTone(const QString& tone) {
    if (dot_ == nullptr || presentation_ != Presentation::Pill) return;
    if (dot_->property("tone").toString() == tone) return;
    dot_->setProperty("tone", tone);
    repolish(dot_);
}

void RateLimitLabel::applyTabularNumerals() {
    if (label_ == nullptr) return;
    QFont font = label_->font();
    if (font.featureValue(QFont::Tag("tnum")) == 1) return;
    font.setFeature(QFont::Tag("tnum"), 1);
    font.setStyleStrategy(QFont::PreferAntialias);
    label_->setFont(font);
}

void RateLimitLabel::renderState(const backend::RateLimitState& state) {
    QString text;
    QString detail;
    const bool hasWindow = state.used.has_value() && state.limit > 0;
    if (!hasWindow) {
        const QString limit =
            state.limit > 0 ? QString::number(state.limit) : QStringLiteral("—");
        text = presentation_ == Presentation::Pill
                   ? QStringLiteral("RPM: — / %1").arg(limit)
                   : QStringLiteral("RPM —/%1").arg(limit);
        detail = tr("No response with rate limit headers yet");
    } else {
        const int used = *state.used;
        text = presentation_ == Presentation::Pill
                   ? QStringLiteral("RPM: %1 / %2").arg(used).arg(state.limit)
                   : QStringLiteral("RPM %1/%2").arg(used).arg(state.limit);
        detail = tr("Last API response: %1 of %2 requests used in the current window")
                     .arg(used)
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
    // The dot is state, not wording, so it is kept current even when the line
    // itself did not move; a stylesheet re-polish can also strip the font
    // feature, so the pill restores it on every pass.
    applyDotTone(dotTone(state));
    if (presentation_ == Presentation::Pill) {
        applyTabularNumerals();
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
    if (presentation_ == Presentation::Pill) {
        // The dot is inside the pill too, so the whole chip explains itself.
        setToolTip(detail);
    }
}

void RateLimitLabel::mouseReleaseEvent(QMouseEvent* event) {
    QWidget::mouseReleaseEvent(event);
    if (event->button() == Qt::LeftButton && rect().contains(event->pos())) {
        Q_EMIT clicked();
    }
}

void RateLimitLabel::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter ||
        event->key() == Qt::Key_Space) {
        Q_EMIT clicked();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

}  // namespace mikai::app
