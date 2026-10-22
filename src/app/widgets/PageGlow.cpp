#include "widgets/PageGlow.hpp"

#include "state/AppearanceStore.hpp"
#include "theme/Appearance.hpp"
#include "theme/Palette.hpp"

#include <QEvent>
#include <QLinearGradient>
#include <QPainter>

namespace mikai::app {

PageGlow::PageGlow(QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setFocusPolicy(Qt::NoFocus);
    if (parent != nullptr) {
        parent->installEventFilter(this);
    }
    connect(&appearanceNotifier(), &AppearanceNotifier::changed, this,
            [this](const theme::Appearance& previous) {
                const auto& appearance = currentAppearance();
                if (previous.backgroundGradient != appearance.backgroundGradient ||
                    previous.gradientIntensity != appearance.gradientIntensity ||
                    previous.gradientHeight != appearance.gradientHeight ||
                    previous.gradientColor != appearance.gradientColor) {
                    refresh();
                }
            });
    refresh();
}

void PageGlow::refresh() {
    QWidget* host = parentWidget();
    if (host == nullptr) return;
    const theme::Appearance& appearance = currentAppearance();
    const bool visible = appearance.backgroundGradient && appearance.gradientIntensity > 0;
    const int extent = visible
                           ? qBound(0, host->height() * appearance.gradientHeight / 100,
                                    host->height())
                           : 0;
    if (geometry() != QRect(0, 0, host->width(), extent)) {
        setGeometry(0, 0, host->width(), extent);
    }
    lower();
    update();
}

bool PageGlow::eventFilter(QObject* watched, QEvent* event) {
    if (watched == parentWidget() && event->type() == QEvent::Resize) {
        refresh();
    }
    return QWidget::eventFilter(watched, event);
}

void PageGlow::paintEvent(QPaintEvent*) {
    const theme::Appearance& appearance = currentAppearance();
    if (!appearance.backgroundGradient || appearance.gradientIntensity <= 0 || height() <= 0) {
        return;
    }
    const QColor accent = theme::gradientColor(appearance);
    // The wash is ambience, not a painted panel: even at full intensity it
    // stays translucent, and the tail is long enough to leave no edge.
    QColor top = accent;
    top.setAlpha(qBound(0, appearance.gradientIntensity * 190 / 100, 170));
    QColor middle = accent;
    middle.setAlpha(top.alpha() * 2 / 5);
    QColor fainter = accent;
    fainter.setAlpha(top.alpha() / 8);
    QColor bottom = accent;
    bottom.setAlpha(0);

    QLinearGradient gradient(0, 0, 0, height());
    gradient.setColorAt(0.0, top);
    gradient.setColorAt(0.45, middle);
    gradient.setColorAt(0.75, fainter);
    gradient.setColorAt(1.0, bottom);

    QPainter painter(this);
    painter.fillRect(rect(), gradient);
}

}  // namespace mikai::app
