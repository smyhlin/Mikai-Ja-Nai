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

void IconButton::setFilled(bool filled) {
    filled_ = filled;
    update();
}

void IconButton::setCircular(bool circular) {
    circular_ = circular;
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
    if (filled_ || hover_ || isChecked()) {
        const QRectF bounds(1, 1, width() - 2, height() - 2);
        if (circular_) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(hoverFill_);
            painter.drawEllipse(bounds);
        } else {
            paintRounded(painter, bounds, 10.0, hoverFill_, QColor());
        }
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
    : QFrame(parent), quality_(QStringLiteral("HD")) {
    setObjectName(QStringLiteral("playerSettingsFlyout"));
    setProperty("playerOverlay", true);
    setFocusPolicy(Qt::StrongFocus);
    setStyleSheet(QStringLiteral("SettingsFlyout { background: transparent; }"));
    hide();

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(10, 10, 10, 10);
    lay->setSpacing(5);

    auto* head = new QHBoxLayout();
    auto* cap = new QLabel(QStringLiteral("ШВИДКІСТЬ"));
    cap->setFont(monoFont(8));
    cap->setStyleSheet(QStringLiteral("color: #9a9aa6; font-weight: 700; background: transparent;"));
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
    sep->setStyleSheet(QStringLiteral("background: rgba(255, 255, 255, 0.10);"));
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
    sep2->setStyleSheet(QStringLiteral("background: rgba(255, 255, 255, 0.10);"));
    lay->addWidget(sep2);

    // Quality lives here rather than in a chip over the picture: the gear is
    // where a viewer looks for stream settings, and the strip is for things
    // that act on the current episode.
    qualitySection_ = new QWidget(this);
    qualitySection_->setStyleSheet(QStringLiteral("background: transparent;"));
    auto* qlay = new QVBoxLayout(qualitySection_);
    qlay->setContentsMargins(0, 0, 0, 0);
    qlay->setSpacing(5);

    auto* qrow = new QHBoxLayout();
    auto* ql = new QLabel(QStringLiteral("ЯКІСТЬ"));
    ql->setFont(monoFont(8));
    ql->setStyleSheet(QStringLiteral("color: #9a9aa6; font-weight: 700; background: transparent;"));
    qrow->addWidget(ql);
    qrow->addStretch();
    qualityLabel_ = new QLabel(QStringLiteral("HD"));
    qualityLabel_->setFont(monoFont(9));
    qualityLabel_->setStyleSheet(
        QStringLiteral("color:%1; font-weight:700;").arg(green().name()));
    qrow->addWidget(qualityLabel_);
    qlay->addLayout(qrow);

    qualityBody_ = new QVBoxLayout();
    qualityBody_->setContentsMargins(0, 0, 0, 0);
    qualityBody_->setSpacing(2);
    qlay->addLayout(qualityBody_);
    lay->addWidget(qualitySection_);
    qualitySection_->setVisible(false);

    refresh();
}

// Qt reads the 4th component of rgba() as an integer 0..255, so the old
// rgba(255,255,255,30) painted a near-solid slab and swallowed the row's own
// text. The popovers already spell alpha as a fraction, which is why their rows
// read correctly; both row styles go through here now.
QString rowQss(bool current) {
    return QStringLiteral(
               "QPushButton { color: %1; background: %2; border: 1px solid %3; border-radius: 6px;"
               " text-align: left; padding: 0 10px; font-size: 13px; font-weight: 500; min-height: 26px; }"
               "QPushButton:hover { background: rgba(255, 255, 255, 0.12); color: #ffffff;"
               " border-color: rgba(255, 255, 255, 0.20); }")
        .arg(current ? QStringLiteral("#ffffff") : QStringLiteral("#e0e0e6"),
             current ? QStringLiteral("rgba(88, 101, 242, 0.40)")
                     : QStringLiteral("rgba(255, 255, 255, 0.05)"),
             current ? QStringLiteral("rgba(88, 101, 242, 0.60)")
                     : QStringLiteral("transparent"));
}

void SettingsFlyout::setRate(double rate) {
    rate_ = qBound(0.25, rate, 2.0);
    refresh();
}

void SettingsFlyout::setQuality(const QString& quality) {
    quality_ = quality;
    refresh();
}

void SettingsFlyout::setQualityOptions(const QVector<QualityOption>& options) {
    qualityOptions_ = options;
    while (qualityBody_->count() > 0) {
        QLayoutItem* item = qualityBody_->takeAt(0);
        if (QWidget* widget = item->widget()) {
            // takeAt already detached it from the layout; reparenting to null
            // on top of that makes a visible row its own top-level window.
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }
    qualityRows_.clear();

    for (int i = 0; i < qualityOptions_.size(); ++i) {
        const int height = qualityOptions_[i].height;
        auto* btn = new QPushButton(this);
        btn->setFont(uiFont(10));
        btn->setCursor(Qt::PointingHandCursor);
        btn->setFixedHeight(26);
        btn->setFocusPolicy(Qt::NoFocus);
        connect(btn, &QPushButton::clicked, this, [this, height] {
            close();
            emit qualitySelected(height);
        });
        qualityBody_->addWidget(btn);
        qualityRows_.append(btn);
    }
    qualitySection_->setVisible(!qualityOptions_.isEmpty());
    refresh();
}

void SettingsFlyout::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    // 100% solid, fully opaque dark card background with crisp border
    p.setPen(QPen(QColor(255, 255, 255, 45), 1));
    p.setBrush(QColor(20, 21, 27));
    p.drawRoundedRect(QRectF(0.5, 0.5, width() - 1, height() - 1), 10.0, 10.0);
}

void SettingsFlyout::popupNear(const QPoint& anchorGlobal) {
    show();
    refresh();
    ensurePolished();
    layout()->invalidate();
    layout()->activate();
    setFixedSize(layout()->sizeHint());
    layout()->activate();
    QWidget* host = parentWidget();
    if (host == nullptr) {
        return;
    }

    const QPoint anchor = host->mapFromGlobal(anchorGlobal);
    const QRect bounds = host->rect().adjusted(4, 4, -4, -4);
    const int x = qBound(bounds.left(), anchor.x() + 14 - width(),
                         bounds.right() - width() + 1);
    const int y = qBound(bounds.top(), anchor.y() - height() - 10,
                         bounds.bottom() - height() + 1);
    move(x, y);
    raise();
    setFocus(Qt::OtherFocusReason);
}

void SettingsFlyout::keyPressEvent(QKeyEvent* event) {
    // The overlay owns focus while open, so Esc lands here before the player.
    if (event->key() == Qt::Key_Escape) {
        close();
        event->accept();
        return;
    }
    QFrame::keyPressEvent(event);
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
    // refresh() runs on every slider tick, so a drag produces dozens of calls
    // in which only the badge and the slider move. Writing a QLabel/QPushButton
    // re-lays it out and invalidates it, and setStyleSheet re-parses the sheet
    // and re-polishes the button - so each row is written only when its text
    // or its selected state actually differs from what is on screen.
    const QString badgeText = formatG(rate_) + QStringLiteral("x");
    if (badge_->text() != badgeText) {
        badge_->setText(badgeText);
    }
    if (qualityLabel_->text() != quality_) {
        qualityLabel_->setText(quality_);
    }
    slider_->setValueSilent((rate_ - 0.25) / 1.75);

    for (int i = 0; i < qualityRows_.size() && i < qualityOptions_.size(); ++i) {
        const QualityOption& option = qualityOptions_[i];
        QPushButton* btn = qualityRows_[i];
        const QString text = (option.current ? QStringLiteral("✓  ") : QString()) + option.label;
        if (btn->text() != text) {
            btn->setText(text);
        }
        const QString sheet = rowQss(option.current);
        if (btn->styleSheet() != sheet) {
            btn->setStyleSheet(sheet);
        }
    }
    for (int i = 0; i < rows_.size(); ++i) {
        const double speed = rows_[i].first;
        QPushButton* btn = rows_[i].second;
        const bool selected = std::abs(rate_ - speed) < 0.026;
        const QString text = (selected ? QStringLiteral("✓  ") : QString())
            + QStringLiteral("%1x   %2").arg(formatG(speed), presetLabel(i));
        if (btn->text() != text) {
            btn->setText(text);
        }
        const QString sheet = rowQss(selected);
        if (btn->styleSheet() != sheet) {
            btn->setStyleSheet(sheet);
        }
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

namespace {
/// The disc the glyph is painted into, and the halo margin the widget reserves
/// around it. One constant for both, because a disc larger than its widget is
/// clipped at the edges - which is what left only part of the circle showing.
constexpr qreal kCenterDisc = 78.0;
constexpr qreal kCenterHalo = 5.0;
constexpr qreal kCenterWidget = kCenterDisc + 2.0 * kCenterHalo;
}  // namespace

CenterGlyph::CenterGlyph(QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setFixedSize(static_cast<int>(kCenterWidget), static_cast<int>(kCenterWidget));
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

    const QRectF disc(kCenterHalo + 0.5, kCenterHalo + 0.5, kCenterDisc - 1.0, kCenterDisc - 1.0);
    // A soft outer halo, then the disc, then a hairline rim. One flat fill read
    // as a sticker pasted on the picture; the halo is what makes it sit above
    // a white frame without a hard black edge.
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 55));
    painter.drawEllipse(disc.adjusted(-kCenterHalo + 2.0, -kCenterHalo + 2.0, kCenterHalo - 2.0,
                                      kCenterHalo - 2.0));
    painter.setBrush(QColor(0, 0, 0, 145));
    painter.drawEllipse(disc);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QColor(255, 255, 255, 38), 1.2));
    painter.drawEllipse(disc);
    // A top-lit gradient inside the disc: lighter where the light would fall.
    QLinearGradient sheen(disc.topLeft(), disc.bottomLeft());
    sheen.setColorAt(0.0, QColor(255, 255, 255, 26));
    sheen.setColorAt(0.55, QColor(255, 255, 255, 0));
    painter.setPen(Qt::NoPen);
    painter.setBrush(sheen);
    painter.drawEllipse(disc);

    // Centred on the glyph's own bounds, not on the 24-grid cell: the triangle
    // spans 8..19.5 and the bars 7..17, so both sit a little right of centre in
    // the cell and need a per-shape correction to land optically centred.
    constexpr qreal kGlyph = 26.0;
    const qreal scale = kGlyph / 24.0;
    const qreal off = kCenterHalo + (kCenterDisc - kGlyph) / 2.0;
    if (kind_ == Kind::Play) {
        IconPainter::play(painter, off - 1.75 * scale, off, kGlyph, w95());
    } else {
        IconPainter::pause(painter, off, off, kGlyph, w95());
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
        {QStringLiteral("Пробіл"), QStringLiteral("Пауза / Відтворення")},
        {QStringLiteral("F"), QStringLiteral("Повний екран / вікно")},
        {QStringLiteral("M"), QStringLiteral("Вимкнути / увімкнути звук")},
        {QStringLiteral("S"), QStringLiteral("Список серій")},
        {QStringLiteral("T"), QStringLiteral("Вибір озвучення")},
        {QStringLiteral("C"), QStringLiteral("Вибір субтитрів")},
        {QStringLiteral("Q"), QStringLiteral("Якість та швидкість")},
        {QStringLiteral("←  →"), QStringLiteral("Перемотування ∓5с")},
        {QStringLiteral("↑  ↓"), QStringLiteral("Гучність ±10 %")},
        {QStringLiteral("Esc"), QStringLiteral("Закрити панель / вікно")},
    };
}

KeyHints::KeyHints(QWidget* parent) : QFrame(parent) {
    setObjectName(QStringLiteral("playerKeyHints"));
    setProperty("playerOverlay", true);
    setFocusPolicy(Qt::StrongFocus);
    setStyleSheet(QStringLiteral("KeyHints { background: transparent; }"));
    hide();

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(12, 10, 12, 10);
    lay->setSpacing(4);
    auto* cap = new QLabel(QStringLiteral("ГАРЯЧІ КЛАВІШІ"));
    cap->setFont(monoFont(8));
    cap->setStyleSheet(QStringLiteral("color: #9a9aa6; font-weight: 700; background: transparent; letter-spacing: 0.5px;"));
    lay->addWidget(cap);
    lay->addSpacing(2);

    const QVector<QPair<QString, QString>> table = shortcutTable();
    for (const auto& entry : table) {
        auto* row = new QHBoxLayout();
        row->setSpacing(8);
        auto* key = new QLabel(entry.first);
        key->setFont(monoFont(8));
        key->setAlignment(Qt::AlignCenter);
        key->setMinimumWidth(68);
        key->setStyleSheet(
            QStringLiteral("color: %1; background: rgba(0,0,0,120); border: 1px solid %2;"
                           " border-radius: 4px; padding: 2px 6px; font-weight: 600;")
                .arg(rgba(white(235)), rgba(white(40))));
        auto* action = new QLabel(entry.second);
        action->setFont(uiFont(9));
        action->setStyleSheet(QStringLiteral("color: %1; background: transparent;").arg(rgba(white(180))));
        row->addWidget(key);
        row->addWidget(action, 1);
        lay->addLayout(row);
    }
}

void KeyHints::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(QColor(255, 255, 255, 45), 1));
    p.setBrush(QColor(20, 21, 27));
    p.drawRoundedRect(QRectF(0.5, 0.5, width() - 1, height() - 1), 10.0, 10.0);
}

void KeyHints::popupNear(const QPoint& anchorGlobal) {
    show();
    ensurePolished();
    layout()->invalidate();
    layout()->activate();
    setFixedSize(layout()->sizeHint());
    layout()->activate();
    QWidget* host = parentWidget();
    if (host == nullptr) {
        return;
    }

    const QPoint anchor = host->mapFromGlobal(anchorGlobal);
    const QRect bounds = host->rect().adjusted(4, 4, -4, -4);
    const int x = qBound(bounds.left(), anchor.x() + 14 - width(),
                         bounds.right() - width() + 1);
    const int y = qBound(bounds.top(), anchor.y() + 8,
                         bounds.bottom() - height() + 1);
    move(x, y);
    raise();
    setFocus(Qt::OtherFocusReason);
}

void KeyHints::keyPressEvent(QKeyEvent* event) {
    // Same reason as SettingsFlyout: the focused overlay owns Esc.
    if (event->key() == Qt::Key_Escape) {
        close();
        event->accept();
        return;
    }
    QFrame::keyPressEvent(event);
}

}  // namespace shell
}  // namespace mplayer
