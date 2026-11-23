#include "widgets/TitleCard.hpp"

#include "Labels.hpp"

#include "gui/ImageDecode.hpp"
#include "state/AppearanceStore.hpp"
#include "theme/Metrics.hpp"

#include <QCoreApplication>
#include <QEvent>
#include <QFont>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QTextLayout>
#include <QTextOption>
#include <QPainter>
#include <QPainterPath>
#include <QLinearGradient>
#include <QPointF>
#include <QPixmap>
#include <QPushButton>
#include <QSizeF>
#include <QStyle>
#include <QVBoxLayout>

namespace mikai::app {
namespace {

const char* kPending = "…";

/// The card shows one count, never a sentence around it, so the plural is
/// the only thing a language has to reshape. Numerus keeps the "%n episodes"
/// form out of the source entirely and lets each catalog entry carry its own
/// singular/plural wording.
QString episodeCountText(int aired, int planned) {
    if (aired > 0 && planned > 0) {
        return planned == 1
                   ? QCoreApplication::translate("TitleCard", "%1 / 1 episode").arg(aired)
                   : QCoreApplication::translate("TitleCard", "%1 / %2 episodes")
                         .arg(aired)
                         .arg(planned);
    }
    const int count = aired > 0 ? aired : planned;
    if (count <= 0) {
        return {};
    }
    return count == 1 ? QCoreApplication::translate("TitleCard", "1 episode")
                      : QCoreApplication::translate("TitleCard", "%1 episodes").arg(count);
}

}  // namespace

TitleCard::TitleCard(const backend::AnimeSummary& summary, QWidget* parent)
    : QPushButton(parent) {
    ref_ = summary.ref;
    posterUrl_ = backend::posterUrl(summary);
    imageKey_ = backend::thumbUrl(summary);
    if (imageKey_.isEmpty()) {
        imageKey_ = posterUrl_;
    }
    if (summary.episodes.has_value()) {
        planned_ = *summary.episodes;
    }
    formatRaw_ = summary.format.trimmed();
    if (summary.year.has_value()) {
        year_ = *summary.year;
    }

    setObjectName(QStringLiteral("animeCard"));
    setFixedSize(theme::kHomeCardWidth, theme::kHomeCardHeight);
    setToolTip(labels::titleName(summary.titles));
    setContextMenuPolicy(Qt::CustomContextMenu);

    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(theme::kSpacingMedium, theme::kSpacingMedium, theme::kSpacingLarge,
                            theme::kSpacingMedium);
    row->setSpacing(theme::kSpacingMedium);

    poster_ = makeLabel(QStringLiteral("carouselPoster"), tr("Poster unavailable"));
    poster_->setAlignment(Qt::AlignCenter);
    poster_->setFixedSize(theme::kHomePosterWidth, theme::kHomePosterHeight);
    poster_->setWordWrap(true);
    // AlignLeft as well as AlignTop: AlignTop alone leaves the horizontal
    // unset, and the box centred the poster in whatever width it was given,
    // which is what pushed it away from the card edge.
    row->addWidget(poster_, 0, Qt::AlignTop | Qt::AlignLeft);

    auto* text = new QVBoxLayout();
    text->setContentsMargins(0, 0, 0, 0);
    text->setSpacing(theme::kSpacingXs);

    const QString fullTitle = labels::titleName(summary.titles);
    rawTitle_ = fullTitle;
    title_ = makeLabel(QStringLiteral("carouselTitle"), QString());
    title_->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    title_->setWordWrap(false);
    QFont titleFont = title_->font();
    titleFont.setPixelSize(13);
    titleFont.setWeight(QFont::DemiBold);
    const int titleMaxWidth = theme::kHomeCardWidth - theme::kSpacingMedium -
                              theme::kHomePosterWidth - theme::kSpacingMedium -
                              theme::kSpacingLarge - 8;
    title_->setText(elideTwoLines(fullTitle, titleFont, titleMaxWidth));
    title_->setFixedHeight(theme::kCarouselTitleHeight);
    // Expanding, not Preferred. A word-wrapped label keeps its sizeHint width,
    // so the column beside an 80px poster left the title wrapping inside a
    // 57px sliver while the rest of the card sat empty.
    title_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    status_ = makeLabel(QStringLiteral("carouselStatus"), QString());
    status_->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);

    meta_ = makeLabel(QStringLiteral("carouselStatus"), QString());
    meta_->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    QStringList metaParts;
    if (!formatRaw_.isEmpty()) {
        metaParts << formatRaw_.toUpper();
    }
    if (year_ > 0) {
        metaParts << QString::number(year_);
    }
    if (!metaParts.isEmpty()) {
        meta_->setText(metaParts.join(QStringLiteral(" · ")));
    } else {
        meta_->hide();
    }

    scoreLabel_ = makeLabel(QStringLiteral("carouselStatusLive"), QString());
    scoreLabel_->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    scoreLabel_->hide();

    // Keep the badge row clean (status + optional rating) and place format/year
    // on its own crisp muted line so narrow cards never crowd or clip pills.
    meta_->setObjectName(QStringLiteral("muted"));
    auto* badgesRow = new QHBoxLayout();
    badgesRow->setContentsMargins(0, 0, 0, 0);
    badgesRow->setSpacing(theme::kSpacingXs);
    badgesRow->addWidget(status_, 0, Qt::AlignVCenter);
    badgesRow->addWidget(scoreLabel_, 0, Qt::AlignVCenter);
    badgesRow->addStretch(1);

    episodes_ = makeLabel(QStringLiteral("carouselEpisodes"), QString());
    episodes_->setWordWrap(true);
    episodes_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    // The title box is always two lines tall, so a one-line title would leave
    // the status and the count floating halfway down the card against a
    // two-line one. Top-aligning the stack puts every card's facts on the
    // same line whichever way the title happens to wrap.
    altTitle_ = makeLabel(QStringLiteral("muted"), QString());
    altTitle_->setWordWrap(false);
    altTitle_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    altTitle_->hide();

    text->addWidget(title_, 0, Qt::AlignTop);
    text->addWidget(altTitle_, 0, Qt::AlignTop);
    text->addLayout(badgesRow);
    text->addWidget(meta_, 0, Qt::AlignTop);
    text->addWidget(episodes_, 0, Qt::AlignTop);
    text->addStretch(1);
    row->addLayout(text, 1);  // the text column takes everything the poster does not

    connect(this, &QPushButton::clicked, this,
            [this] { Q_EMIT openRequested(ref_, posterUrl_); });
    connect(this, &QWidget::customContextMenuRequested, this,
            [this](const QPoint& position) { Q_EMIT menuRequested(ref_, mapToGlobal(position)); });

    applyStatus(summary.status);
    applyEpisodes(0, planned_);
}

TitleCard::~TitleCard() = default;

QSize TitleCard::sizeHint() const {
    return QSize(theme::kHomeCardWidth, theme::kHomeCardHeight);
}

QSize TitleCard::minimumSizeHint() const {
    return sizeHint();
}

void TitleCard::changeEvent(QEvent* event) {
    QPushButton::changeEvent(event);
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
}

void TitleCard::retranslateUi() {
    // A landed poster owns the label: setText would drop the pixmap it is
    // showing, so only a label still on its fallback is re-worded.
    if (poster_ != nullptr && poster_->pixmap().isNull()) {
        poster_->setText(tr("Poster unavailable"));
    }
    const QString status = labels::status(statusRaw_);
    status_->setText(status.isEmpty() ? QString::fromLatin1(kPending) : status);
    const QString count =
        hasDetailStats_
            ? tr("Aired: %1 · Localized: %2 · Total: %3")
                  .arg(aired_)
                  .arg(localized_)
                  .arg(planned_ > 0 ? QString::number(planned_) : tr("unknown"))
            : episodeCountText(aired_, planned_);
    episodes_->setText(count.isEmpty() ? QString::fromLatin1(kPending) : count);
}

QLabel* TitleCard::makeLabel(const QString& objectName, const QString& text) {
    auto* label = new QLabel(text, this);
    label->setObjectName(objectName);
    // The card is the button; the labels are decoration and must not eat clicks.
    label->setAttribute(Qt::WA_TransparentForMouseEvents);
    return label;
}

QString TitleCard::elideTwoLines(const QString& text, const QFont& font, int maxWidth) {
    const QString clean = text.simplified();
    if (clean.isEmpty() || maxWidth <= 0) {
        return clean;
    }
    const QFontMetrics metrics(font);
    if (metrics.horizontalAdvance(clean) <= maxWidth) {
        return clean;
    }
    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    QTextLayout layout(clean, font);
    layout.setTextOption(option);
    layout.beginLayout();
    QTextLine firstLine = layout.createLine();
    if (!firstLine.isValid()) {
        layout.endLayout();
        return metrics.elidedText(clean, Qt::ElideRight, maxWidth);
    }
    firstLine.setLineWidth(maxWidth);
    layout.endLayout();

    const int split = firstLine.textStart() + firstLine.textLength();
    if (split >= clean.size()) {
        return clean;
    }
    const QString line1 = clean.left(split).trimmed();
    const QString remainder = clean.mid(split).trimmed();
    if (remainder.isEmpty()) {
        return line1;
    }
    const QString line2 = metrics.elidedText(remainder, Qt::ElideRight, maxWidth);
    return line1 + QLatin1Char('\n') + line2;
}

QPixmap TitleCard::rounded(const QPixmap& pixmap, const QSize& logical, qreal dpr, int radius,
                           bool shade) {
    if (pixmap.isNull() || logical.isEmpty()) {
        return {};
    }
    const qreal ratio = dpr > 0.0 ? dpr : 1.0;
    const QSize device = gui::deviceSizeFor(logical, ratio);

    // One smooth resample, straight to the device resolution the label shows.
    // Scaling to the logical size first and letting Qt enlarge the result on a
    // HiDPI display was the "soft poster" half of the unpolished report.
    QPixmap source = pixmap;
    // The source may itself be a device-pixel raster from another ratio; work in
    // its own pixels so the scale lands on exactly `device`.
    source.setDevicePixelRatio(1.0);
    QPixmap scaled =
        source.scaled(device, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    const int cropX = qMax(0, (scaled.width() - device.width()) / 2);
    const int cropY = qMax(0, (scaled.height() - device.height()) / 2);
    scaled.setDevicePixelRatio(ratio);

    // Tagged target: the painter below is logical-coordinate only, so the
    // radius, the shade and the mask stay in the caller's units while the
    // raster behind them is `ratio` times larger.
    QPixmap out = gui::pixmapForDevice(logical, ratio);
    // Fill with solid black first: drawPixmap with negative offsets (below) does
    // not guarantee full coverage, leaving garbage from prior allocations visible
    // as horizontal striping until a full repaint (e.g. resize) clears it.
    out.fill(Qt::black);
    QPainter painter(&out);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    // Draw the centred region directly at its negative offset: the target box
    // is exactly `device`, so the parts outside it are discarded and the second
    // allocation of scaled.copy() is pure cost. Both the crop and the offset
    // divide by the ratio, which keeps the blit one source pixel per device
    // pixel.
    painter.drawPixmap(QPointF(-qreal(cropX) / ratio, -qreal(cropY) / ratio), scaled);
    if (shade && currentAppearance().coverGradient) {
        // A bottom shade keeps a bright poster from swallowing the card's own
        // controls; the top stays untouched.
        QLinearGradient gradient(0, logical.height() * 0.45, 0, logical.height());
        gradient.setColorAt(0.0, QColor(0, 0, 0, 0));
        gradient.setColorAt(1.0, QColor(0, 0, 0, 120));
        painter.fillRect(QRectF(0, logical.height() * 0.45, logical.width(),
                                logical.height() * 0.55),
                         gradient);
    }
    if (radius > 0) {
        // Inverted mask cleared with DestinationOut: compositing the rounded
        // shape in only inside the path left the corners outside it opaque,
        // while a clip would be one bit and step on the arc. Antialiased path
        // coverage keeps the corner ramp and the allocated frame's full extent.
        const QSizeF frame = out.deviceIndependentSize();
        const QRectF full(0, 0, frame.width(), frame.height());
        QPainterPath mask;
        mask.setFillRule(Qt::OddEvenFill);
        mask.addRect(full);
        mask.addRoundedRect(full, radius, radius);
        painter.setCompositionMode(QPainter::CompositionMode_DestinationOut);
        painter.fillPath(mask, Qt::black);
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    }
    return out;
}

void TitleCard::applyPixmap(const QPixmap& pixmap) {
    if (pixmap.isNull()) {
        return;
    }
    poster_->setText(QString());
    poster_->setPixmap(pixmap);
}

void TitleCard::applyStatus(const QString& status) {
    // Retained untranslated: a language switch re-derives the label from it.
    statusRaw_ = status;
    const bool live = status == QLatin1String("ongoing");
    if (live != statusLive_) {
        statusLive_ = live;
        status_->setObjectName(live ? QStringLiteral("carouselStatusLive")
                                    : QStringLiteral("carouselStatus"));
        // Re-polish: the object name selects the QSS rule, and Qt caches the
        // style against the old name until the widget is told to look again.
        // applyDetail replays the list status for every card on the page, so
        // this pair is only worth paying when the role actually flips.
        status_->style()->unpolish(status_);
        status_->style()->polish(status_);
    }
    const QString text = labels::status(status);
    status_->setText(text.isEmpty() ? QString::fromLatin1(kPending) : text);
}

void TitleCard::applyEpisodes(int aired, int planned) {
    aired_ = aired;
    if (planned > 0) {
        planned_ = planned;
    }
    const QString text = episodeCountText(aired, planned_);
    episodes_->setText(text.isEmpty() ? QString::fromLatin1(kPending) : text);
}

void TitleCard::applySearchHighlight(const QString& query, const backend::Titles& titles) {
    if (title_ == nullptr) {
        return;
    }
    QFont titleFont = title_->font();
    titleFont.setPixelSize(13);
    titleFont.setWeight(QFont::DemiBold);
    const int titleMaxWidth = theme::kHomeCardWidth - theme::kSpacingMedium -
                              theme::kHomePosterWidth - theme::kSpacingMedium -
                              theme::kSpacingLarge - 8;
    const QString plain = elideTwoLines(rawTitle_, titleFont, titleMaxWidth);

    const QString needle = query.trimmed();
    if (needle.isEmpty()) {
        title_->setTextFormat(Qt::PlainText);
        title_->setText(plain);
        if (altTitle_ != nullptr) altTitle_->hide();
        setToolTip(rawTitle_);
        return;
    }
    const qsizetype pos = plain.indexOf(needle, 0, Qt::CaseInsensitive);
    if (pos >= 0) {
        const QString before = plain.left(pos).toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>"));
        const QString match = plain.mid(pos, needle.size()).toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>"));
        const QString after = plain.mid(pos + needle.size()).toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>"));
        title_->setTextFormat(Qt::RichText);
        title_->setText(QStringLiteral("%1<span style=\"background:rgba(255,210,77,0.28);border-radius:3px;\">%2</span>%3")
                            .arg(before, match, after));
    } else {
        title_->setTextFormat(Qt::PlainText);
        title_->setText(plain);
    }
    if (altTitle_ != nullptr) {
        QString matchedAlt;
        if (rawTitle_.indexOf(needle, 0, Qt::CaseInsensitive) < 0) {
            for (const QString& candidate : {titles.ua, titles.english, titles.original}) {
                if (!candidate.trimmed().isEmpty() && candidate != rawTitle_ &&
                    candidate.indexOf(needle, 0, Qt::CaseInsensitive) >= 0) {
                    matchedAlt = candidate.trimmed();
                    break;
                }
            }
        }
        if (!matchedAlt.isEmpty()) {
            const QFontMetrics fm(altTitle_->font());
            const int maxW = theme::kHomeCardWidth - theme::kSpacingMedium * 2 -
                             theme::kHomePosterWidth - theme::kSpacingLarge - 8;
            altTitle_->setText(fm.elidedText(matchedAlt, Qt::ElideRight, qMax(80, maxW)));
            altTitle_->show();
            setToolTip(rawTitle_ + QStringLiteral("\n") + matchedAlt);
        } else {
            altTitle_->hide();
            setToolTip(rawTitle_);
        }
    }
}

void TitleCard::applyDetail(const backend::AnimeDetail& detail) {
    applyStatus(detail.status);
    aired_ = detail.episodes.aired;
    localized_ = detail.episodes.localized;
    planned_ = detail.episodes.total.value_or(0);
    hasDetailStats_ = true;
    if (detail.scores.mal.has_value() && *detail.scores.mal > 0.0) {
        score_ = *detail.scores.mal;
    } else if (detail.scores.mikai.has_value() && *detail.scores.mikai > 0.0) {
        score_ = *detail.scores.mikai;
    } else if (detail.scores.hikka.has_value() && *detail.scores.hikka > 0.0) {
        score_ = *detail.scores.hikka;
    }
    if (score_ > 0.0 && scoreLabel_ != nullptr) {
        scoreLabel_->setText(QStringLiteral("★ %1").arg(score_, 0, 'f', 1));
        scoreLabel_->show();
    }
    if (meta_ != nullptr && meta_->isHidden()) {
        QStringList metaParts;
        if (!detail.format.trimmed().isEmpty()) {
            metaParts << detail.format.trimmed().toUpper();
        }
        if (year_ > 0) {
            metaParts << QString::number(year_);
        }
        if (!metaParts.isEmpty()) {
            meta_->setText(metaParts.join(QStringLiteral(" · ")));
            meta_->show();
        }
    }
    retranslateUi();
}

}  // namespace mikai::app
