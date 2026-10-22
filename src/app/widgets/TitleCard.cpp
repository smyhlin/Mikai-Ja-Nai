#include "widgets/TitleCard.hpp"

#include "Labels.hpp"
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
#include <QPixmap>
#include <QPushButton>
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
        return QCoreApplication::translate("TitleCard", "%1 / %n episode(s)", nullptr, planned)
            .arg(aired);
    }
    const int count = aired > 0 ? aired : planned;
    if (count <= 0) {
        return {};
    }
    return QCoreApplication::translate("TitleCard", "%n episode(s)", nullptr, count);
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

    setObjectName(QStringLiteral("animeCard"));
    setFixedSize(theme::kHomeCardWidth, theme::kHomeCardHeight);
    setToolTip(backend::displayName(summary.titles));
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

    const QString fullTitle = backend::displayName(summary.titles);
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
    episodes_ = makeLabel(QStringLiteral("carouselEpisodes"), QString());
    episodes_->setWordWrap(true);
    episodes_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    // The title box is always two lines tall, so a one-line title would leave
    // the status and the count floating halfway down the card against a
    // two-line one. Top-aligning the stack puts every card's facts on the
    // same line whichever way the title happens to wrap.
    text->addWidget(title_, 0, Qt::AlignTop);
    text->addWidget(status_, 0, Qt::AlignTop);
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

QPixmap TitleCard::rounded(const QPixmap& pixmap, const QSize& target) {
    if (pixmap.isNull() || target.isEmpty()) {
        return {};
    }
    QPixmap scaled =
        pixmap.scaled(target, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    const int cropX = qMax(0, (scaled.width() - target.width()) / 2);
    const int cropY = qMax(0, (scaled.height() - target.height()) / 2);

    // Clip to rounded corners matching the card's inner radius so artwork
    // sits seamlessly inside the container without square corners overflowing.
    QPixmap out(target);
    out.fill(Qt::transparent);
    QPainter painter(&out);
    painter.setRenderHint(QPainter::Antialiasing);
    QPainterPath path;
    path.addRoundedRect(QRectF(0, 0, target.width(), target.height()), theme::kRadiusSmall,
                        theme::kRadiusSmall);
    painter.setClipPath(path);
    // Draw the centred region directly at its negative offset: the clip is
    // exactly the target box, so the parts outside it are discarded and the
    // second allocation of scaled.copy() is pure cost.
    painter.drawPixmap(-cropX, -cropY, scaled);
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

void TitleCard::applyDetail(const backend::AnimeDetail& detail) {
    applyStatus(detail.status);
    aired_ = detail.episodes.aired;
    localized_ = detail.episodes.localized;
    planned_ = detail.episodes.total.value_or(0);
    hasDetailStats_ = true;
    retranslateUi();
}

}  // namespace mikai::app
