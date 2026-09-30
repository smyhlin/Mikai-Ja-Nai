#include "widgets/TitleCard.hpp"

#include "Labels.hpp"
#include "theme/Metrics.hpp"

#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPushButton>
#include <QStyle>
#include <QVBoxLayout>

namespace mikai::app {
namespace {

const char* kPending = "…";

QString episodeCountText(int aired, int planned) {
    if (aired > 0 && planned > 0) {
        return QStringLiteral("%1 / %2 серій").arg(aired).arg(planned);
    }
    if (aired > 0) {
        return QStringLiteral("%1 серій").arg(aired);
    }
    return planned > 0 ? QStringLiteral("%1 серій").arg(planned) : QString();
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

    poster_ = makeLabel(QStringLiteral("carouselPoster"), tr("Постер недоступний"));
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

    title_ = makeLabel(QStringLiteral("carouselTitle"), backend::displayName(summary.titles));
    title_->setWordWrap(true);
    title_->setFixedHeight(title_->fontMetrics().height() * 2);
    // Expanding, not Preferred. A word-wrapped label keeps its sizeHint width,
    // so the column beside an 80px poster left the title wrapping inside a
    // 57px sliver while the rest of the card sat empty.
    title_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    status_ = makeLabel(QStringLiteral("carouselStatus"), QString());
    status_->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    episodes_ = makeLabel(QStringLiteral("carouselEpisodes"), QString());
    episodes_->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);

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

QLabel* TitleCard::makeLabel(const QString& objectName, const QString& text) {
    auto* label = new QLabel(text, this);
    label->setObjectName(objectName);
    // The card is the button; the labels are decoration and must not eat clicks.
    label->setAttribute(Qt::WA_TransparentForMouseEvents);
    return label;
}

void TitleCard::applyPixmap(const QPixmap& pixmap) {
    if (pixmap.isNull()) {
        return;
    }
    const QSize targetSize = poster_->size();
    QPixmap scaled = pixmap.scaled(targetSize, Qt::KeepAspectRatioByExpanding,
                                   Qt::SmoothTransformation);
    const int cropX = qMax(0, (scaled.width() - targetSize.width()) / 2);
    const int cropY = qMax(0, (scaled.height() - targetSize.height()) / 2);

    // Clip to rounded corners matching the card's inner radius so artwork
    // sits seamlessly inside the container without square corners overflowing.
    QPixmap rounded(targetSize);
    rounded.fill(Qt::transparent);
    QPainter painter(&rounded);
    painter.setRenderHint(QPainter::Antialiasing);
    QPainterPath path;
    path.addRoundedRect(QRectF(0, 0, targetSize.width(), targetSize.height()),
                        theme::kRadiusSmall, theme::kRadiusSmall);
    painter.setClipPath(path);
    // Draw the centred region directly at its negative offset: the clip is
    // exactly the target box, so the parts outside it are discarded and the
    // second allocation of scaled.copy() is pure cost.
    painter.drawPixmap(-cropX, -cropY, scaled);

    poster_->setText(QString());
    poster_->setPixmap(rounded);
}

void TitleCard::applyStatus(const QString& status) {
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
    if (planned > 0) {
        planned_ = planned;
    }
    const QString text = episodeCountText(aired, planned_);
    episodes_->setText(text.isEmpty() ? QString::fromLatin1(kPending) : text);
}

void TitleCard::applyDetail(const backend::AnimeDetail& detail) {
    applyStatus(detail.status);
    applyEpisodes(detail.episodes.aired, detail.episodes.total.value_or(0));
}

}  // namespace mikai::app
