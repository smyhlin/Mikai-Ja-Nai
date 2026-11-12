#include <utility>
#include "ShellWidgets.hpp"

#include "ShellTheme.hpp"

#include <QCoreApplication>
#include <QEnterEvent>
#include <QEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointF>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

#include "ShellSliders.hpp"
#include "player/core/VideoSession.hpp"
#include "gui/SystemDefaultPlayer.hpp"
#include "gui/UiProfile.hpp"

namespace mplayer {
namespace shell {
namespace {

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
    {0.5, QT_TRANSLATE_NOOP("mplayer::shell::SettingsFlyout", "Slow")},
    {1.0, QT_TRANSLATE_NOOP("mplayer::shell::SettingsFlyout", "Normal")},
    {1.2, QT_TRANSLATE_NOOP("mplayer::shell::SettingsFlyout", "Slightly faster")},
    {1.5, QT_TRANSLATE_NOOP("mplayer::shell::SettingsFlyout", "Fast")},
    {1.7, QT_TRANSLATE_NOOP("mplayer::shell::SettingsFlyout", "Very fast")},
    {2.0, QT_TRANSLATE_NOOP("mplayer::shell::SettingsFlyout", "Double speed")},
};

/// Label for the preset at `index`. Rows are appended in `kSpeedPresets` order,
/// so the index is the whole lookup — `refresh` runs on every slider-drag frame.
QString presetLabel(int index) {
    return QCoreApplication::translate("mplayer::shell::SettingsFlyout",
                                       kSpeedPresets[index].label);
}

// ── skip pill metrics ──────────────────────────────────────────────────────
// The pill is measured by its text, so a longer translation grows it instead of
// being clipped. The height is fixed: a stack of offers reads as one column.
constexpr int kSkipPillHeight = 34;
constexpr int kSkipPillPadding = 14;
constexpr int kSkipPillGlyph = 13;
constexpr int kSkipPillGlyphGap = 7;
constexpr int kSkipPillMinText = 64;

/// paintRounded() with a brush instead of a flat colour, so an IconButton's
/// standing fill can be the gradient the float's play disc is drawn with.
void paintRoundedBrush(QPainter& painter, const QRectF& rect, double radius, const QBrush& fill,
                       const QColor& border, double borderWidth) {
    enableChromeAntialiasing(painter);
    painter.setPen(border.isValid() ? QPen(border, borderWidth) : QPen(Qt::NoPen));
    painter.setBrush(fill);
    painter.drawRoundedRect(rect, radius, radius);
}

/// Keeps a popover that re-measured itself inside its host without moving a
/// popover that still fits.
void clampInsideHost(QWidget* popover) {
    QWidget* host = popover->parentWidget();
    if (host == nullptr) {
        return;
    }
    const QRect bounds = host->rect().adjusted(4, 4, -4, -4);
    const QPoint at = popover->pos();
    popover->move(qBound(bounds.left(), at.x(), bounds.right() - popover->width() + 1),
                  qBound(bounds.top(), at.y(), bounds.bottom() - popover->height() + 1));
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
//  PAINTED CHROME
// ═══════════════════════════════════════════════════════════════════════════

void ChromePaint::paintOwnBackingStore(QWidget* widget) {
    if (surfacePainted_) {
        return;
    }
    QPainter painter(widget);
    painter.setRenderHint(QPainter::Antialiasing);
    paintChrome(painter);
}

ChromeHost* chromeHostOf(QWidget* widget) {
    for (QWidget* parent = (widget != nullptr) ? widget->parentWidget() : nullptr;
         parent != nullptr; parent = parent->parentWidget()) {
        if (auto* host = dynamic_cast<ChromeHost*>(parent)) {
            return host;
        }
    }
    return nullptr;
}

void chromeUpdate(QWidget* widget, const QRect& rect) {
    if (widget == nullptr) {
        return;
    }
    if (rect.isNull()) {
        widget->update();
    } else {
        widget->update(rect);
    }
    auto* painted = dynamic_cast<ChromePaint*>(widget);
    if (painted == nullptr || !painted->surfacePainted()) {
        return;
    }
    if (ChromeHost* host = chromeHostOf(widget)) {
        mikai::gui::uiprofile::noteChromeDirtyRequest();
        host->chromeDirty(widget, rect);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  PANEL
// ═══════════════════════════════════════════════════════════════════════════

Panel::Panel(const QColor& fill, double radius, const QColor& border, QWidget* parent)
    : QWidget(parent), fill_(fill), radius_(radius), border_(border) {
    setAttribute(Qt::WA_TranslucentBackground);
}

void Panel::setFill(const QColor& color) {
    if (fill_ == color) {
        return;
    }
    fill_ = color;
    chromeUpdate(this);
}

void Panel::paintChrome(QPainter& painter) {
    enableChromeAntialiasing(painter);
    paintRounded(painter, QRectF(0.5, 0.5, width() - 1, height() - 1), radius_, fill_, border_);
}

void Panel::paintEvent(QPaintEvent*) { paintOwnBackingStore(this); }

// ═══════════════════════════════════════════════════════════════════════════
//  ROW
// ═══════════════════════════════════════════════════════════════════════════

namespace {
/// One row's colours, read from the tokens at paint time: an accent change
/// must reach an already built row without rebuilding the list.
QColor rowInk(ChromeRow::Role role, bool hovered, bool highlighted) {
    if (role == ChromeRow::Role::Disabled) return QColor(0x9a, 0x9a, 0xa6);
    if (highlighted || hovered) return QColor(0xff, 0xff, 0xff);
    return role == ChromeRow::Role::Current ? QColor(0xff, 0xff, 0xff)
                                            : QColor(0xe0, 0xe0, 0xe6);
}

QColor rowFill(ChromeRow::Role role, bool hovered, bool highlighted) {
    if (role == ChromeRow::Role::Disabled) return QColor(255, 255, 255, 8);
    if (hovered || highlighted) return QColor(255, 255, 255, 31);
    if (role == ChromeRow::Role::Current) {
        QColor wash = shell::accent();
        wash.setAlphaF(0.40);
        return wash;
    }
    return QColor(255, 255, 255, 13);
}
}  // namespace

ChromeRow::ChromeRow(const QString& text, QWidget* parent) : QPushButton(text, parent) {
    setAttribute(Qt::WA_StyledBackground, false);
    setFocusPolicy(Qt::NoFocus);
    setCursor(Qt::PointingHandCursor);
    setMinimumHeight(28);
    QFont rowFont = font();
    rowFont.setPixelSize(13);
    rowFont.setWeight(QFont::Medium);
    setFont(rowFont);
}

void ChromeRow::setRole(Role role) {
    if (role_ == role) return;
    role_ = role;
    chromeUpdate(this);
}

void ChromeRow::setHighlighted(bool highlighted) {
    if (highlighted_ == highlighted) return;
    highlighted_ = highlighted;
    chromeUpdate(this);
}

void ChromeRow::setSubmenu(bool submenu) {
    if (submenu_ == submenu) return;
    submenu_ = submenu;
    chromeUpdate(this);
}

void ChromeRow::setRowPadding(int horizontal) {
    if (padX_ == horizontal) return;
    padX_ = horizontal;
    chromeUpdate(this);
}

void ChromeRow::paintChrome(QPainter& painter) {
    enableChromeAntialiasing(painter);
    const Role role = isEnabled() ? role_ : Role::Disabled;
    // The pointer state belongs to this widget, and a surface paint is exactly
    // the repaint a hover would have caused on its own backing store.
    const bool hovered = underMouse() && role != Role::Disabled;
    const bool highlighted = highlighted_ && role != Role::Disabled;
    const QRectF box = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    painter.setPen(QPen(hovered ? QColor(255, 255, 255, 51) : QColor(0, 0, 0, 0), 1.0));
    painter.setBrush(rowFill(role, hovered, highlighted));
    painter.drawRoundedRect(box, 6.0, 6.0);
    const QColor ink = rowInk(role, hovered, highlighted);
    painter.setFont(font());
    painter.setPen(ink);
    const int chevron = (submenu_ && role != Role::Disabled) ? 14 : 0;
    const QRect textBox = rect().adjusted(padX_, 0, -padX_ - chevron, 0);
    painter.drawText(textBox, Qt::AlignLeft | Qt::AlignVCenter,
                     fontMetrics().elidedText(text(), Qt::ElideRight, textBox.width()));
    if (chevron == 0) return;
    // The classic menu chevron: a small stroke, not a glyph, so it carries the
    // row's own ink.
    const QPointF tip(box.right() - padX_ + 1.0, box.center().y() + 0.5);
    QPainterPath path;
    path.moveTo(tip.x() - 5.0, tip.y() - 3.0);
    path.lineTo(tip.x(), tip.y() + 1.0);
    path.lineTo(tip.x() - 5.0, tip.y() + 5.0);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(ink, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.drawPath(path);
}

void ChromeRow::paintEvent(QPaintEvent*) { paintOwnBackingStore(this); }

void ChromeRow::enterEvent(QEnterEvent* event) {
    QPushButton::enterEvent(event);
    chromeUpdate(this);
}

void ChromeRow::leaveEvent(QEvent* event) {
    QPushButton::leaveEvent(event);
    chromeUpdate(this);
}

void ChromeRow::mousePressEvent(QMouseEvent* event) {
    QPushButton::mousePressEvent(event);
    chromeUpdate(this);
}

void ChromeRow::mouseReleaseEvent(QMouseEvent* event) {
    QPushButton::mouseReleaseEvent(event);
    chromeUpdate(this);
}

void ChromeRow::changeEvent(QEvent* event) {
    QPushButton::changeEvent(event);
    switch (event->type()) {
        case QEvent::EnabledChange:
        case QEvent::FontChange:
        case QEvent::PaletteChange:
        case QEvent::StyleChange:
            chromeUpdate(this);
            break;
        default:
            break;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  ICON BUTTON
// ═══════════════════════════════════════════════════════════════════════════

IconButton::IconButton(IconFn icon, const QString& tooltip, int size, int glyph, QWidget* parent,
                       const QColor& hoverFill)
    : QPushButton(parent), icon_(icon), glyph_(glyph) {
    chrome_.hoverFill = hoverFill.isValid() ? QBrush(hoverFill) : QBrush(white(28));
    setFixedSize(size, size);
    setToolTip(tooltip);
    if (!tooltip.isEmpty()) {
        setAccessibleName(tooltip);
    }
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::NoFocus);
    setFlat(true);
}

void IconButton::setTint(const QColor& color) {
    const bool valid = color.isValid();
    if (tint_ == color && hasTint_ == valid) {
        return;
    }
    tint_ = color;
    hasTint_ = valid;
    chromeUpdate(this);
}

void IconButton::setIcon(IconFn icon) {
    if (icon_ == icon) {
        return;
    }
    icon_ = icon;
    chromeUpdate(this);
}

void IconButton::setButtonChrome(const ButtonChrome& chrome) {
    chrome_ = chrome;
    chromeUpdate(this);
}

void IconButton::enterEvent(QEnterEvent* event) {
    // The look is a pure function of hover_, isChecked() and isEnabled(), none of
    // which this event changed if the button was already hovered. A repeated
    // entry - a pointer sweeping back over a control whose leave never arrived, a
    // re-enter after a popup - asked for pixels that are already on screen, and a
    // cache miss for them costs a full re-raster of the control.
    if (!hover_) {
        hover_ = true;
        chromeUpdate(this);
    }
    QPushButton::enterEvent(event);
}

void IconButton::leaveEvent(QEvent* event) {
    if (hover_) {
        hover_ = false;
        chromeUpdate(this);
    }
    QPushButton::leaveEvent(event);
}

void IconButton::paintChrome(QPainter& painter) {
    enableChromeAntialiasing(painter);
    // Inset by the stroke width, so a bordered button's outline lands inside
    // its own rect rather than being clipped by it.
    const QRectF box(0.5, 0.5, width() - 1.0, height() - 1.0);
    const bool lit = hover_ || isChecked();
    const double radius = chrome_.useThemeRadius ? cornerRadius() : chrome_.radius;
    if (chrome_.fill.style() != Qt::NoBrush) {
        paintRoundedBrush(painter, box, radius, chrome_.fill, chrome_.border,
                          chrome_.borderWidth);
    }
    if (lit && chrome_.hoverFill.style() != Qt::NoBrush) {
        paintRoundedBrush(painter, box, radius, chrome_.hoverFill, chrome_.hoverBorder,
                          chrome_.borderWidth);
    }
    QColor color = hasTint_ ? tint_ : w90();
    if (!isEnabled()) color.setAlpha(color.alpha() / 3);
    // Integer positioning for crisp rendering - align to pixel grid
    const int off = (width() - glyph_) / 2;
    if (icon_) {
        icon_(painter, off, off, glyph_, color);
    }
}

void IconButton::paintEvent(QPaintEvent*) { paintOwnBackingStore(this); }

// ═══════════════════════════════════════════════════════════════════════════
//  SKIP BUTTON
// ═══════════════════════════════════════════════════════════════════════════

SkipButton::SkipButton(QWidget* parent) : QPushButton(parent) {
    // Text is painted by paintChrome, never by QPushButton's own style, so the
    // control is flat and carries no stylesheet of its own.
    setFlat(true);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);
    setFont(uiFont(9, true));
    setFixedHeight(kSkipPillHeight);
}

void SkipButton::setLabel(const QString& label) {
    if (label_ == label) {
        return;
    }
    label_ = label;
    // The base class keeps the string too, so a screen reader that reads the
    // button's text and one that reads its accessible name agree.
    QPushButton::setText(label);
    resize(sizeHint());
    chromeUpdate(this);
}

QSize SkipButton::sizeHint() const {
    const QFontMetrics metrics(font());
    const int text = std::max(kSkipPillMinText, metrics.horizontalAdvance(label_));
    return {text + 2 * kSkipPillPadding + kSkipPillGlyphGap + kSkipPillGlyph, kSkipPillHeight};
}

void SkipButton::enterEvent(QEnterEvent* event) {
    // Same rule as IconButton: only a real hover change is a repaint. The pill,
    // its text colour and its chevron all read hover_ and nothing else that this
    // event touched.
    if (!hover_) {
        hover_ = true;
        chromeUpdate(this);
    }
    QPushButton::enterEvent(event);
}

void SkipButton::leaveEvent(QEvent* event) {
    if (hover_) {
        hover_ = false;
        chromeUpdate(this);
    }
    QPushButton::leaveEvent(event);
}

void SkipButton::paintChrome(QPainter& painter) {
    enableChromeAntialiasing(painter);
    // Inset by the stroke, so the outline lands inside the button's own rect.
    const QRectF box(0.5, 0.5, width() - 1.0, height() - 1.0);
    const double radius = std::min(cornerRadius() + 6.0, height() / 2.0);
    paintRounded(painter, box, radius, hover_ ? QColor(48, 51, 58, 240) : pill(),
                 hover_ ? w25() : w15());

    const int glyphX = width() - kSkipPillPadding - kSkipPillGlyph;
    const int textWidth = std::max(0, glyphX - kSkipPillGlyphGap - kSkipPillPadding);
    painter.setFont(font());
    painter.setPen(isEnabled() ? w95() : w45());
    painter.drawText(QRect(kSkipPillPadding, 0, textWidth, height()),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     QFontMetrics(font()).elidedText(label_, Qt::ElideRight, textWidth));
    // Integer placement, so the chevron stays crisp at any scale factor.
    IconPainter::chevronRight(painter, glyphX, (height() - kSkipPillGlyph) / 2, kSkipPillGlyph,
                              hover_ ? accent() : w70());
}

void SkipButton::paintEvent(QPaintEvent*) { paintOwnBackingStore(this); }

// ═══════════════════════════════════════════════════════════════════════════
//  SETTINGS FLYOUT
// ═══════════════════════════════════════════════════════════════════════════

namespace {
/// The opaque card both popovers are: the fill and hairline the stylesheet used
/// to give them, painted by the card's own painter instead.
void paintPopoverCard(QPainter& painter, const QWidget& card) {
    enableChromeAntialiasing(painter);
    painter.setPen(QPen(QColor(255, 255, 255, 45), 1));
    painter.setBrush(QColor(20, 21, 27));
    painter.drawRoundedRect(QRectF(0.5, 0.5, card.width() - 1.0, card.height() - 1.0), 10.0,
                            10.0);
}
}  // namespace

SettingsFlyout::SettingsFlyout(QWidget* parent)
    : QFrame(parent), quality_(QStringLiteral("HD")) {
    setObjectName(QStringLiteral("playerSettingsFlyout"));
    // Marks a panel that stands over the picture: the host raises its paint
    // order when it appears, exactly as Qt z-order used to.
    setProperty("chromePanel", true);
    setFocusPolicy(Qt::StrongFocus);
    hide();

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(10, 10, 10, 10);
    lay->setSpacing(5);

    auto* head = new QHBoxLayout();
    speedCaption_ = new ChromeLabel(tr("SPEED"), monoFont(8, true), QColor(0x9a, 0x9a, 0xa6), this);
    head->addWidget(speedCaption_);
    head->addStretch();
    // The pill behind this label is this card's own paint: a ChromeLabel draws
    // text only, which keeps the accent wash from becoming a widget of its own.
    badge_ = new ChromeLabel(QStringLiteral("1.0x"), monoFont(9, true), inkOn(accent()), this);
    badge_->setAlignment(Qt::AlignCenter);
    badge_->setContentsMargins(6, 1, 6, 1);
    badge_->setChromePadding(6, 1);
    head->addWidget(badge_);
    lay->addLayout(head);

    slider_ = new RangeSlider(accent(), 4, this);
    slider_->setObjectName(QStringLiteral("playerSpeedSlider"));
    slider_->setAccessibleName(tr("Speed"));
    connect(slider_, &RangeSlider::valueChanged, this, &SettingsFlyout::onSlider);
    lay->addWidget(slider_);

    // A hairline is this card's paint, not a widget's: a QFrame with a styled
    // background would paint its own strip over the picture.
    const auto addSeparator = [this, lay] {
        auto* sep = new QWidget(this);
        sep->setFixedHeight(1);
        sep->setAttribute(Qt::WA_TransparentForMouseEvents);
        lay->addWidget(sep);
        separators_.append(sep);
        return sep;
    };
    addSeparator();

    for (const SpeedPreset& preset : kSpeedPresets) {
        auto* btn = new ChromeRow(QString(), this);
        btn->setFixedHeight(26);
        btn->setRowPadding(10);
        const double speed = preset.speed;
        connect(btn, &QPushButton::clicked, this, [this, speed] { pick(speed); });
        lay->addWidget(btn);
        rows_.append(std::make_pair(speed, btn));
    }

    addSeparator();

    // Quality lives here rather than in a chip over the picture: the gear is
    // where a viewer looks for stream settings, and the strip is for things
    // that act on the current episode.
    qualitySection_ = new QWidget(this);
    qualitySection_->setObjectName(QStringLiteral("playerQualitySection"));
    qualitySection_->setAccessibleName(tr("Quality"));
    auto* qlay = new QVBoxLayout(qualitySection_);
    qlay->setContentsMargins(0, 0, 0, 0);
    qlay->setSpacing(5);

    auto* qrow = new QHBoxLayout();
    qualityCaption_ = new ChromeLabel(tr("QUALITY"), monoFont(8, true), QColor(0x9a, 0x9a, 0xa6),
                                      qualitySection_);
    qualityCaption_->setObjectName(QStringLiteral("playerQualityCaption"));
    qrow->addWidget(qualityCaption_);
    qrow->addStretch();
    qualityLabel_ = new ChromeLabel(QStringLiteral("HD"), monoFont(9, true), green(),
                                    qualitySection_);
    qualityLabel_->setObjectName(QStringLiteral("playerQualityLabel"));
    qrow->addWidget(qualityLabel_);
    qlay->addLayout(qrow);

    qualityBody_ = new QVBoxLayout();
    qualityBody_->setContentsMargins(0, 0, 0, 0);
    qualityBody_->setSpacing(2);
    qlay->addLayout(qualityBody_);
    lay->addWidget(qualitySection_);
    qualitySection_->setVisible(false);

    // The display mode belongs next to quality: both are "how the picture is
    // shown", and a viewer reaching for one is already looking at this card.
    // Original 1:1 stays a Settings-only choice - a quarter-screen picture is
    // not a quick switch, and Fit is the way back from every row here.
    scaleSection_ = new QWidget(this);
    scaleSection_->setObjectName(QStringLiteral("playerScaleSection"));
    auto* slay = new QVBoxLayout(scaleSection_);
    slay->setContentsMargins(0, 0, 0, 0);
    slay->setSpacing(2);
    scaleCaption_ = new ChromeLabel(tr("SCALING"), monoFont(8, true), QColor(0x9a, 0x9a, 0xa6),
                                    scaleSection_);
    scaleCaption_->setObjectName(QStringLiteral("playerScaleCaption"));
    slay->addWidget(scaleCaption_);
    const mplayer::VideoScaleMode kQuickModes[] = {
        mplayer::VideoScaleMode::Fit, mplayer::VideoScaleMode::Fill,
        mplayer::VideoScaleMode::Stretch, mplayer::VideoScaleMode::Aspect16x9,
        mplayer::VideoScaleMode::Aspect4x3};
    for (const mplayer::VideoScaleMode mode : kQuickModes) {
        auto* btn = new ChromeRow(QString(), scaleSection_);
        btn->setFixedHeight(26);
        btn->setRowPadding(10);
        const int value = static_cast<int>(mode);
        // The row carries the mode it selects, so the order of this list is not
        // also the meaning of the value the refresh loop reads back.
        btn->setProperty("videoScaleMode", value);
        connect(btn, &QPushButton::clicked, this, [this, value] {
            close();
            emit videoScaleModeSelected(value);
        });
        slay->addWidget(btn);
        scaleRows_.append(btn);
    }
    lay->addWidget(scaleSection_);

    addSeparator();

    defaultPlayerButton_ = new ChromeRow(QString(), this);
    defaultPlayerButton_->setObjectName(QStringLiteral("playerDefaultVideoPlayerButton"));
    defaultPlayerButton_->setFont(uiFont(10));
    defaultPlayerButton_->setFocusPolicy(Qt::StrongFocus);
    defaultPlayerButton_->setRowPadding(10);
    defaultPlayerButton_->setMinimumHeight(34);
    defaultPlayerButton_->setEnabled(mikai::gui::systemDefaultPlayerSupported());
    lay->addWidget(defaultPlayerButton_);
    connect(defaultPlayerButton_, &QPushButton::clicked, this, [this] {
        close();
        emit defaultPlayerRequested();
    });
    // The badge, the speed track and the selection washes carry the accent; a
    // live change restates them instead of waiting for the flyout to be rebuilt.
    connect(&accentNotifier(), &AccentNotifier::changed, this, [this] {
        badge_->setChromeColor(inkOn(accent()));
        slider_->setAccent(accent());
        // The badge pill and every row's wash are painted from the tokens at
        // paint time; the card itself only needs to be marked for a repaint.
        chromeUpdate(this);
        refresh();
    });

    retranslate();
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
        auto* btn = new ChromeRow(QString(), qualitySection_);
        btn->setFixedHeight(26);
        btn->setRowPadding(10);
        connect(btn, &QPushButton::clicked, this, [this, height] {
            close();
            emit qualitySelected(height);
        });
        qualityBody_->addWidget(btn);
        qualityRows_.append(btn);
        // Created after the host walked the tree, so it is not in the adopted
        // list yet. A rebuild happens while the popover may already be open.
        if (ChromeHost* host = chromeHostOf(this)) {
            host->chromeAdopt(btn);
        }
    }
    qualitySection_->setVisible(!qualityOptions_.isEmpty());
    refresh();
}

void SettingsFlyout::setVideoScaleMode(int mode) {
    videoScaleMode_ = mode;
    refresh();
}

void SettingsFlyout::changeEvent(QEvent* event) {
    QFrame::changeEvent(event);
    if (event->type() == QEvent::LanguageChange) {
        retranslate();
    }
}

void SettingsFlyout::retranslate() {
    speedCaption_->setText(tr("SPEED"));
    qualityCaption_->setText(tr("QUALITY"));
    scaleCaption_->setText(tr("SCALING"));
    // Spoken names never reach the painter, so only this handler refreshes them.
    slider_->setAccessibleName(tr("Speed"));
    qualitySection_->setAccessibleName(tr("Quality"));
    scaleSection_->setAccessibleName(tr("Video scaling"));
    const QString defaultText = tr("Set as default video player…");
    if (defaultPlayerButton_->text() != defaultText) {
        defaultPlayerButton_->setText(defaultText);
        chromeUpdate(defaultPlayerButton_);
    }
    defaultPlayerButton_->setAccessibleName(defaultText);
#ifdef Q_OS_LINUX
    const QString defaultDescription = tr("Registers Mikai Ja Nai and opens desktop settings so you can choose it under Default Applications. Your defaults are not changed automatically.");
#else
    const QString defaultDescription = mikai::gui::systemDefaultPlayerSupported()
        ? tr("Opens Windows Default Apps so you can choose Mikai Ja Nai. Your defaults are not changed automatically.")
        : tr("Setting the default video player is only supported on Windows.");
#endif
    defaultPlayerButton_->setToolTip(defaultDescription);
    defaultPlayerButton_->setAccessibleDescription(defaultDescription);
    // refresh() rewrites the translated preset rows from rate_ and the
    // caller-held quality options: neither selection moves.
    refresh();
    if (!isVisible()) {
        // popupNear() measures the new wording when it opens.
        return;
    }
    // popupNear() froze the size for the previous wording, and a longer caption
    // would be clipped by it. The anchor is gone by now, so the corner stays.
    resizeToContents();
    clampInsideHost(this);
    chromeUpdate(this);
}

void SettingsFlyout::paintChrome(QPainter& painter) {
    paintPopoverCard(painter, *this);
    // The hairlines and the badge's pill are part of this card's paint: their
    // boxes come from the layout, so they follow the rows that move.
    enableChromeAntialiasing(painter);
    painter.setPen(Qt::NoPen);
    painter.setBrush(w10());
    for (QWidget* sep : separators_) {
        if (sep == nullptr || !sep->isVisible()) {
            continue;
        }
        painter.drawRect(sep->geometry());
    }
    if (badge_ != nullptr && badge_->isVisible()) {
        painter.setBrush(accent());
        painter.drawRoundedRect(QRectF(badge_->geometry()), 4.0, 4.0);
    }
}

void SettingsFlyout::paintEvent(QPaintEvent*) { paintOwnBackingStore(this); }

void SettingsFlyout::resizeToContents() {
    // Drop the previous fixed bounds before measuring translated/wrapped text.
    setMinimumSize(0, 0);
    setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
    layout()->invalidate();
    layout()->activate();
    int targetWidth = layout()->sizeHint().width();
    if (parentWidget() != nullptr) {
        targetWidth = qMin(targetWidth, qMax(1, parentWidget()->width() - 8));
    }
    const int wrappedHeight = layout()->totalHeightForWidth(targetWidth);
    setFixedSize(targetWidth, wrappedHeight >= 0 ? wrappedHeight : layout()->sizeHint().height());
    layout()->activate();
}

void SettingsFlyout::popupNear(const QPoint& anchorGlobal) {
    show();
    refresh();
    ensurePolished();
    resizeToContents();
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
    // in which only the badge and the slider move. Writing a row re-lays it
    // out, so each row's text and role are written only when they differ from
    // what is on screen. A ChromeRow's own setters repaint the surface.
    const QString badgeText = formatG(rate_) + QStringLiteral("x");
    if (badge_->text() != badgeText) {
        badge_->setText(badgeText);
        // The pill is drawn over this label's box, and the label only draws text
        // inside its own padding, so the box carries the padding here.
        const QFontMetrics badgeMetrics(badge_->font());
        badge_->setFixedSize(badgeMetrics.horizontalAdvance(badgeText) + 12,
                             badgeMetrics.height() + 2);
    }
    if (qualityLabel_->text() != quality_) {
        qualityLabel_->setText(quality_);
    }
    slider_->setValueSilent((rate_ - 0.25) / 1.75);

    for (int i = 0; i < qualityRows_.size() && i < qualityOptions_.size(); ++i) {
        const QualityOption& option = qualityOptions_[i];
        ChromeRow* btn = qualityRows_[i];
        const QString text = (option.current ? QStringLiteral("✓  ") : QString()) + option.label;
        if (btn->text() != text) {
            btn->setText(text);
            chromeUpdate(btn);
        }
        btn->setRole(option.current ? ChromeRow::Role::Current : ChromeRow::Role::Choice);
    }
    for (int i = 0; i < scaleRows_.size(); ++i) {
        ChromeRow* btn = scaleRows_[i];
        // A mode outside this list (Original 1:1, chosen in Settings) simply
        // ticks nothing here.
        const int mode = btn->property("videoScaleMode").toInt();
        const bool selected = videoScaleMode_ == mode;
        const QString text = (selected ? QStringLiteral("✓  ") : QString())
            + mplayer::videoScaleModeLabel(static_cast<mplayer::VideoScaleMode>(mode));
        if (btn->text() != text) {
            btn->setText(text);
            chromeUpdate(btn);
        }
        btn->setRole(selected ? ChromeRow::Role::Current : ChromeRow::Role::Choice);
    }
    for (int i = 0; i < rows_.size(); ++i) {
        const double speed = rows_[i].first;
        ChromeRow* btn = rows_[i].second;
        const bool selected = std::abs(rate_ - speed) < 0.026;
        const QString text = (selected ? QStringLiteral("✓  ") : QString())
            + QStringLiteral("%1x   %2").arg(formatG(speed), presetLabel(i));
        if (btn->text() != text) {
            btn->setText(text);
            chromeUpdate(btn);
        }
        btn->setRole(selected ? ChromeRow::Role::Current : ChromeRow::Role::Choice);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  CHROME LABEL
// ═══════════════════════════════════════════════════════════════════════════

ChromeLabel::ChromeLabel(const QString& text, const QFont& font, const QColor& color,
                         QWidget* parent)
    : QLabel(text, parent), color_(color) {
    setFont(font);
    setAttribute(Qt::WA_StyledBackground, false);
}

void ChromeLabel::setText(const QString& text) {
    if (text == QLabel::text()) {
        return;
    }
    QLabel::setText(text);
    chromeUpdate(this);
}

void ChromeLabel::setChromeColor(const QColor& color) {
    if (color_ == color) {
        return;
    }
    color_ = color;
    chromeUpdate(this);
}

void ChromeLabel::setChromePadding(int horizontal, int vertical) {
    if (padX_ == horizontal && padY_ == vertical) {
        return;
    }
    padX_ = horizontal;
    padY_ = vertical;
    chromeUpdate(this);
}

void ChromeLabel::setChromeWordWrap(bool wrap) {
    if (wrap_ == wrap) return;
    wrap_ = wrap;
    chromeUpdate(this);
}

void ChromeLabel::paintChrome(QPainter& painter) {
    enableChromeAntialiasing(painter);
    painter.setFont(font());
    painter.setPen(color_);
    int flags = static_cast<int>(alignment());
    if (wrap_) flags |= Qt::TextWordWrap;
    painter.drawText(rect().adjusted(padX_, padY_, -padX_, -padY_), flags, text());
}

void ChromeLabel::paintEvent(QPaintEvent*) { paintOwnBackingStore(this); }

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
    spinnerTimer_.setInterval(40);
    connect(&spinnerTimer_, &QTimer::timeout, this, [this] {
        spinnerAngle_ = std::fmod(spinnerAngle_ + 15.0, 360.0);
        chromeUpdate(this);
    });
    hide();
}

void CenterGlyph::setKind(Kind kind) {
    kind_ = kind;
    if (loading_) {
        show();
        chromeUpdate(this);
        return;
    }
    if (kind == Kind::None) {
        hide();
        return;
    }
    show();
    chromeUpdate(this);
}

void CenterGlyph::setLoading(bool loading) {
    if (loading_ == loading) {
        return;
    }
    loading_ = loading;
    if (loading_) {
        setAccessibleName(tr("Loading comparison"));
        setAccessibleDescription(tr("A comparison is being prepared."));
        spinnerAngle_ = 0.0;
        spinnerTimer_.start();
        show();
    } else {
        spinnerTimer_.stop();
        setAccessibleName(QString());
        setAccessibleDescription(QString());
        setKind(kind_);
    }
    chromeUpdate(this);
}

void CenterGlyph::paintEvent(QPaintEvent*) { paintOwnBackingStore(this); }

void CenterGlyph::paintChrome(QPainter& painter) {
    enableChromeAntialiasing(painter);
    if (!loading_ && kind_ == Kind::None) {
        return;
    }
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

    if (loading_) {
        const QPointF center(width() / 2.0, height() / 2.0);
        painter.setRenderHint(QPainter::Antialiasing);
        QPen track(QColor(255, 255, 255, 70), 4.0, Qt::SolidLine, Qt::RoundCap);
        painter.setPen(track);
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(center, 17.0, 17.0);
        QPen arc(w95(), 4.0, Qt::SolidLine, Qt::RoundCap);
        painter.setPen(arc);
        painter.drawArc(QRectF(center.x() - 17.0, center.y() - 17.0, 34.0, 34.0),
                        static_cast<int>(spinnerAngle_ * 16.0), 245 * 16);
        return;
    }

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
//  TOAST
// ═══════════════════════════════════════════════════════════════════════════

Toast::Toast(QWidget* parent) : Panel(QColor(0, 0, 0, 205), 8.0, w15(), parent) {
    setAttribute(Qt::WA_TransparentForMouseEvents);

    label_ = new ChromeLabel(QString(), uiFont(10), white(235), this);
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
    timer_->start(ms);
}

// ═══════════════════════════════════════════════════════════════════════════
//  KEY HINTS
// ═══════════════════════════════════════════════════════════════════════════

QVector<std::pair<QString, QString>> KeyHints::shortcutTable() {
    return {
        {QStringLiteral("Space"), tr("Pause / play")},
        {QStringLiteral("F"), tr("Fullscreen / windowed")},
        {QStringLiteral("M"), tr("Mute / unmute audio")},
        {QStringLiteral("S"), tr("Episode list")},
        {QStringLiteral("T"), tr("Choose audio track")},
        {QStringLiteral("C"), tr("Choose subtitle track")},
        {QStringLiteral("Q"), tr("Quality and speed")},
        {QStringLiteral("←  →"), tr("Seek ∓5s")},
        {QStringLiteral("↑  ↓"), tr("Volume ±10%")},
        {QStringLiteral("Esc"), tr("Close panel / window")},
        {QStringLiteral("F11"), tr("Fullscreen / windowed")},
        {QStringLiteral("? / /"), tr("Show keyboard shortcuts in player")},
        {QStringLiteral("Enter"), tr("Search from the search field")},
        {QStringLiteral("Shift + scroll"), tr("Jump through home feed rows")},
        {QStringLiteral("Alt + scroll"), tr("Scroll title rows or change provider")},
        {QStringLiteral("Scroll"), tr("Adjust volume over the player volume control")},
        {QStringLiteral("Double-click"), tr("Toggle player fullscreen")},
        {QStringLiteral("F / S / R / P"), tr("Compare: fit / swap / recapture / pin")},
        {QStringLiteral("Space"), tr("Compare: hold to peek at side A")},
        {QStringLiteral("F11"), tr("Compare: toggle fullscreen")},
        {QStringLiteral("+ / − / 0"), tr("Cover viewer: zoom in / out / reset")},
    };
}

namespace {
// The keycap inset and minimum column width the shortcut card was laid out
// with. The wash behind a key is the card's own paint, sized from the key
// label's box, so both stay in one place.
constexpr int kKeyCapPadX = 6;
constexpr int kKeyCapPadY = 2;
constexpr int kKeyCapMinWidth = 68;
}  // namespace

KeyHints::KeyHints(QWidget* parent) : QFrame(parent) {
    setObjectName(QStringLiteral("playerKeyHints"));
    setProperty("chromePanel", true);
    setFocusPolicy(Qt::StrongFocus);
    // The keycaps are an accent wash read at paint time, so a live accent
    // change only has to mark the card for a repaint.
    connect(&accentNotifier(), &AccentNotifier::changed, this, [this] { chromeUpdate(this); });
    hide();

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(12, 10, 12, 10);
    lay->setSpacing(4);
    QFont captionFont = monoFont(8, true);
    // The caption was letter-spaced by a stylesheet rule; the painter gets the
    // same spacing from the font instead.
    captionFont.setLetterSpacing(QFont::AbsoluteSpacing, 0.5);
    caption_ = new ChromeLabel(tr("KEYBOARD SHORTCUTS"), captionFont, QColor(0x9a, 0x9a, 0xa6), this);
    lay->addWidget(caption_);
    lay->addSpacing(2);

    // A plain layout widget, not a scroll area: a viewport would be a second
    // raster over the picture, which is the repaint this card must not cause.
    content_ = new QWidget(this);
    auto* rowsLayout = new QVBoxLayout(content_);
    rowsLayout->setContentsMargins(0, 0, 0, 0);
    rowsLayout->setSpacing(4);
    lay->addWidget(content_);

    const QVector<std::pair<QString, QString>> table = shortcutTable();
    keys_.reserve(table.size());
    descriptions_.reserve(table.size());
    for (const auto& entry : table) {
        auto* row = new QHBoxLayout();
        row->setSpacing(8);
        auto* key = new ChromeLabel(entry.first, monoFont(8, true), white(235), content_);
        key->setObjectName(QStringLiteral("playerKeyHintKey"));
        key->setAlignment(Qt::AlignCenter);
        key->setContentsMargins(kKeyCapPadX, kKeyCapPadY, kKeyCapPadX, kKeyCapPadY);
        key->setChromePadding(kKeyCapPadX, kKeyCapPadY);
        // The painted text is inset by the keycap padding, so the box must be
        // big enough for the glyphs plus that inset.
        const QFontMetrics keyMetrics(key->font());
        key->setMinimumWidth(
            qMax(kKeyCapMinWidth, keyMetrics.horizontalAdvance(entry.first) + 2 * kKeyCapPadX));
        key->setMinimumHeight(keyMetrics.height() + 2 * kKeyCapPadY);
        auto* action = new ChromeLabel(entry.second, uiFont(9), white(180), content_);
        row->addWidget(key);
        keys_.append(key);
        row->addWidget(action, 1);
        descriptions_.append(action);
        rowsLayout->addLayout(row);
    }
}

void KeyHints::changeEvent(QEvent* event) {
    QFrame::changeEvent(event);
    if (event->type() == QEvent::LanguageChange) {
        retranslate();
    }
}

void KeyHints::retranslate() {
    caption_->setText(tr("KEYBOARD SHORTCUTS"));
    const auto table = shortcutTable();
    const qsizetype rows = qMin(descriptions_.size(), table.size());
    for (qsizetype row = 0; row < rows; ++row) {
        descriptions_.at(row)->setText(table.at(row).second);
    }
    if (!isVisible()) {
        // popupNear() measures the new wording when it opens.
        return;
    }
    // The card was sized for the previous wording; a longer row must not be
    // clipped by that frozen width.
    layout()->invalidate();
    layout()->activate();
    const QSize contentSize = content_->sizeHint();
    const QSize hostSize = parentWidget() != nullptr ? parentWidget()->size() : QSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
    setFixedSize(qMin(hostSize.width() - 8, contentSize.width() + 48),
                 qMin(hostSize.height() - 8, contentSize.height() + caption_->sizeHint().height() + 32));
    layout()->activate();
    clampInsideHost(this);
    chromeUpdate(this);
}

void KeyHints::paintChrome(QPainter& painter) {
    paintPopoverCard(painter, *this);
    // The keycap wash: one rounded box per key column, behind the label the
    // surface paints on top of it.
    enableChromeAntialiasing(painter);
    QColor wash = accent();
    wash.setAlphaF(0.18);
    QColor edge = accent();
    edge.setAlphaF(0.42);
    painter.setPen(QPen(edge, 1.0));
    painter.setBrush(wash);
    for (ChromeLabel* key : keys_) {
        if (key == nullptr || !key->isVisible()) {
            continue;
        }
        const QRect box(key->mapTo(this, QPoint(0, 0)), key->size());
        painter.drawRoundedRect(QRectF(box).adjusted(0.5, 0.5, -0.5, -0.5), 4.0, 4.0);
    }
}

void KeyHints::paintEvent(QPaintEvent*) { paintOwnBackingStore(this); }

void KeyHints::popupNear(const QPoint& anchorGlobal) {
    show();
    ensurePolished();
    layout()->invalidate();
    layout()->activate();
    const QSize contentSize = content_->sizeHint();
    const QSize hostSize = parentWidget() != nullptr ? parentWidget()->size() : QSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
    setFixedSize(qMin(hostSize.width() - 8, contentSize.width() + 48),
                 qMin(hostSize.height() - 8, contentSize.height() + caption_->sizeHint().height() + 32));
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
