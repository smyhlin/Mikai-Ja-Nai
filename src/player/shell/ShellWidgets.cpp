#include <utility>
#include "ShellWidgets.hpp"

#include <QCoreApplication>
#include <QEnterEvent>
#include <QEvent>
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

/// paintRounded() with a brush instead of a flat colour, so an IconButton's
/// standing fill can be the gradient the float's play disc is drawn with.
void paintRoundedBrush(QPainter& painter, const QRectF& rect, double radius, const QBrush& fill,
                       const QColor& border, double borderWidth) {
    painter.setPen(border.isValid() ? QPen(border, borderWidth) : QPen(Qt::NoPen));
    painter.setBrush(fill);
    painter.drawRoundedRect(rect, radius, radius);
}

/// True for a widget that paints itself and covers what is under it: the
/// popovers. They are the chrome's opaque cards, so they are not adopted into
/// the surface painter and neither is anything they own.
bool selfPaintedChrome(const QWidget* widget) {
    return widget->property("playerOverlay").toBool();
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
        host->chromeDirty(widget, rect);
    }
}

void ChromeList::adopt(QWidget* root, QObject* watcher) {
    entries_.clear();
    addSubtree(root, watcher);
}

void ChromeList::addSubtree(QWidget* root, QObject* watcher) {
    if (root == nullptr) {
        return;
    }
    // The reference, not a copy: adopting never creates or destroys a widget,
    // and this runs once per host rather than per frame.
    const QObjectList& children = root->children();
    for (QObject* object : children) {
        auto* child = qobject_cast<QWidget*>(object);
        if (child == nullptr || child->isWindow() || selfPaintedChrome(child)) {
            continue;
        }
        if (auto* painted = dynamic_cast<ChromePaint*>(child)) {
            // A widget already adopted keeps its attributes and its filter: a
            // second pass only records it again.
            if (!painted->surfacePainted()) {
                painted->setSurfacePainted(true);
                // Translucent, so the window still paints the part of this
                // widget that falls outside the picture; the two background
                // attributes go with it, or Qt fills a slab behind the chrome.
                child->setAttribute(Qt::WA_TranslucentBackground, true);
                child->setAttribute(Qt::WA_StyledBackground, false);
                child->setAutoFillBackground(false);
                if (watcher != nullptr) {
                    child->installEventFilter(watcher);
                }
            }
            entries_.append(Entry{child, painted});
        }
        addSubtree(child, watcher);
    }
}

void chromeRepaintEvent(ChromeHost* host, QObject* watched, QEvent* event) {
    if (host == nullptr || event == nullptr) {
        return;
    }
    auto* widget = qobject_cast<QWidget*>(watched);
    if (widget == nullptr) {
        return;
    }
    // Only an adopted widget: a surface that asked for its own repaint is
    // already painting (routing that back would be the repaint loop this
    // mechanism exists to avoid), and a popover covers what is under it.
    auto* adopted = dynamic_cast<ChromePaint*>(widget);
    if (adopted == nullptr || !adopted->surfacePainted()) {
        return;
    }
    // Deliberately not QEvent::UpdateRequest: a control over a QOpenGLWidget is
    // repainted again whenever the picture under it is, and that arrives as a
    // paint, not a request - routing requests too would hang a repaint of the
    // picture off every extra invalidation, which is the paused-picture loop to
    // avoid. Every state change the chrome has routes itself through
    // chromeUpdate() instead.
    switch (event->type()) {
        case QEvent::Show:
        case QEvent::Hide:
        case QEvent::EnabledChange:
        // A font, a palette or a stylesheet change redraws chrome pixels that
        // no widget update would carry.
        case QEvent::FontChange:
        case QEvent::PaletteChange:
        case QEvent::StyleChange:
            host->chromeDirty(widget, QRect());
            break;
        // A move or a resize also leaves stale pixels where the widget was, and
        // no rect in the widget's own coordinates describes those.
        case QEvent::Move:
        case QEvent::Resize:
            host->chromeDirtyAll();
            break;
        default:
            break;
    }
}

void ChromeList::paint(QPainter& painter, QWidget* root, const QRect& clip) const {
    if (root == nullptr) {
        return;
    }
    for (const Entry& entry : entries_) {
        QWidget* widget = entry.widget;
        if (widget == nullptr || !widget->isVisible()) {
            continue;
        }
        // Recomputed rather than cached: a control that moved keeps its slot in
        // the list, and this is a walk up its own parent chain.
        const QPoint at = widget->mapTo(root, QPoint(0, 0));
        QRect box(at, widget->size());
        // Qt clips a child to its parent's rect; nesting must behave the same
        // here, or an oversized child would paint past its container.
        for (QWidget* parent = widget->parentWidget(); parent != nullptr && parent != root;
             parent = parent->parentWidget()) {
            box = box.intersected(QRect(parent->mapTo(root, QPoint(0, 0)), parent->size()));
            if (box.isEmpty()) {
                break;
            }
        }
        if (box.isEmpty() || !box.intersects(clip)) {
            continue;
        }
        painter.save();
        painter.translate(at);
        painter.setClipRect(box.translated(-at), Qt::IntersectClip);
        entry.painted->paintChrome(painter);
        painter.restore();
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
    paintRounded(painter, QRectF(0.5, 0.5, width() - 1, height() - 1), radius_, fill_, border_);
}

void Panel::paintEvent(QPaintEvent*) { paintOwnBackingStore(this); }

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
    hover_ = true;
    chromeUpdate(this);
    QPushButton::enterEvent(event);
}

void IconButton::leaveEvent(QEvent* event) {
    hover_ = false;
    chromeUpdate(this);
    QPushButton::leaveEvent(event);
}

void IconButton::paintChrome(QPainter& painter) {
    // Inset by the stroke width, so a bordered button's outline lands inside
    // its own rect rather than being clipped by it.
    const QRectF box(0.5, 0.5, width() - 1.0, height() - 1.0);
    const bool lit = hover_ || isChecked();
    if (chrome_.fill.style() != Qt::NoBrush) {
        paintRoundedBrush(painter, box, chrome_.radius, chrome_.fill, chrome_.border,
                          chrome_.borderWidth);
    }
    if (lit && chrome_.hoverFill.style() != Qt::NoBrush) {
        paintRoundedBrush(painter, box, chrome_.radius, chrome_.hoverFill, chrome_.hoverBorder,
                          chrome_.borderWidth);
    }
    const QColor color = hasTint_ ? tint_ : w90();
    const qreal off = (width() - glyph_) / 2.0;
    if (icon_) {
        icon_(painter, off, off, glyph_, color);
    }
}

void IconButton::paintEvent(QPaintEvent*) { paintOwnBackingStore(this); }

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
    speedCaption_ = new QLabel(tr("SPEED"));
    speedCaption_->setFont(monoFont(8));
    speedCaption_->setStyleSheet(QStringLiteral("color: #9a9aa6; font-weight: 700; background: transparent;"));
    head->addWidget(speedCaption_);
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
    slider_->setObjectName(QStringLiteral("playerSpeedSlider"));
    slider_->setAccessibleName(tr("Speed"));
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
        btn->setFocusPolicy(Qt::NoFocus);
        const double speed = preset.speed;
        connect(btn, &QPushButton::clicked, this, [this, speed] { pick(speed); });
        lay->addWidget(btn);
        rows_.append(std::make_pair(speed, btn));
    }

    auto* sep2 = new QFrame(this);
    sep2->setFixedHeight(1);
    sep2->setStyleSheet(QStringLiteral("background: rgba(255, 255, 255, 0.10);"));
    lay->addWidget(sep2);

    // Quality lives here rather than in a chip over the picture: the gear is
    // where a viewer looks for stream settings, and the strip is for things
    // that act on the current episode.
    qualitySection_ = new QWidget(this);
    qualitySection_->setObjectName(QStringLiteral("playerQualitySection"));
    qualitySection_->setAccessibleName(tr("Quality"));
    qualitySection_->setStyleSheet(QStringLiteral("background: transparent;"));
    auto* qlay = new QVBoxLayout(qualitySection_);
    qlay->setContentsMargins(0, 0, 0, 0);
    qlay->setSpacing(5);

    auto* qrow = new QHBoxLayout();
    qualityCaption_ = new QLabel(tr("QUALITY"));
    qualityCaption_->setObjectName(QStringLiteral("playerQualityCaption"));
    qualityCaption_->setFont(monoFont(8));
    qualityCaption_->setStyleSheet(QStringLiteral("color: #9a9aa6; font-weight: 700; background: transparent;"));
    qrow->addWidget(qualityCaption_);
    qrow->addStretch();
    qualityLabel_ = new QLabel(QStringLiteral("HD"));
    qualityLabel_->setObjectName(QStringLiteral("playerQualityLabel"));
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

void SettingsFlyout::changeEvent(QEvent* event) {
    QFrame::changeEvent(event);
    if (event->type() == QEvent::LanguageChange) {
        retranslate();
    }
}

void SettingsFlyout::retranslate() {
    speedCaption_->setText(tr("SPEED"));
    qualityCaption_->setText(tr("QUALITY"));
    // Spoken names never reach the painter, so only this handler refreshes them.
    slider_->setAccessibleName(tr("Speed"));
    qualitySection_->setAccessibleName(tr("Quality"));
    // refresh() rewrites the translated preset rows from rate_ and the
    // caller-held quality options: neither selection moves.
    refresh();
    if (!isVisible()) {
        // popupNear() measures the new wording when it opens.
        return;
    }
    // popupNear() froze the size for the previous wording, and a longer caption
    // would be clipped by it. The anchor is gone by now, so the corner stays.
    layout()->invalidate();
    layout()->activate();
    setFixedSize(layout()->sizeHint());
    layout()->activate();
    clampInsideHost(this);
    update();
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

void ChromeLabel::paintChrome(QPainter& painter) {
    painter.setFont(font());
    painter.setPen(color_);
    painter.drawText(rect(), alignment(), text());
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
    hide();
}

void CenterGlyph::setKind(Kind kind) {
    kind_ = kind;
    if (kind == Kind::None) {
        hide();
        return;
    }
    show();
    chromeUpdate(this);
}

void CenterGlyph::paintEvent(QPaintEvent*) { paintOwnBackingStore(this); }

void CenterGlyph::paintChrome(QPainter& painter) {
    if (kind_ == Kind::None) {
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

PillButton::PillButton(QWidget* parent) : QWidget(parent), font_(uiFont(10)) {
    setCursor(Qt::PointingHandCursor);
    retranslate();
}

void PillButton::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::LanguageChange) {
        retranslate();
    }
}

void PillButton::retranslate() {
    const QString text = tr("Original File");
    if (text == text_) {
        return;
    }
    text_ = text;
    // The caption is painted, so the name is the only thing a screen reader
    // has to go on.
    setAccessibleName(text_);
    // The fixed size is both the hit target and the clip the caption is drawn
    // into, so a translated string re-measures both.
    setFixedSize(textWidth(font_, text_) + 26, 22);
    chromeUpdate(this);
}

void PillButton::enterEvent(QEnterEvent* event) {
    hover_ = true;
    chromeUpdate(this);
    QWidget::enterEvent(event);
}

void PillButton::leaveEvent(QEvent* event) {
    hover_ = false;
    chromeUpdate(this);
    QWidget::leaveEvent(event);
}

void PillButton::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        emit clicked();
    }
}

void PillButton::paintChrome(QPainter& painter) {
    const QColor color = hover_ ? w95() : w70();
    IconPainter::folder(painter, 1, 4, 14, color);
    painter.setFont(font_);
    painter.setPen(color);
    painter.drawText(QRectF(19, 0, width() - 19, height()), Qt::AlignVCenter, text_);
}

void PillButton::paintEvent(QPaintEvent*) { paintOwnBackingStore(this); }

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
    caption_ = new QLabel(tr("KEYBOARD SHORTCUTS"));
    caption_->setFont(monoFont(8));
    caption_->setStyleSheet(QStringLiteral("color: #9a9aa6; font-weight: 700; background: transparent; letter-spacing: 0.5px;"));
    lay->addWidget(caption_);
    lay->addSpacing(2);

    const QVector<std::pair<QString, QString>> table = shortcutTable();
    descriptions_.reserve(table.size());
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
        descriptions_.append(action);
        row->addWidget(action, 1);
        lay->addLayout(row);
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
    setFixedSize(layout()->sizeHint());
    layout()->activate();
    clampInsideHost(this);
    update();
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
