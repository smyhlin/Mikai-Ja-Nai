#pragma once

#include <utility>
#include "pages/TitlePage.hpp"

#include "pages/EpisodeGrid.hpp"

#include "gui/Clipboard.hpp"
#include "gui/ImageDecode.hpp"
#include "gui/UiProfile.hpp"
#include "Labels.hpp"
#include "dialogs/CoverViewer.hpp"
#include "state/AppearanceStore.hpp"
#include "state/NativePlaybackAssembly.hpp"
#include "state/QualityCache.hpp"
#include "theme/Colors.hpp"
#include "theme/Icons.hpp"
#include "theme/Metrics.hpp"
#include "theme/Palette.hpp"
#include "theme/Theme.hpp"
#include "widgets/FailureView.hpp"
#include "widgets/CarouselButton.hpp"
#include "widgets/FlowLayout.hpp"
#include "widgets/RangePager.hpp"
#include "widgets/TitleCard.hpp"
#include "widgets/RateLimitLabel.hpp"
#include "workers/Runner.hpp"

#include <player/quick/PlayerController.hpp>
#include <player/shell/ShellTheme.hpp>
#include <backend/MikaiClient.hpp>
#include <backend/Releases.hpp>
#include <backend/Streams.hpp>
#include <native/bundle/NativeRuntimeSelection.hpp>

#include <QAbstractButton>
#include <QAccessible>
#include <QApplication>
#include <QBoxLayout>
#include <QButtonGroup>
#include <QColor>
#include <QtCore/QDebug>
#include <QDesktopServices>
#include <QDateTime>
#include <QEasingCurve>
#include <QEvent>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QFileDialog>
#include <QFrame>
#include <QGraphicsBlurEffect>
#include <QGraphicsDropShadowEffect>
#include <QGraphicsPixmapItem>
#include <QGraphicsScene>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QProgressBar>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QLayout>
#include <QMenu>
#include <QMouseEvent>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QProcess>
#include "backend/Downloads.hpp"

#include <QPropertyAnimation>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStandardPaths>
#include <QStyle>
#include <QStyleOption>
#include <QPointer>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QTextLayout>
#include <QTextOption>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>

namespace mikai::app {


namespace title_page_detail {

constexpr bool kShowHeroChips = true;
constexpr int kHeroGenreMax = 6;
constexpr int kHeroTagMax = 3;
constexpr int kSheetSlideMs = 220;
constexpr int kSheetSlidePx = 36;

/// Flips a label's dynamic "state" property and re-resolves only that label.
inline void setLabelState(QLabel* label, const char* state) {
    if (label == nullptr) {
        return;
    }
    const QString wanted = QString::fromLatin1(state);
    if (label->property("state").toString() == wanted) {
        return;
    }
    label->setProperty("state", wanted);
    label->style()->unpolish(label);
    label->style()->polish(label);
    label->update();
}

/// Genre and tag chips for the hero, rebuilt from the retained detail so a
/// language switch re-words them. Purely presentational: no signal, no state.
inline void fillHeroChips(QWidget* host, const backend::AnimeDetail& detail,
                   QObject* filterOwner) {
    if (host == nullptr || host->layout() == nullptr) {
        return;
    }
    QLayout* layout = host->layout();
    while (QLayoutItem* item = layout->takeAt(0)) {
        if (item->widget() != nullptr) {
            item->widget()->deleteLater();
        }
        delete item;
    }
    if (!kShowHeroChips) {
        host->hide();
        return;
    }
    int added = 0;
    const auto addChips = [&](const auto& source, int limit, const QString& objectName) {
        int count = 0;
        for (const backend::GenreTag& tag : source) {
            if (count >= limit) {
                break;
            }
            const QString text = tag.ua.isEmpty() ? tag.name : tag.ua;
            if (text.isEmpty()) {
                continue;
            }
            auto* chip = new QLabel(text, host);
            chip->setObjectName(objectName);
            chip->setTextFormat(Qt::PlainText);
            // A chip is a link into the catalogue filtered by that term. The
            // canonical name travels in a property: the visible text follows the
            // interface language, the filter value does not.
            chip->setProperty(objectName == QLatin1String("titleTagChip") ? "mikaiTag" : "mikaiGenre",
                              tag.name);
            chip->setCursor(Qt::PointingHandCursor);
            // A free function has no tr(): the chips belong to the page's
            // translation context.
            chip->setToolTip(TitlePage::tr("Show titles in %1").arg(text));
            chip->setAccessibleName(TitlePage::tr("Show titles in %1").arg(text));
            if (filterOwner != nullptr) chip->installEventFilter(filterOwner);
            layout->addWidget(chip);
            ++count;
            ++added;
        }
    };
    addChips(detail.genres, kHeroGenreMax, QStringLiteral("titleGenreChip"));
    addChips(detail.tags, kHeroTagMax, QStringLiteral("titleTagChip"));
    host->setVisible(added > 0);
}

inline QColor episodeTypeColor(const QString& type) {
    if (type == QLatin1String("mangaCanon")) return QColor(theme::kEpisodeMangaCanon);
    if (type == QLatin1String("animeCanon")) return QColor(theme::kEpisodeAnimeCanon);
    if (type == QLatin1String("mixed")) return QColor(theme::kEpisodeMixed);
    if (type == QLatin1String("filler")) return QColor(theme::kEpisodeFiller);
    return {};
}

inline QPixmap episodeTypeDot(const QString& type, qreal devicePixelRatio) {
    constexpr int size = 10;
    QPixmap pixmap(qRound(size * devicePixelRatio), qRound(size * devicePixelRatio));
    pixmap.setDevicePixelRatio(devicePixelRatio);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(episodeTypeColor(type));
    painter.drawEllipse(QRectF(1, 1, size - 2, size - 2));
    return pixmap;
}

class ReleaseKindIcon final : public QWidget {
public:
    ReleaseKindIcon(bool subtitles, QWidget* parent) : QWidget(parent), subtitles_(subtitles) {
        setFixedSize(theme::kIconSize, theme::kControlHeight - theme::kSpacingSmall);
        setAttribute(Qt::WA_TransparentForMouseEvents);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(palette().color(QPalette::PlaceholderText), 1.5,
                            Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);
        painter.translate(width() / 2.0, height() / 2.0);
        if (subtitles_) {
            painter.drawRoundedRect(QRectF(-7, -5, 14, 10), 2, 2);
            painter.drawLine(QPointF(-4, -1), QPointF(-1, -1));
            painter.drawLine(QPointF(1, -1), QPointF(4, -1));
            painter.drawLine(QPointF(-4, 2), QPointF(4, 2));
        } else {
            for (int i = -2; i <= 2; ++i) {
                const qreal half = i == 0 ? 7 : (qAbs(i) == 1 ? 5 : 2);
                painter.drawLine(QPointF(i * 3, -half), QPointF(i * 3, half));
            }
        }
    }

private:
    bool subtitles_;
};

// Styled pane geometry can omit frame space still reserved by Qt's minimum-size contract.
class TitlePageTabs final : public QTabWidget {
public:
    using QTabWidget::QTabWidget;

    int contentWidthForOuterWidth(int outerWidth) const {
        QStyleOptionTabWidgetFrame option;
        initStyleOption(&option);
        const QSize contents(qMax(1, outerWidth), 1);
        const QSize framed =
            style()->sizeFromContents(QStyle::CT_TabWidget, &option, contents, this);
        const int frameWidth = qMax(0, framed.width() - contents.width());
        return qMax(1, outerWidth - frameWidth);
    }
};

// Two-line elided title that measures with its live font() and re-elides on
// font, style, language or DPI changes. Replaces fixed-height QLabel + pre-elide.
class ElidedTwoLineLabel final : public QWidget {
public:
    explicit ElidedTwoLineLabel(const QString& text, QWidget* parent = nullptr)
        : QWidget(parent), fullText_(text) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
    }

    void setFullText(const QString& text) {
        if (fullText_ == text) return;
        fullText_ = text;
        updateGeometry();
        update();
    }

    QSize sizeHint() const override {
        const QFontMetrics fm(font());
        return QSize(10, 2 * fm.lineSpacing());
    }
    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void paintEvent(QPaintEvent*) override {
        if (fullText_.isEmpty()) return;
        const QString elided = TitleCard::elideTwoLines(fullText_, font(), width());
        if (elided.isEmpty()) return;
        QPainter painter(this);
        painter.setPen(palette().color(QPalette::WindowText));
        const QFontMetrics fm(font());
        const int lineH = fm.lineSpacing();
        const QStringList lines = elided.split(QLatin1Char('\n'));
        int y = 0;
        for (const QString& line : lines) {
            const int textW = fm.horizontalAdvance(line);
            const int x = (width() - textW) / 2;
            painter.drawText(x, y + fm.ascent(), line);
            y += lineH;
        }
    }

    void changeEvent(QEvent* e) override {
        QWidget::changeEvent(e);
        if (e->type() == QEvent::FontChange ||
            e->type() == QEvent::StyleChange ||
            e->type() == QEvent::LanguageChange ||
            e->type() == QEvent::ScreenChangeInternal) {
            updateGeometry();
            update();
        }
    }

private:
    QString fullText_;
};

}  // namespace title_page_detail
namespace title_page_detail {

inline void fitCarousel(QScrollArea* scroll, QWidget* track,
                 const QList<QPointer<QPushButton>>& cards, int inset) {
    if (!scroll || !track) return;

    int maxCardH = 0;
    for (const auto& cardPtr : cards) {
        if (cardPtr.isNull()) continue;
        QPushButton* card = cardPtr.data();
        card->ensurePolished();
        for (QWidget* child : card->findChildren<QWidget*>()) {
            child->ensurePolished();
        }
        if (card->layout()) {
            card->layout()->invalidate();
            card->layout()->activate();
            const int h = card->layout()->sizeHint().height();
            if (h > maxCardH) maxCardH = h;
        }
    }
    if (maxCardH <= 0) {
        maxCardH = theme::kCarouselCardHeight;
    }

    for (const auto& cardPtr : cards) {
        if (!cardPtr.isNull()) {
            cardPtr->setFixedHeight(maxCardH);
        }
    }

    track->setFixedHeight(maxCardH + 2 * inset);
    scroll->setFixedHeight(track->height() + 2 * scroll->frameWidth());

    // Debug guard
    for (const auto& cardPtr : cards) {
        if (cardPtr.isNull()) continue;
        if (auto* lay = cardPtr->layout()) {
            if (lay->minimumSize().height() > cardPtr->height() + 1) {
                qWarning() << "carousel card content taller than card:"
                           << "cardH=" << cardPtr->height()
                           << "minLayoutH=" << lay->minimumSize().height();
            }
        }
    }
}

// Event filter that triggers refit on style/font/DPI/language changes for a carousel.
class CarouselFitFilter final : public QObject {
public:
    CarouselFitFilter(QScrollArea* s, QWidget* t, const QList<QPointer<QPushButton>>& cs, int ins)
        : QObject(t), scroll(s), track(t), cards(cs), inset(ins) {}

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched != track) return false;
        switch (event->type()) {
        case QEvent::Show:
        case QEvent::StyleChange:
        case QEvent::FontChange:
        case QEvent::ApplicationFontChange:
        case QEvent::LanguageChange:
        case QEvent::DevicePixelRatioChange:
        case QEvent::ScreenChangeInternal:
            QTimer::singleShot(0, this, [this] {
                fitCarousel(scroll, track, cards, inset);
            });
            break;
        default: break;
        }
        return false;
    }

private:
    QPointer<QScrollArea> scroll;
    QPointer<QWidget> track;
    QList<QPointer<QPushButton>> cards;
    int inset;
};

}  // namespace title_page_detail
/// The cover blurred once per cover, tails included, on a surface small enough
/// that the poster reads as its palette and not as a second face.
inline QImage blurredCover(const QImage& poster) {
    const QImage small = poster.scaled(theme::kTitleBackdropSourceWidth,
                                       theme::kTitleBackdropSourceHeight,
                                       Qt::KeepAspectRatioByExpanding,
                                       Qt::SmoothTransformation);
    QGraphicsScene scene;
    QGraphicsPixmapItem* item = scene.addPixmap(QPixmap::fromImage(small));
    auto* blur = new QGraphicsBlurEffect;
    blur->setBlurRadius(theme::kTitleBackdropBlurRadius);
    blur->setBlurHints(QGraphicsBlurEffect::QualityHint);
    item->setGraphicsEffect(blur);
    const QRectF bounds = blur->boundingRectFor(item->boundingRect());
    QImage out(bounds.size().toSize(), QImage::Format_ARGB32_Premultiplied);
    out.fill(Qt::transparent);
    QPainter painter(&out);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    scene.render(&painter, QRectF(out.rect()), bounds);
    return out;
}

/// A single cached composite behind the hero. Poster blur is refreshed only
/// when artwork changes; size and appearance changes only rebuild the composite.
class PosterBackdrop : public QWidget {
public:
    explicit PosterBackdrop(QWidget* parent) : QWidget(parent) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setFocusPolicy(Qt::NoFocus);
    }

    void setPoster(const QPixmap& poster) {
        if (poster.cacheKey() == posterKey_) {
            return;
        }
        posterKey_ = poster.cacheKey();
        image_ = poster.isNull() ? QImage() : blurredCover(poster.toImage());
        rebuild();
    }

    void refreshPalette() { rebuild(); }

protected:
    bool event(QEvent* event) override {
        const bool dprChanged = event->type() == QEvent::DevicePixelRatioChange;
        const bool result = QWidget::event(event);
        if (dprChanged) {
            rebuild();
        }
        return result;
    }

    void resizeEvent(QResizeEvent* event) override {
        QWidget::resizeEvent(event);
        rebuild();
    }

    void paintEvent(QPaintEvent*) override {
        if (!composite_.isNull()) {
            QPainter painter(this);
            painter.drawPixmap(0, 0, composite_);
        }
    }

private:
    void rebuild() {
        composite_ = QPixmap();
        if (width() <= 0 || height() <= 0) {
            return;
        }
        const qreal dpr = devicePixelRatioF();
        const QSize pixels(qRound(width() * dpr), qRound(height() * dpr));
        QImage result(pixels, QImage::Format_ARGB32_Premultiplied);
        result.fill(Qt::transparent);
        QPainter painter(&result);
        painter.scale(dpr, dpr);
        const theme::Appearance& appearance = currentAppearance();
        const auto palette = theme::paletteFor(appearance);
        const bool light = palette.light;
        const qreal intensity = appearance.backgroundGradient
                                    ? qBound(0, appearance.gradientIntensity, 100) / 100.0
                                    : 0.0;
        const qreal gradientHeight =
            qBound(0, height() * qBound(0, appearance.gradientHeight, 100) / 100,
                   height());
        if (intensity > 0.0) {
            if (!image_.isNull()) {
                const qreal baseOpacity = light ? 0.07 : 0.14;
                const qreal opacity = qMin(light ? 0.10 : 0.20,
                                           baseOpacity * intensity * (100.0 / 30.0));
                painter.setOpacity(opacity);
                painter.drawImage(QPoint(0, 0),
                                  image_.scaled(theme::kTitleBackdropWidth,
                                                theme::kTitleBackdropHeight,
                                                Qt::KeepAspectRatioByExpanding,
                                                Qt::SmoothTransformation));
                painter.setOpacity(1.0);
            }
            QColor configured = theme::gradientColor(appearance);
            configured.setAlphaF(intensity * (light ? 0.08 : 0.16));
            painter.fillRect(rect(), configured);
            QImage mask(pixels, QImage::Format_Alpha8);
            mask.fill(0);
            for (int y = 0; y < pixels.height(); ++y) {
                const qreal logicalY = y / dpr;
                const qreal top = qBound<qreal>(0.0, logicalY / 16.0, 1.0);
                const qreal fadeStart = gradientHeight * 0.35;
                const qreal bottom = logicalY <= fadeStart
                                         ? 1.0
                                         : qBound<qreal>(0.0,
                                                         (gradientHeight - logicalY) /
                                                             qMax<qreal>(1.0, gradientHeight - fadeStart),
                                                         1.0);
                const qreal vertical = top * bottom;
                for (int x = 0; x < pixels.width(); ++x) {
                    const qreal logicalX = x / dpr;
                    const qreal left = qBound<qreal>(0.0, logicalX / 16.0, 1.0);
                    const qreal right = qBound<qreal>(0.0, (width() * 0.85 - logicalX) /
                                                               qMax(1.0, width() * 0.85 - 16.0), 1.0);
                    mask.scanLine(y)[x] = uchar(qRound(255.0 * vertical * left * right));
                }
            }
            painter.setCompositionMode(QPainter::CompositionMode_DestinationIn);
            painter.drawImage(QRect(0, 0, width(), height()), mask);
            painter.end();
        } else {
            painter.end();
        }
        composite_ = QPixmap::fromImage(std::move(result));
        composite_.setDevicePixelRatio(dpr);
        update();
    }

    qint64 posterKey_ = 0;
    QImage image_;
    QPixmap composite_;
};

namespace title_page_detail {

constexpr int kFlashMs = 1200;

/// The highest usable height in a quality map, 0 when there is none.
inline int maxQuality(const QMap<int, QString>& map) {
    int best = 0;
    for (auto it = map.constBegin(); it != map.constEnd(); ++it) {
        if (it.key() > 0 && !it.value().trimmed().isEmpty()) {
            best = qMax(best, it.key());
        }
    }
    return best;
}

/// A wrapping label inside a vertical layout has to declare height-for-width,
/// or the layout keeps the one-line height and clips a long title or synopsis
/// once the column narrows.
inline void wrapLabel(QLabel* label) {
    if (label == nullptr) {
        return;
    }
    label->setWordWrap(true);
    label->setMinimumWidth(0);
    QSizePolicy policy(QSizePolicy::Preferred, QSizePolicy::Minimum);
    policy.setHeightForWidth(true);
    label->setSizePolicy(policy);
}

inline QString capitalize(const QString& text) {
    if (text.isEmpty()) {
        return text;
    }
    QString result = text;
    result[0] = result.at(0).toUpper();
    return result;
}

/// The identifier an episode shows on its pill, from its (possibly fractional)
/// number. Episodes number from anywhere, so this is never assumed to start at
/// 1. The API's label is only the identifier when there is no number: it is
/// usually the caption ("Episode 3"), which the pill cannot hold and the watch
/// panel would then word twice.
inline QString episodeNumber(const backend::Episode& episode) {
    if (episode.number.isValid() && !episode.number.isNull()) {
        const QString rendered = labels::variant(episode.number);
        if (!rendered.isEmpty()) {
            return rendered;
        }
    }
    if (!episode.label.isEmpty()) {
        return episode.label;
    }
    return QStringLiteral("?");
}

inline QString teamReference(const backend::Team& team) {
    if (!team.slug.isEmpty()) {
        return team.slug;
    }
    return team.id > 0 ? QString::number(team.id) : QString();
}

inline QString teamDisplayName(const backend::Team& team) {
    if (!team.name.isEmpty()) {
        return team.name;
    }
    return team.id > 0 ? QObject::tr("Team %1").arg(team.id) : team.slug;
}

inline QString licenseValueHtml(const QJsonValue& value) {
    if (value.isObject()) {
        QStringList rows;
        const QJsonObject object = value.toObject();
        for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
            const QString key = it.key().toHtmlEscaped();
            rows << QStringLiteral("<b>%1</b>: %2").arg(key, licenseValueHtml(it.value()));
        }
        return rows.join(QStringLiteral("<br>"));
    }
    if (value.isArray()) {
        QStringList items;
        for (const QJsonValue& item : value.toArray()) {
            const QString rendered = licenseValueHtml(item);
            if (!rendered.isEmpty()) {
                items << rendered;
            }
        }
        return items.join(QStringLiteral("<br>"));
    }
    if (value.isString()) {
        const QString text = value.toString();
        const QUrl url(text);
        if ((url.scheme() == QLatin1String("http") || url.scheme() == QLatin1String("https")) &&
            !url.host().isEmpty()) {
            const QString escaped = text.toHtmlEscaped();
            return QStringLiteral("<a href=\"%1\">%1</a>").arg(escaped);
        }
        return text.toHtmlEscaped();
    }
    if (value.isBool()) {
        return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    }
    return value.isNull() || value.isUndefined() ? QString() : value.toVariant().toString().toHtmlEscaped();
}

}  // namespace title_page_detail
}  // namespace mikai::app
