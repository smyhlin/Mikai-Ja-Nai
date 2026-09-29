#include "PlayerPopups.hpp"

#include <QGuiApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QVBoxLayout>

namespace mplayer {
namespace shell {
namespace {

// ── mikai theme tokens, as the app defines them ────────────────────────────
// The app keeps its palette in Python; the popovers reproduce the exact values
// so a stylesheet here matches the rest of the client.

constexpr int kSpacingXs = 4;
constexpr int kSpacingSmall = 8;
constexpr int kRowHeight = 28;

// Qt's QWIDGETSIZE_MAX is a C++ macro; `refresh` needs it to undo the fixed
// geometry a previous popupNear left behind.
constexpr int kUnlimited = 16777215;

const char* const kText = "#f0f0f5";
const char* const kMuted = "#9a9aa6";

constexpr auto kPanelQss =
    "QFrame[chromePanel=\"true\"] {"
    "  background-color: transparent;"
    "  border: none; }";

QString rowQss(const char* foreground, const char* fill) {
    return QStringLiteral(
               "QPushButton {"
               "  color: %1;"
               "  background: %2;"
               "  border: 1px solid transparent; border-radius: 6px; text-align: left;"
               "  padding: 0 %3px; min-height: 28px; font-size: 13px; font-weight: 500; }"
               "QPushButton:hover {"
               "  background: rgba(255, 255, 255, 0.12); color: #ffffff;"
               "  border-color: rgba(255, 255, 255, 0.20); }")
        .arg(QLatin1String(foreground), QLatin1String(fill))
        .arg(kSpacingSmall);
}

QString captionQss(int size) {
    return QStringLiteral("color: %1; font-size: %2px; font-weight: 700; background: transparent; letter-spacing: 0.5px;")
        .arg(QLatin1String(kMuted))
        .arg(size);
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
//  MENU POPUP
// ═══════════════════════════════════════════════════════════════════════════

MenuPopup::MenuPopup(const QString& title, QWidget* parent)
    : QFrame(parent, Qt::Popup | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint) {
    setObjectName(QStringLiteral("playerMenuPopup"));
    setProperty("chromePanel", true);
    setAttribute(Qt::WA_TranslucentBackground);
    setFocusPolicy(Qt::StrongFocus);
    setStyleSheet(QString::fromLatin1(kPanelQss));

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(kSpacingSmall, kSpacingSmall, kSpacingSmall, kSpacingXs);
    outer->setSpacing(kSpacingXs);

    auto* caption = new QLabel(title, this);
    caption->setObjectName(QStringLiteral("playerMenuCaption"));
    caption->setStyleSheet(captionQss(11));
    outer->addWidget(caption);

    body_ = new QVBoxLayout();
    body_->setContentsMargins(0, 0, 0, 0);
    body_->setSpacing(2);
    outer->addLayout(body_);

    // A long hint would set the popover's width, and a quality list two rows
    // high does not want to be 500 px wide.
    auto* hint = new QLabel(QStringLiteral("↑↓ · Enter · Esc"), this);
    hint->setObjectName(QStringLiteral("playerMenuHint"));
    hint->setStyleSheet(captionQss(10));
    outer->addWidget(hint);

    setToolTip(QStringLiteral("Керування: ↑ ↓ — вибір, Enter — підтвердити, Esc — закрити"));
}

int MenuPopup::rowCount() const {
    return rows_.size();
}

void MenuPopup::setEntries(const QVector<MenuEntry>& entries) {
    while (body_->count()) {
        if (QWidget* widget = body_->takeAt(0)->widget()) {
            widget->setParent(nullptr);
            widget->deleteLater();
        }
    }
    rows_.clear();
    rowEntries_.clear();
    highlight_ = -1;

    for (const MenuEntry& entry : entries) {
        if (!entry.heading.isEmpty()) {
            auto* label = new QLabel(entry.heading, this);
            label->setObjectName(QStringLiteral("playerMenuHeading"));
            label->setStyleSheet(captionQss(11) + QStringLiteral(" padding: 4px 6px;"));
            body_->addWidget(label);
            continue;
        }
        const QString text = entry.text;
        auto* button = new QPushButton(text, this);
        button->setObjectName(QStringLiteral("playerMenuRow"));
        button->setToolTip(entry.tooltip.isEmpty() ? text : entry.tooltip);
        button->setAccessibleName(text);
        button->setCursor(Qt::PointingHandCursor);
        button->setFocusPolicy(Qt::NoFocus);
        button->setFixedHeight(kRowHeight);
        const int spot = rows_.size();
        connect(button, &QPushButton::clicked, this, [this, spot] { activate(spot); });
        body_->addWidget(button);
        rows_.append(button);
        rowEntries_.append(entry);
    }
    paintRows();
    fitToHint();
}

void MenuPopup::refresh() {
    fitToHint();
}

void MenuPopup::popupNear(const QPoint& anchorGlobal, bool preferAbove) {
    fitToHint();
    QScreen* screen = QGuiApplication::screenAt(anchorGlobal);
    if (!screen)
        screen = QGuiApplication::primaryScreen();

    const int above = anchorGlobal.y() - height() - 8;
    int x = anchorGlobal.x() - width();
    int y = preferAbove ? above : anchorGlobal.y() + 8;
    if (screen) {
        const QRect bounds = screen->availableGeometry();
        if (preferAbove && above < bounds.top() + 4)
            y = anchorGlobal.y() + 8;
        x = qBound(bounds.left() + 4, x, bounds.right() - width() - 4);
        y = qBound(bounds.top() + 4, y, bounds.bottom() - height() - 4);
    }
    move(x, y);
    show();
    raise();
    // The topmost flag is set in the constructor: calling setWindowFlag on a
    // visible widget hides it, because Qt rebuilds the native window.
    raise();
    activateWindow();
    setFocus(Qt::OtherFocusReason);
}

void MenuPopup::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(QColor(255, 255, 255, 45), 1));
    p.setBrush(QColor(20, 21, 27));
    p.drawRoundedRect(QRectF(0.5, 0.5, width() - 1, height() - 1), 10.0, 10.0);
}

void MenuPopup::keyPressEvent(QKeyEvent* event) {
    switch (event->key()) {
        case Qt::Key_Escape:
            close();
            break;
        case Qt::Key_Down:
        case Qt::Key_Right:
            moveHighlight(1);
            break;
        case Qt::Key_Up:
        case Qt::Key_Left:
            moveHighlight(-1);
            break;
        case Qt::Key_Home:
            setHighlight(0);
            break;
        case Qt::Key_End:
            setHighlight(rows_.size() - 1);
            break;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            if (highlight_ >= 0)
                activate(highlight_);
            break;
        default:
            QFrame::keyPressEvent(event);
            return;
    }
    event->accept();
}

void MenuPopup::fitToHint() {
    // A previous popupNear pinned both edges, and adjustSize alone would be
    // bounded by that stale geometry.
    setMinimumSize(0, 0);
    setMaximumSize(kUnlimited, kUnlimited);
    setFixedSize(sizeHint());
}

void MenuPopup::moveHighlight(int delta) {
    if (rows_.isEmpty())
        return;
    if (highlight_ < 0) {
        setHighlight(delta > 0 ? 0 : rows_.size() - 1);
        return;
    }
    setHighlight((highlight_ + delta) % rows_.size());
}

void MenuPopup::setHighlight(int spot) {
    if (rows_.isEmpty())
        return;
    highlight_ = qBound(0, spot, rows_.size() - 1);
    paintRows();
}

void MenuPopup::activate(int spot) {
    if (spot < 0 || spot >= rowEntries_.size())
        return;
    const QVariant payload = rowEntries_.at(spot).payload;
    close();
    emit picked(payload);
}

void MenuPopup::paintRows() {
    for (int index = 0; index < rows_.size(); ++index) {
        if (index < rowEntries_.size() && rowEntries_.at(index).current)
            rows_.at(index)->setStyleSheet(rowQss("#ffffff", "rgba(88, 101, 242, 0.40)"));
        else if (index == highlight_)
            rows_.at(index)->setStyleSheet(rowQss("#ffffff", "rgba(255, 255, 255, 0.12)"));
        else
            rows_.at(index)->setStyleSheet(rowQss("#e0e0e6", "rgba(255, 255, 255, 0.05)"));
    }
}



// ═══════════════════════════════════════════════════════════════════════════
//  SERIES DRAWER
// ═══════════════════════════════════════════════════════════════════════════

void SeriesDrawer::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(QColor(255, 255, 255, 45), 1));
    p.setBrush(QColor(20, 21, 27));
    p.drawRoundedRect(QRectF(0.5, 0.5, width() - 1, height() - 1), 10.0, 10.0);
}

SeriesDrawer::SeriesDrawer(QWidget* parent) : QFrame(parent) {
    setObjectName(QStringLiteral("playerSeriesDrawer"));
    setProperty("chromePanel", true);
    setStyleSheet(QString::fromLatin1(kPanelQss));

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(kSpacingXs, kSpacingXs, kSpacingXs, kSpacingXs);
    outer->setSpacing(kSpacingXs);

    auto* caption = new QLabel(QStringLiteral("СЕРІЇ"), this);
    caption->setObjectName(QStringLiteral("playerSeriesCaption"));
    caption->setStyleSheet(captionQss(11));
    outer->addWidget(caption);

    auto* scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("playerSeriesScroll"));
    scroll->setStyleSheet(QStringLiteral("background: transparent; border: none;"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* content = new QWidget();
    content->setObjectName(QStringLiteral("playerSeriesList"));
    content->setStyleSheet(QStringLiteral("background: transparent;"));
    rowsLayout_ = new QVBoxLayout(content);
    rowsLayout_->setContentsMargins(0, 0, 0, 0);
    rowsLayout_->setSpacing(2);
    scroll->setWidget(content);
    outer->addWidget(scroll, 1);
}

void SeriesDrawer::setEntries(const QStringList& labels, int current) {
    while (rowsLayout_->count()) {
        if (QWidget* widget = rowsLayout_->takeAt(0)->widget()) {
            widget->setParent(nullptr);
            widget->deleteLater();
        }
    }
    rows_.clear();
    for (int spot = 0; spot < labels.size(); ++spot) {
        const QString label = labels.at(spot);
        auto* button = new QPushButton(label, this);
        button->setObjectName(spot == current ? QStringLiteral("playerSeriesItemCurrent")
                                              : QStringLiteral("playerSeriesItem"));
        button->setCursor(Qt::PointingHandCursor);
        button->setToolTip(label);
        button->setAccessibleName(label);
        // A focused button would swallow Space, and Space is play/pause.
        button->setFocusPolicy(Qt::NoFocus);
        button->setFixedHeight(kRowHeight);
        connect(button, &QPushButton::clicked, this, [this, spot] { emit picked(spot); });
        rowsLayout_->addWidget(button);
        rows_.append(button);
        button->setStyleSheet(spot == current ? rowQss(kText, "rgba(189, 172, 255, 0.20)")
                                              : rowQss(kMuted, "transparent"));
    }
    rowsLayout_->addStretch(1);
}

int SeriesDrawer::contentHeight() const {
    return rows_.size() * (kRowHeight + 2) + 3 * kSpacingXs + 20;
}

}  // namespace shell
}  // namespace mplayer
