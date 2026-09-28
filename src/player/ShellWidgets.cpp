#include "ShellWidgets.hpp"

#include <QEnterEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPointF>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include <cmath>

#include "ShellSliders.hpp"

namespace mplayer {
namespace shell {
namespace {

/// `rgba(r,g,b,a)` for a colour, so a stylesheet fragment is built from the
/// same tokens the paint routines use.
QString rgba(const QColor& color) {
    return QStringLiteral("rgba(%1,%2,%3,%4)")
        .arg(color.red())
        .arg(color.green())
        .arg(color.blue())
        .arg(color.alpha());
}

/// Python's `%g`: up to six significant digits with trailing zeros stripped.
QString formatG(double value) {
    return QString::number(value, 'g', 6);
}

struct SpeedPreset {
    double speed;
    const char* label;
};

// SPEED_PRESETS — order matters, the selected row is picked by proximity.
const SpeedPreset kSpeedPresets[] = {
    {0.5, "Повільна"},        {1.0, "Звичайна"},
    {1.2, "Пришвидшена"},  {1.5, "Швидка"},
    {1.7, "Дуже швидка"},  {2.0, "Подвійна"},
};

inline constexpr int kHudRows = 7;

/// Label for the preset at `index`. Rows are appended in `kSpeedPresets` order,
/// so the index is the whole lookup — `refresh` runs on every slider-drag frame.
QString presetLabel(int index) {
    return QString::fromUtf8(kSpeedPresets[index].label);
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
//  PANEL
// ═══════════════════════════════════════════════════════════════════════════

Panel::Panel(const QColor& fill, double radius, const QColor& border, QWidget* parent)
    : QWidget(parent), fill_(fill), radius_(radius), border_(border) {
    setAttribute(Qt::WA_TranslucentBackground);
}

void Panel::setFill(const QColor& color) {
    fill_ = color;
    update();
}

void Panel::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    paintRounded(painter, QRectF(0.5, 0.5, width() - 1, height() - 1), radius_, fill_, border_);
}

// ═══════════════════════════════════════════════════════════════════════════
//  ICON BUTTON
// ═══════════════════════════════════════════════════════════════════════════

IconButton::IconButton(IconFn icon, const QString& tooltip, int size, int glyph, QWidget* parent,
                       const QColor& hoverFill)
    : QPushButton(parent),
      icon_(icon),
      glyph_(glyph),
      hoverFill_(hoverFill.isValid() ? hoverFill : white(28)) {
    setFixedSize(size, size);
    setToolTip(tooltip);
    if (!tooltip.isEmpty()) {
        setAccessibleName(tooltip);
    }
    setCursor(Qt::PointingHandCursor);
    setFlat(true);
}

void IconButton::setTint(const QColor& color) {
    tint_ = color;
    hasTint_ = color.isValid();
    update();
}

void IconButton::setIcon(IconFn icon) {
    icon_ = icon;
    update();
}

void IconButton::enterEvent(QEnterEvent* event) {
    hover_ = true;
    update();
    QPushButton::enterEvent(event);
}

void IconButton::leaveEvent(QEvent* event) {
    hover_ = false;
    update();
    QPushButton::leaveEvent(event);
}

void IconButton::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    if (hover_ || isChecked()) {
        paintRounded(painter, QRectF(1, 1, width() - 2, height() - 2), 7.0, hoverFill_, QColor());
    }
    const QColor color = hasTint_ ? tint_ : w90();
    const qreal off = (width() - glyph_) / 2.0;
    if (icon_) {
        icon_(painter, off, off, glyph_, color);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  SETTINGS FLYOUT
// ═══════════════════════════════════════════════════════════════════════════

SettingsFlyout::SettingsFlyout(QWidget* parent)
    : QFrame(parent, Qt::Popup | Qt::FramelessWindowHint), quality_(QStringLiteral("HD")) {
    setAttribute(Qt::WA_TranslucentBackground);
    setStyleSheet(
        QStringLiteral("SettingsFlyout { background-color: %1; border: 1px solid %2;"
                       " border-radius: 9px; }")
            .arg(rgba(flyout()), rgba(white(38))));

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(10, 10, 10, 10);
    lay->setSpacing(5);

    auto* head = new QHBoxLayout();
    auto* cap = new QLabel(QStringLiteral("ШВИДКІСТЬ"));
    cap->setFont(monoFont(8));
    cap->setStyleSheet(QStringLiteral("color: %1;").arg(rgba(white(115))));
    head->addWidget(cap);
    head->addStretch();
    badge_ = new QLabel(QStringLiteral("1.0x"));
    badge_->setFont(monoFont(9));
    badge_->setStyleSheet(
        QStringLiteral("background:%1; color:#ffffff; border-radius:4px; padding:1px 6px;"
                       " font-weight:700;")
            .arg(accent().name()));
    head->addWidget(badge_);
    lay->addLayout(head);

    slider_ = new RangeSlider(accent(), 4, this);
    connect(slider_, &RangeSlider::valueChanged, this, &SettingsFlyout::onSlider);
    lay->addWidget(slider_);

    auto* sep = new QFrame(this);
    sep->setFixedHeight(1);
    sep->setStyleSheet(QStringLiteral("background: %1;").arg(rgba(white(25))));
    lay->addWidget(sep);

    for (const SpeedPreset& preset : kSpeedPresets) {
        auto* btn = new QPushButton(this);
        btn->setFont(uiFont(10));
        btn->setCursor(Qt::PointingHandCursor);
        btn->setFixedHeight(26);
        const double speed = preset.speed;
        connect(btn, &QPushButton::clicked, this, [this, speed] { pick(speed); });
        lay->addWidget(btn);
        rows_.append(qMakePair(speed, btn));
    }

    auto* sep2 = new QFrame(this);
    sep2->setFixedHeight(1);
    sep2->setStyleSheet(QStringLiteral("background: %1;").arg(rgba(white(25))));
    lay->addWidget(sep2);

    auto* qrow = new QHBoxLayout();
    auto* ql = new QLabel(QStringLiteral("ЯКІСТЬ"));
    ql->setFont(monoFont(8));
    ql->setStyleSheet(QStringLiteral("color: %1;").arg(rgba(white(100))));
    qrow->addWidget(ql);
    qrow->addStretch();
    qualityLabel_ = new QLabel(QStringLiteral("HD"));
    qualityLabel_->setFont(monoFont(9));
    qualityLabel_->setStyleSheet(
        QStringLiteral("color:%1; font-weight:700;").arg(green().name()));
    qrow->addWidget(qualityLabel_);
    lay->addLayout(qrow);

    refresh();
}

void SettingsFlyout::setRate(double rate) {
    rate_ = qBound(0.25, rate, 2.0);
    refresh();
}

void SettingsFlyout::setQuality(const QString& quality) {
    quality_ = quality;
    refresh();
}

void SettingsFlyout::popupNear(const QPoint& anchorGlobal) {
    refresh();
    setFixedSize(sizeHint());
    const int x = anchorGlobal.x() + 14 - width();
    const int y = anchorGlobal.y() - height() - 10;
    move(qMax(4, x), qMax(4, y));
    show();
    raise();
}

void SettingsFlyout::pick(double speed) {
    rate_ = speed;
    refresh();
    emit speedSelected(speed);
    close();
}

void SettingsFlyout::onSlider(double fraction) {
    rate_ = 0.25 + fraction * 1.75;
    refresh();
    emit speedSelected(std::round(rate_ * 100.0) / 100.0);
}

void SettingsFlyout::refresh() {
    badge_->setText(formatG(rate_) + QStringLiteral("x"));
    qualityLabel_->setText(quality_);
    slider_->setValueSilent((rate_ - 0.25) / 1.75);
    for (int i = 0; i < rows_.size(); ++i) {
        const double speed = rows_[i].first;
        QPushButton* btn = rows_[i].second;
        const bool selected = std::abs(rate_ - speed) < 0.026;
        const QString label =
            QStringLiteral("%1x   %2").arg(formatG(speed), presetLabel(i));
        btn->setText((selected ? QStringLiteral("✓  ") : QString()) + label);
        btn->setStyleSheet(
            QStringLiteral("QPushButton { color: %1; background: %2; border: none;"
                           " border-radius: 5px; text-align: left; padding: 0 8px; }"
                           "QPushButton:hover { background: %3; }")
                .arg(selected ? QStringLiteral("#ffffff") : rgba(white(175)),
                     selected ? rgba(white(30)) : QStringLiteral("transparent"), rgba(white(22))));
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  STATE BADGE
// ═══════════════════════════════════════════════════════════════════════════

StateBadge::StateBadge(QWidget* parent)
    : QWidget(parent),
      state_(QStringLiteral("DEFAULT_FIT")),
      font_(monoFont(8)) {
    setMinimumHeight(22);
    resizeToText();
}

void StateBadge::setState(const QString& state) {
    state_ = state;
    resizeToText();
    update();
}

void StateBadge::resizeToText() {
    setFixedSize(textWidth(font_, state_) + 34, 22);
}

void StateBadge::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const bool fullscreen = state_.startsWith(QStringLiteral("FULLSCREEN_ACTIVE"));
    paintRounded(painter, QRectF(0.5, 0.5, width() - 1, height() - 1), 11.0,
                 fullscreen ? accentSoft() : blackSoft(), w15());
    painter.setPen(Qt::NoPen);
    painter.setBrush(green());
    painter.drawEllipse(QPointF(11, height() / 2.0), 3, 3);
    painter.setFont(font_);
    painter.setPen(w70());
    painter.drawText(QRectF(18, 0, width() - 18, height()), Qt::AlignVCenter, state_);
}

// ═══════════════════════════════════════════════════════════════════════════
//  STATE HUD
// ═══════════════════════════════════════════════════════════════════════════

StateHUD::StateHUD(QWidget* parent) : Panel(blackSoft(), 9.0, w15(), parent) {
    setAttribute(Qt::WA_TransparentForMouseEvents);

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(11, 9, 11, 9);
    lay->setSpacing(1);

    auto* head = new QHBoxLayout();
    auto* h1 = new QLabel(QStringLiteral("state"));
    h1->setFont(monoFont(8));
    h1->setStyleSheet(QStringLiteral("color:#7289ff;"));
    head->addWidget(h1);
    head->addStretch();
    auto* h2 = new QLabel(QStringLiteral("live"));
    h2->setFont(monoFont(8));
    h2->setStyleSheet(QStringLiteral("color:%1;").arg(green().name()));
    head->addWidget(h2);
    lay->addLayout(head);

    title_ = new QLabel(QStringLiteral("DEFAULT_FIT"));
    title_->setFont(monoFont(9));
    title_->setStyleSheet(QStringLiteral("color:#7289ff;"));
    lay->addWidget(title_);
    lay->addSpacing(4);

    for (int i = 0; i < kHudRows; ++i) {
        auto* k = new QLabel(QString());
        k->setFont(monoFont(8));
        k->setStyleSheet(QStringLiteral("color: %1;").arg(rgba(white(95))));
        auto* v = new QLabel(QString());
        v->setFont(monoFont(8));
        v->setStyleSheet(QStringLiteral("color: %1;").arg(rgba(white(220))));
        auto* row = new QHBoxLayout();
        row->setSpacing(6);
        row->addWidget(k, 0);
        row->addStretch();
        row->addWidget(v, 0);
        lay->addLayout(row);
        rows_.append(qMakePair(k, v));
    }

    auto* foot = new QLabel(QStringLiteral("D — hide · manifest v2.0.0"));
    foot->setFont(monoFont(7));
    foot->setStyleSheet(QStringLiteral("color: %1;").arg(rgba(white(70))));
    lay->addSpacing(3);
    lay->addWidget(foot);

    setFixedWidth(212);
    adjustSize();
}

void StateHUD::updateData(const QString& state, const QVector<QPair<QString, QString>>& data) {
    title_->setText(state);
    for (int i = 0; i < rows_.size(); ++i) {
        if (i < data.size()) {
            rows_[i].first->setText(data.at(i).first);
            rows_[i].second->setText(data.at(i).second);
        } else {
            rows_[i].first->clear();
            rows_[i].second->clear();
        }
    }
    adjustSize();
}

// ═══════════════════════════════════════════════════════════════════════════
//  CENTER GLYPH
// ═══════════════════════════════════════════════════════════════════════════

CenterGlyph::CenterGlyph(QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setFixedSize(74, 74);
    hide();
}

void CenterGlyph::setKind(Kind kind) {
    kind_ = kind;
    if (kind == Kind::None) {
        hide();
        return;
    }
    show();
    raise();
    update();
}

void CenterGlyph::paintEvent(QPaintEvent*) {
    if (kind_ == Kind::None) {
        return;
    }
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    paintRounded(painter, QRectF(0.5, 0.5, 73, 73), 37.0, QColor(0, 0, 0, 130), w25());
    const qreal off = 74 / 2.0 - 22 / 2.0;
    if (kind_ == Kind::Play) {
        IconPainter::play(painter, off + 3, off, 22, w95());
    } else {
        IconPainter::pause(painter, off, off, 22, w95());
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  PILL BUTTON
// ═══════════════════════════════════════════════════════════════════════════

PillButton::PillButton(QWidget* parent)
    : QWidget(parent), text_(QStringLiteral("Original File")), font_(uiFont(10)) {
    setCursor(Qt::PointingHandCursor);
    setFixedSize(textWidth(font_, text_) + 26, 22);
}

void PillButton::enterEvent(QEnterEvent* event) {
    hover_ = true;
    update();
    QWidget::enterEvent(event);
}

void PillButton::leaveEvent(QEvent* event) {
    hover_ = false;
    update();
    QWidget::leaveEvent(event);
}

void PillButton::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        emit clicked();
    }
}

void PillButton::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QColor color = hover_ ? w95() : w70();
    IconPainter::folder(painter, 1, 4, 14, color);
    painter.setFont(font_);
    painter.setPen(color);
    painter.drawText(QRectF(19, 0, width() - 19, height()), Qt::AlignVCenter, text_);
}

// ═══════════════════════════════════════════════════════════════════════════
//  TOAST
// ═══════════════════════════════════════════════════════════════════════════

Toast::Toast(QWidget* parent) : Panel(QColor(0, 0, 0, 205), 8.0, w15(), parent) {
    setAttribute(Qt::WA_TransparentForMouseEvents);

    label_ = new QLabel(this);
    label_->setFont(uiFont(10));
    label_->setStyleSheet(QStringLiteral("color: %1;").arg(rgba(white(235))));
    label_->setAlignment(Qt::AlignCenter);
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(14, 8, 14, 8);
    lay->addWidget(label_);
    hide();

    timer_ = new QTimer(this);
    timer_->setSingleShot(true);
    connect(timer_, &QTimer::timeout, this, &QWidget::hide);
}

void Toast::flash(const QString& text, int ms) {
    label_->setText(text);
    adjustSize();
    if (QWidget* parent = parentWidget()) {
        move((parent->width() - width()) / 2, static_cast<int>(parent->height() * 0.80));
    }
    show();
    raise();
    timer_->start(ms);
}

// ═══════════════════════════════════════════════════════════════════════════
//  KEY HINTS
// ═══════════════════════════════════════════════════════════════════════════

QVector<QPair<QString, QString>> KeyHints::shortcutTable() {
    return {
        {QStringLiteral("Space"), QStringLiteral("toggle_playback")},
        {QStringLiteral("F"), QStringLiteral("toggle_fullscreen_mode")},
        {QStringLiteral("M"), QStringLiteral("toggle_mute")},
        {QStringLiteral("Esc"), QStringLiteral("exit_viewer / reset_zoom")},
        {QStringLiteral("←  →"), QStringLiteral("seek ∓5s / prev·next")},
        {QStringLiteral("↑  ↓"), QStringLiteral("volume ±10 %")},
        {QStringLiteral("+  −"), QStringLiteral("zoom_in / zoom_out")},
        {QStringLiteral("R  0"), QStringLiteral("reset_canvas_transform")},
        {QStringLiteral("D"), QStringLiteral("toggle_state_manifest_hud")},
        {QStringLiteral("?"), QStringLiteral("this panel")},
    };
}

KeyHints::KeyHints(QWidget* parent) : QFrame(parent, Qt::Popup | Qt::FramelessWindowHint) {
    setAttribute(Qt::WA_TranslucentBackground);
    setStyleSheet(
        QStringLiteral("KeyHints { background-color: %1; border: 1px solid %2;"
                       " border-radius: 9px; }")
            .arg(rgba(flyout()), rgba(white(38))));

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(11, 10, 11, 10);
    lay->setSpacing(3);
    auto* cap = new QLabel(QStringLiteral("supported_input_modalities"));
    cap->setFont(monoFont(8));
    cap->setStyleSheet(QStringLiteral("color: %1;").arg(rgba(white(115))));
    lay->addWidget(cap);
    lay->addSpacing(2);

    const QVector<QPair<QString, QString>> table = shortcutTable();
    for (const auto& entry : table) {
        auto* row = new QHBoxLayout();
        row->setSpacing(8);
        auto* key = new QLabel(entry.first);
        key->setFont(monoFont(8));
        key->setAlignment(Qt::AlignCenter);
        key->setFixedWidth(58);
        key->setStyleSheet(
            QStringLiteral("color: %1; background: rgba(0,0,0,90); border: 1px solid %2;"
                           " border-radius: 4px; padding: 2px 0;")
                .arg(rgba(white(225)), rgba(white(30))));
        auto* action = new QLabel(entry.second);
        action->setFont(monoFont(8));
        action->setStyleSheet(QStringLiteral("color: %1;").arg(rgba(white(140))));
        row->addWidget(key);
        row->addWidget(action);
        lay->addLayout(row);
    }
}

void KeyHints::popupNear(const QPoint& anchorGlobal) {
    setFixedSize(sizeHint());
    move(qMax(4, anchorGlobal.x()), qMax(4, anchorGlobal.y() + 18));
    show();
    raise();
}

}  // namespace shell
}  // namespace mplayer
