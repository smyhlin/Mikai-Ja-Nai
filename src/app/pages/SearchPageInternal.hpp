#pragma once

#include <utility>
#include "state/SettingsKeys.hpp"
#include "pages/SearchPage.hpp"

#include "gui/Clipboard.hpp"
#include "gui/ImageDecode.hpp"
#include "Labels.hpp"
#include "state/Feeds.hpp"
#include "support/I18n.hpp"
#include "theme/Colors.hpp"
#include "theme/Icons.hpp"
#include "state/AppearanceStore.hpp"
#include "theme/Metrics.hpp"
#include "theme/Palette.hpp"
#include "theme/Theme.hpp"
#include "widgets/CarouselButton.hpp"
#include "widgets/FlowLayout.hpp"
#include "widgets/RangePager.hpp"
#include "widgets/RateLimitLabel.hpp"
#include "widgets/FailureView.hpp"
#include "widgets/PageGlow.hpp"
#include "widgets/TitleCard.hpp"
#include "workers/Runner.hpp"
#include "ShellWidgets.hpp"

#include <backend/MikaiClient.hpp>
#include <backend/UpdateFeed.hpp>

#include <QAccessible>
#include <QAccessibilityHints>
#include <QDateTime>
#include <QDebug>
#include <QDesktopServices>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QSpinBox>
#include <QGuiApplication>
#include <QStyleHints>
#include <QFont>
#include <QFontMetricsF>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QEasingCurve>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMouseEvent>
#include <QPixmap>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QResizeEvent>
#include <QStyle>
#include <QScrollArea>
#include <QShortcut>
#include <QStackedWidget>
#include <QScrollBar>
#include <QSizePolicy>
#include <QSettings>
#include <QTabBar>
#include <QToolButton>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <memory>

namespace mikai::app {
namespace search_page_detail {

inline const QString kRecentKey = QStringLiteral("recent");

inline bool isRemoteUrl(const QString& value) {
    return value.startsWith(QLatin1String("https://")) ||
           value.startsWith(QLatin1String("http://"));
}

/// "13.09 · 21:04", or "21:04" when only the time is wanted. Empty when the
/// timestamp does not parse: a feed row is better short than wrong.
inline QString formatFeedTime(const QString& timestamp, bool timeOnly = false) {
    if (timestamp.isEmpty()) {
        return {};
    }
    QDateTime value = QDateTime::fromString(timestamp, Qt::ISODateWithMs);
    if (!value.isValid()) {
        value = QDateTime::fromString(timestamp, Qt::ISODate);
    }
    if (!value.isValid()) {
        return {};
    }
    const QDateTime local = value.toLocalTime();
    return timeOnly ? local.toString(QStringLiteral("HH:mm"))
                    : local.toString(QStringLiteral("dd.MM · HH:mm"));
}

/// The biggest box anything on the page draws a poster into, in logical pixels:
/// the home card poster. Search rows (56x78) and feed posters (34x48) are
/// smaller, so decoding down to its device size leaves no consumer upscaling.
inline const QSize kThumbDecodeSize(theme::kHomePosterWidth, theme::kHomePosterHeight);

/// Enable a carousel's arrows only where there is somewhere to scroll to.
inline void syncArrows(QScrollBar* bar, QPushButton* previous, QPushButton* next) {
    previous->setEnabled(bar->value() > bar->minimum());
    next->setEnabled(bar->value() < bar->maximum());
}

inline int activeCatalogueFilterCount(const backend::AnimeQuery& query) {
    int count = 0;
    count += query.genres.size() + query.genres_exclude.size();
    count += query.tags.size() + query.tags_exclude.size();
    count += query.years.size();
    if (query.year_from || query.year_to) count += 1;
    count += query.seasons.size() + query.formats.size() + query.statuses.size();
    count += query.studios.size() + query.teams.size();
    return count;
}

class FlowScrollHost final : public QWidget {
public:
    explicit FlowScrollHost(QWidget* parent = nullptr) : QWidget(parent) {
        QSizePolicy p(QSizePolicy::Expanding, QSizePolicy::Minimum);
        p.setHeightForWidth(true);
        setSizePolicy(p);
    }
    bool hasHeightForWidth() const override { return layout() != nullptr && layout()->hasHeightForWidth(); }
    int heightForWidth(int width) const override {
        return layout() != nullptr ? layout()->heightForWidth(width) : QWidget::heightForWidth(width);
    }
    QSize sizeHint() const override {
        if (layout() != nullptr && width() > 0) {
            return QSize(width(), layout()->heightForWidth(width()));
        }
        return QWidget::sizeHint();
    }
};
inline bool hasExplicitCatalogueCriteria(const backend::AnimeQuery& query) {
    if (!query.genres.isEmpty() || !query.genres_exclude.isEmpty()) return true;
    if (!query.tags.isEmpty() || !query.tags_exclude.isEmpty()) return true;
    if (!query.years.isEmpty() || query.year_from || query.year_to) return true;
    if (!query.seasons.isEmpty() || !query.formats.isEmpty() || !query.statuses.isEmpty()) return true;
    if (!query.studios.isEmpty() || !query.teams.isEmpty()) return true;
    if (query.genres_match != QLatin1String("all") || query.tags_match != QLatin1String("all")) return true;
    return query.sort != QStringLiteral("name") || !query.order.isEmpty();
}

// Keep the return arrow independent of font fallback and subpixel text rendering.
class SearchKeycap : public QLabel {
public:
    explicit SearchKeycap(QWidget* parent = nullptr) : QLabel(parent) {
        setObjectName(QStringLiteral("searchKeycap"));
        setAlignment(Qt::AlignCenter);
        setFocusPolicy(Qt::NoFocus);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        applyGreyText();
    }

    QSize sizeHint() const override {
        const int edge = 2 * theme::kSearchKeycapBorder;
        return QSize(contentWidth() + edge, theme::kSearchKeycapContentHeight + edge);
    }
    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void paintEvent(QPaintEvent*) override {
        const theme::SearchHeaderPalette roles =
            theme::searchHeaderPalette(currentAppearance());
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::TextAntialiasing, true);

        const qreal edge = theme::kSearchKeycapBorder;
        const qreal radius = theme::kSearchKeycapRadius;
        const QRectF bed(rect());
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(roles.keycapSurface));
        painter.drawRoundedRect(bed, radius, radius);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(QColor(roles.keycapBorder), edge));
        painter.drawRoundedRect(bed.adjusted(edge / 2, edge / 2, -edge / 2, -edge / 2),
                                 radius, radius);

        const QFontMetricsF metrics(font());
        const QString caption = text();
        const qreal captionWidth = metrics.horizontalAdvance(caption);
        const qreal textLeft = std::round((width() - captionWidth) / 2.0);
        const qreal baseline =
            std::round((height() - metrics.height()) / 2.0 + metrics.ascent());

        const QColor ink(roles.pillText);
        painter.save();
        painter.setBrush(ink);
        painter.setPen(Qt::NoPen);
        painter.translate(textLeft, baseline);
        painter.fillPath(captionPath(caption), ink);
        painter.restore();
    }

    void changeEvent(QEvent* event) override {
        QLabel::changeEvent(event);
        switch (event->type()) {
        case QEvent::FontChange:
            // The sheet restates the type size whenever it is re-polished.
            applyGreyText();
            updateGeometry();
            update();
            break;
        case QEvent::StyleChange:
        case QEvent::PaletteChange:
        case QEvent::ApplicationPaletteChange:
        case QEvent::LayoutDirectionChange:
            // The bed, hairline, ink and arrow come from the live theme.
            update();
            break;
        default:
            break;
        }
    }

private:
    struct CaptionPath {
        QPainterPath path;
        QString text;
        QString font;
        qreal ratio = 0.0;
    };
    CaptionPath caption_;

    static constexpr QFont::StyleStrategy kGreyText =
        static_cast<QFont::StyleStrategy>(QFont::PreferAntialias |
                                          QFont::NoSubpixelAntialias);

    void applyGreyText() {
        const auto strategy = font().styleStrategy();
        if ((strategy & QFont::PreferAntialias) == QFont::PreferAntialias &&
            (strategy & QFont::NoSubpixelAntialias) == QFont::NoSubpixelAntialias) {
            return;
        }
        QFont adjusted = font();
        adjusted.setStyleStrategy(kGreyText);
        setFont(adjusted);
    }

    /// Filled outlines, not drawText: Qt's subpixel text path ignores the
    /// no-subpixel hint on some Windows renderers and leaves colour fringes on
    /// a glyph this small.
    const QPainterPath& captionPath(const QString& caption) {
        const qreal ratio = devicePixelRatioF();
        if (caption_.path.isEmpty() || caption_.text != caption ||
            caption_.font != font().key() || caption_.ratio != ratio) {
            caption_.path = QPainterPath();
            caption_.path.addText(0.0, 0.0, font(), caption);
            caption_.text = caption;
            caption_.font = font().key();
            caption_.ratio = ratio;
        }
        return caption_.path;
    }

    int contentWidth() const {
        const QFontMetricsF metrics(font());
        return 2 * theme::kSearchKeycapPaddingH + qCeil(metrics.horizontalAdvance(text()));
    }
};

}  // namespace search_page_detail

}  // namespace mikai::app
