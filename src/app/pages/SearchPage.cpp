#include "pages/SearchPage.hpp"

#include "Clipboard.hpp"
#include "Labels.hpp"
#include "state/Feeds.hpp"
#include "theme/Icons.hpp"
#include "theme/Metrics.hpp"
#include "widgets/RangePager.hpp"
#include "widgets/RateLimitLabel.hpp"
#include "widgets/TitleCard.hpp"
#include "workers/Runner.hpp"
#include "ShellWidgets.hpp"

#include <backend/MikaiClient.hpp>

#include <QBuffer>
#include <QDateTime>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QImageReader>
#include <QJsonArray>
#include <QEasingCurve>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPixmap>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QTabBar>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>

namespace mikai::app {
namespace {

const QString kRecentKey = QStringLiteral("recent");
const char* kRecentSettingsKey = "home/recentTitles";

bool isRemoteUrl(const QString& value) {
    return value.startsWith(QLatin1String("https://")) ||
           value.startsWith(QLatin1String("http://"));
}

/// "13.09 · 21:04", or "21:04" when only the time is wanted. Empty when the
/// timestamp does not parse: a feed row is better short than wrong.
QString formatFeedTime(const QString& timestamp, bool timeOnly = false) {
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

/// The biggest box anything on the page draws a poster into: the home card
/// poster. Search rows (56x78) and feed posters (34x48) are smaller, so
/// decoding down to this leaves no consumer upscaling.
const QSize kThumbDecodeSize(theme::kHomePosterWidth, theme::kHomePosterHeight);

QPixmap pixmapFrom(const QByteArray& data, const QSize& target) {
    // A QImageReader built straight from the bytes would read them as a
    // filename, not as an image: the reader needs a device to pull from.
    QBuffer buffer;
    buffer.setData(data);
    if (!buffer.open(QIODevice::ReadOnly)) {
        return {};
    }
    QImageReader reader(&buffer);
    // Scaling during the decode is far cheaper than decoding a full cover and
    // then throwing nearly all of it away, and the two agree on the result to
    // within a rounding step. Artwork already at or below the display rect is
    // decoded whole: scaling it up first would only cost time.
    const QSize source = reader.size();
    if (source.isValid() && !target.isEmpty() &&
        (source.width() > target.width() || source.height() > target.height())) {
        reader.setScaledSize(source.scaled(target, Qt::KeepAspectRatioByExpanding));
    }
    const QImage image = reader.read();
    if (image.isNull()) {
        return {};
    }
    return QPixmap::fromImage(image);
}

/// Enable a carousel's arrows only where there is somewhere to scroll to.
void syncArrows(QScrollBar* bar, QPushButton* previous, QPushButton* next) {
    previous->setEnabled(bar->value() > bar->minimum());
    next->setEnabled(bar->value() < bar->maximum());
}

}  // namespace

SearchPage::SearchPage(Runner& runner, backend::MikaiClient& client, QWidget* parent)
    : QWidget(parent), runner_(runner), client_(client) {
    pageSize_ = theme::kSearchResultsPerPage;
    for (const QString& key : feeds::all()) {
        feedSeq_.insert(key, 0);
    }
    QSettings settings;
    enabledFeeds_ = feeds::enabled(settings.value(feeds::settingsKey()).toString());
    recentItems_ = readRecentTitles();

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(theme::kSpacingLarge, theme::kSpacingLarge, theme::kSpacingLarge,
                             theme::kSpacingLarge);
    root->setSpacing(theme::kSpacingMedium);

    auto* topRow = new QHBoxLayout();
    auto* heading = new QLabel(tr("Mikai"), this);
    heading->setObjectName(QStringLiteral("heading"));
    topRow->addWidget(heading);
    topRow->addStretch();
    auto* mediaButton = new QPushButton(tr("Open video"), this);
    mediaButton->setObjectName(QStringLiteral("secondary"));
    mediaButton->setAccessibleName(tr("Open video"));
    auto* mediaMenu = new QMenu(mediaButton);
    mediaMenu->setObjectName(QStringLiteral("openMediaMenu"));
    auto* fileAction = mediaMenu->addAction(tr("Video file…"));
    fileAction->setObjectName(QStringLiteral("openVideoFile"));
    connect(fileAction, &QAction::triggered, this, &SearchPage::mediaFileRequested);
    auto* urlAction = mediaMenu->addAction(tr("Video URL…"));
    urlAction->setObjectName(QStringLiteral("openVideoUrl"));
    connect(urlAction, &QAction::triggered, this, &SearchPage::mediaUrlRequested);
    mediaButton->setMenu(mediaMenu);
    topRow->addWidget(mediaButton);
    rateLabel_ = new RateLimitLabel(client_, this);
    topRow->addWidget(rateLabel_);

    auto* keysButton = new QPushButton(this);
    keysButton->setObjectName(QStringLiteral("settingsChip"));
    keysButton->setIcon(theme::keyboardIcon());
    keysButton->setIconSize(QSize(theme::kIconSize, theme::kIconSize));
    keysButton->setToolTip(tr("Keyboard shortcuts (?)"));
    keysButton->setAccessibleName(tr("Keyboard shortcuts"));
    connect(keysButton, &QPushButton::clicked, this, [this, keysButton] {
        if (keyHints_ == nullptr) {
            keyHints_ = new mplayer::shell::KeyHints(this);
        }
        keyHints_->popupNear(keysButton->mapToGlobal(QPoint(keysButton->width() / 2, keysButton->height())));
    });
    topRow->addWidget(keysButton);

    auto* settingsButton = new QPushButton(this);
    settingsButton->setObjectName(QStringLiteral("settingsChip"));
    settingsButton->setIcon(theme::gearIcon());
    settingsButton->setIconSize(QSize(theme::kIconSize, theme::kIconSize));
    settingsButton->setToolTip(tr("API token settings"));
    settingsButton->setAccessibleName(tr("API token settings"));
    connect(settingsButton, &QPushButton::clicked, this, &SearchPage::settingsRequested);
    topRow->addWidget(settingsButton);
    root->addLayout(topRow);

    auto* searchRow = new QHBoxLayout();
    query_ = new QLineEdit(this);
    query_->setPlaceholderText(tr("Search by title — Enter or the button"));
    query_->setClearButtonEnabled(true);
    auto* searchButton = new QPushButton(tr("Search"), this);
    searchButton->setObjectName(QStringLiteral("primary"));
    connect(query_, &QLineEdit::returnPressed, this, &SearchPage::search);
    connect(searchButton, &QPushButton::clicked, this, &SearchPage::search);
    connect(query_, &QLineEdit::textChanged, this, [this](const QString& text) {
        if (text.trimmed().isEmpty() && !browseMode_) {
            loadOngoing();
        }
    });
    searchRow->addWidget(query_);
    searchRow->addWidget(searchButton);
    root->addLayout(searchRow);

    resultsHeading_ = new QLabel(tr("Currently airing · sorted by newest episode"), this);
    resultsHeading_->setObjectName(QStringLiteral("sectionHeading"));
    root->addWidget(resultsHeading_);

    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("status"));
    root->addWidget(status_);

    auto* discovery = new QWidget(this);
    auto* discoveryLayout = new QVBoxLayout(discovery);
    discoveryLayout->setContentsMargins(0, 0, 0, 0);
    discoveryLayout->setSpacing(theme::kSpacingMedium);
    CarouselSection* recent = buildCarousel(tr("Recently opened"), kRecentKey);
    CarouselSection* ongoing = buildCarousel(tr("Currently airing"), QStringLiteral("ongoing"));
    CarouselSection* popular = buildCarousel(tr("Popular on Mikai"), QStringLiteral("popular"));
    FeedSection* updates = buildFeedSection(tr("Latest updates"), QStringLiteral("updates"));
    FeedSection* schedule = buildFeedSection(tr("Release schedule"), QStringLiteral("schedule"));
    discoveryLayout->addWidget(recent->section);
    discoveryLayout->addWidget(ongoing->section);
    discoveryLayout->addWidget(popular->section);
    discoveryLayout->addWidget(updates->section);
    discoveryLayout->addWidget(schedule->section);
    // Carousels and list feeds share one key space but not one widget set, so
    // each block is shown only if it is the kind of block this key names.
    for (const QString& key : feeds::all()) {
        const bool on = enabledFeeds_.contains(key);
        if (CarouselSection* carousel = carouselSection(key)) {
            carousel->section->setVisible(on);
        }
        if (FeedSection* feed = feedSection(key)) {
            feed->section->setVisible(on);
        }
    }
    discoveryLayout->addStretch();

    discoveryScroll_ = new QScrollArea(this);
    discoveryScroll_->setObjectName(QStringLiteral("discoveryScroll"));
    discoveryScroll_->setWidgetResizable(true);
    discoveryScroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    discoveryScroll_->setFrameShape(QFrame::NoFrame);
    discoveryScroll_->setWidget(discovery);
    root->addWidget(discoveryScroll_, 1);

    formatBar_ = new QTabBar(this);
    formatBar_->setExpanding(false);
    formatBar_->setDrawBase(false);
    formatBar_->hide();
    connect(formatBar_, &QTabBar::currentChanged, this, [this](int) { applyFormatFilter(); });
    root->addWidget(formatBar_);

    results_ = new QListWidget(this);
    results_->setIconSize(QSize(theme::kSearchPosterWidth, theme::kSearchPosterHeight));
    results_->setTextElideMode(Qt::ElideRight);
    results_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    results_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(results_, &QListWidget::itemClicked, this, &SearchPage::openItem);
    connect(results_, &QListWidget::itemActivated, this, &SearchPage::openItem);
    connect(results_, &QListWidget::customContextMenuRequested, this,
            &SearchPage::resultsContextMenu);
    results_->hide();
    root->addWidget(results_, 1);

    pager_ = new RangePager(theme::kSearchResultsPerPage, this);
    connect(pager_, &RangePager::pageRequested, this, &SearchPage::goToPage);
    pager_->hide();
    root->addWidget(pager_);

    healTimer_ = new QTimer(this);
    healTimer_->setSingleShot(true);
    healTimer_->setInterval(200);
    connect(healTimer_, &QTimer::timeout, this, &SearchPage::flushRecentPosters);

    // The same coalescing for thumbs, on a zero interval: a feed's posters land
    // in one burst, and one pass over the whole batch beats one pass per image.
    thumbTimer_ = new QTimer(this);
    thumbTimer_->setSingleShot(true);
    thumbTimer_->setInterval(0);
    connect(thumbTimer_, &QTimer::timeout, this, &SearchPage::flushThumbs);

    QTimer::singleShot(0, this, &SearchPage::loadOngoing);
    if (!recentItems_.isEmpty()) {
        showCarousel(kRecentKey, recentItems_);
    }
}

SearchPage::~SearchPage() = default;

SearchPage::CarouselSection* SearchPage::buildCarousel(const QString& heading,
                                                       const QString& key) {
    auto* section = new CarouselSection();
    section->section = new QWidget(this);
    auto* layout = new QVBoxLayout(section->section);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(theme::kSpacingSmall - theme::kSpacingXs);

    auto* header = new QHBoxLayout();
    auto* title = new QLabel(heading, section->section);
    title->setObjectName(QStringLiteral("sectionHeading"));
    header->addWidget(title);
    header->addStretch();
    section->previous = new QPushButton(QStringLiteral("←"), section->section);
    section->next = new QPushButton(QStringLiteral("→"), section->section);
    for (QPushButton* button : {section->previous, section->next}) {
        button->setObjectName(QStringLiteral("carouselControl"));
        header->addWidget(button);
    }
    layout->addLayout(header);

    auto* stateRow = new QHBoxLayout();
    section->state = new QLabel(tr("Collection not loaded yet"), section->section);
    section->state->setObjectName(QStringLiteral("muted"));
    section->state->setWordWrap(true);
    section->retry = new QPushButton(tr("Retry"), section->section);
    section->retry->hide();
    connect(section->retry, &QPushButton::clicked, this,
            [this, key] { requestRecommendation(key); });
    stateRow->addWidget(section->state, 1);
    stateRow->addWidget(section->retry);
    layout->addLayout(stateRow);

    section->scroll = new QScrollArea(section->section);
    section->scroll->setObjectName(QStringLiteral("animeCarousel"));
    section->scroll->setWidgetResizable(false);
    section->scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    section->scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    section->scroll->setFixedHeight(theme::kHomeCardHeight + theme::kSpacingMedium);
    auto* track = new QWidget(section->scroll);
    track->setObjectName(QStringLiteral("animeCarouselTrack"));
    section->cards = new QHBoxLayout(track);
    section->cards->setContentsMargins(2, 2, 2, 2);
    section->cards->setSpacing(theme::kCarouselCardSpacing);
    section->scroll->setWidget(track);
    layout->addWidget(section->scroll);

    QScrollBar* bar = section->scroll->horizontalScrollBar();
    QPushButton* previous = section->previous;
    QPushButton* next = section->next;
    previous->setToolTip(tr("Previous page (or Alt + mouse wheel)"));
    next->setToolTip(tr("Next page (or Alt + mouse wheel)"));
    connect(previous, &QPushButton::clicked, this,
            [this, bar] { smoothScrollCarousel(bar, -theme::kHomeCarouselScrollStep); });
    connect(next, &QPushButton::clicked, this,
            [this, bar] { smoothScrollCarousel(bar, theme::kHomeCarouselScrollStep); });
    // Both edges of the track disable the arrow that would go nowhere.
    connect(bar, &QScrollBar::rangeChanged, this,
            [bar, previous, next](int, int) { syncArrows(bar, previous, next); });
    connect(bar, &QScrollBar::valueChanged, this,
            [bar, previous, next](int) { syncArrows(bar, previous, next); });
    syncArrows(bar, previous, next);

    // Mouse wheel only scrolls the carousel horizontally when Alt is held;
    // normal vertical scrolling passes through to the discovery scroll area.
    section->scroll->viewport()->installEventFilter(this);
    section->scroll->hide();

    carousels_.insert(key, section);
    return section;
}

void SearchPage::smoothScrollCarousel(QScrollBar* bar, int delta) {
    if (bar == nullptr) {
        return;
    }
    const int start = bar->value();
    const int target = qBound(bar->minimum(), start + delta, bar->maximum());
    if (start == target) {
        return;
    }
    auto* anim = new QPropertyAnimation(bar, "value", this);
    anim->setDuration(240);
    anim->setEasingCurve(QEasingCurve::OutCubic);
    anim->setStartValue(start);
    anim->setEndValue(target);
    anim->start(QAbstractAnimation::DeleteWhenStopped);
}

bool SearchPage::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::Wheel) {
        for (const auto* section : carousels_) {
            if (section != nullptr && section->scroll != nullptr &&
                watched == section->scroll->viewport()) {
                auto* we = static_cast<QWheelEvent*>(event);
                const bool altHeld = we->modifiers().testFlag(Qt::AltModifier);
                const int hDelta = we->angleDelta().x();
                const int vDelta = we->angleDelta().y();

                // Horizontal wheeling occurs only when Alt is held, or when
                // hardware emits a true horizontal scroll delta.
                if (altHeld && vDelta != 0) {
                    QScrollBar* bar = section->scroll->horizontalScrollBar();
                    if (bar != nullptr) {
                        smoothScrollCarousel(bar, -vDelta * 2);
                        return true;
                    }
                } else if (hDelta != 0) {
                    QScrollBar* bar = section->scroll->horizontalScrollBar();
                    if (bar != nullptr) {
                        smoothScrollCarousel(bar, -hDelta * 2);
                        return true;
                    }
                }

                // Normal vertical wheel scroll must bubble up to the main
                // discovery scroll area so carousels do not hijack navigation.
                event->ignore();
                return false;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

SearchPage::FeedSection* SearchPage::buildFeedSection(const QString& heading,
                                                     const QString& key) {
    auto* section = new FeedSection();
    section->section = new QWidget(this);
    auto* layout = new QVBoxLayout(section->section);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(theme::kSpacingSmall - theme::kSpacingXs);

    auto* header = new QHBoxLayout();
    auto* title = new QLabel(heading, section->section);
    title->setObjectName(QStringLiteral("sectionHeading"));
    header->addWidget(title);
    header->addStretch();
    section->retry = new QPushButton(tr("Refresh"), section->section);
    section->retry->setObjectName(QStringLiteral("textAction"));
    connect(section->retry, &QPushButton::clicked, this, [this, key] {
        if (key == QLatin1String("updates")) {
            loadUpdates();
        } else {
            loadSchedule();
        }
    });
    header->addWidget(section->retry);
    layout->addLayout(header);

    section->state = new QLabel(tr("Loading…"), section->section);
    section->state->setObjectName(QStringLiteral("muted"));
    layout->addWidget(section->state);

    section->rowsHost = new QWidget(section->section);
    section->rows = new QVBoxLayout(section->rowsHost);
    section->rows->setContentsMargins(0, 0, 0, 0);
    section->rows->setSpacing(theme::kSpacingXs);
    section->rowsHost->hide();
    layout->addWidget(section->rowsHost);

    feeds_.insert(key, section);
    return section;
}

SearchPage::CarouselSection* SearchPage::carouselSection(const QString& key) const {
    return carousels_.value(key, nullptr);
}

SearchPage::FeedSection* SearchPage::feedSection(const QString& key) const {
    return feeds_.value(key, nullptr);
}

QList<backend::AnimeSummary> SearchPage::readRecentTitles() {
    QSettings settings;
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(
        settings.value(QLatin1String(kRecentSettingsKey)).toString().toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !document.isArray()) {
        return {};
    }
    QList<backend::AnimeSummary> items;
    for (const QJsonValue& value : document.array()) {
        if (!value.isObject()) {
            continue;
        }
        const QJsonObject record = value.toObject();
        const QString ref = record.value(QStringLiteral("ref")).toString();
        if (ref.isEmpty()) {
            continue;
        }
        backend::AnimeSummary summary;
        summary.ref = ref;
        summary.titles.ua = record.value(QStringLiteral("title")).toString();
        const QString poster = record.value(QStringLiteral("poster")).toString();
        if (isRemoteUrl(poster)) {
            summary.poster.big.webp = poster;
        }
        const QString thumb = record.value(QStringLiteral("thumb")).toString();
        if (isRemoteUrl(thumb)) {
            summary.poster.small.webp = thumb;
        }
        items.append(summary);
        if (items.size() >= theme::kHomeRecentMaxTitles) {
            break;
        }
    }
    return items;
}

void SearchPage::persistRecentTitles() {
    QJsonArray records;
    for (const backend::AnimeSummary& recent : recentItems_) {
        QJsonObject record;
        record.insert(QStringLiteral("ref"), recent.ref);
        record.insert(QStringLiteral("title"), backend::displayName(recent.titles));
        const QString poster = backend::posterUrl(recent);
        record.insert(QStringLiteral("poster"), isRemoteUrl(poster) ? poster : QString());
        const QString thumb = backend::thumbUrl(recent);
        record.insert(QStringLiteral("thumb"), isRemoteUrl(thumb) ? thumb : QString());
        records.append(record);
    }
    QSettings settings;
    settings.setValue(QLatin1String(kRecentSettingsKey),
                      QString::fromUtf8(QJsonDocument(records).toJson(QJsonDocument::Compact)));
}

void SearchPage::recordRecentTitle(const QString& ref, const QString& title,
                                   const QString& poster) {
    if (ref.isEmpty()) {
        return;
    }
    for (int index = recentItems_.size() - 1; index >= 0; --index) {
        if (recentItems_.at(index).ref == ref) {
            recentItems_.removeAt(index);
        }
    }
    backend::AnimeSummary summary;
    summary.ref = ref;
    summary.titles.ua = title;
    if (isRemoteUrl(poster)) {
        summary.poster.big.webp = poster;
    }
    recentItems_.prepend(summary);
    while (recentItems_.size() > theme::kHomeRecentMaxTitles) {
        recentItems_.removeLast();
    }
    persistRecentTitles();
    carouselSection(kRecentKey)->section->show();
    showCarousel(kRecentKey, recentItems_);
}

void SearchPage::showCarousel(const QString& key, const QList<backend::AnimeSummary>& items) {
    CarouselSection* section = carouselSection(key);
    if (section == nullptr) {
        return;
    }
    section->section->show();
    section->retry->hide();
    QList<backend::AnimeSummary> shown = items;
    while (shown.size() > theme::kHomeCarouselMaxTitles) {
        shown.removeLast();
    }
    carouselEntries_.insert(key, shown);
    if (shown.isEmpty()) {
        section->state->setText(tr("No titles in this collection yet."));
        section->scroll->hide();
        return;
    }
    section->state->setText(tr("%n title(s)", nullptr, static_cast<int>(shown.size())));

    while (section->cards->count() > 0) {
        QLayoutItem* item = section->cards->takeAt(0);
        if (item->widget() != nullptr) {
            item->widget()->deleteLater();
        }
    }
    QList<TitleCard*> cards;
    for (const backend::AnimeSummary& anime : shown) {
        auto* card = new TitleCard(anime, section->section);
        connect(card, &TitleCard::openRequested, this, &SearchPage::openRequested);
        connect(card, &TitleCard::menuRequested, this, &SearchPage::animeCardMenu);
        section->cards->addWidget(card);
        cards.append(card);
    }
    // Registered before the thumbs are asked for: a url already in the cache
    // paints synchronously, and it can only find these cards once they are
    // the ones carouselWidgets_ points at.
    // The cards this call is about to drop are on their way to deleteLater, so
    // they have to leave the image index with them; a thumb landing afterwards
    // must not reach a freed card.
    for (TitleCard* card : carouselWidgets_.value(key)) {
        const auto indexed = cardsByImageKey_.find(card->imageKey());
        if (indexed != cardsByImageKey_.end()) {
            indexed.value().removeAll(card);
            if (indexed.value().isEmpty()) {
                cardsByImageKey_.erase(indexed);
            }
        }
        const auto byRef = cardsByRef_.find(card->ref());
        if (byRef != cardsByRef_.end()) {
            byRef.value().removeAll(card);
            if (byRef.value().isEmpty()) {
                cardsByRef_.erase(byRef);
            }
        }
    }
    carouselWidgets_.insert(key, cards);
    for (TitleCard* card : cards) {
        cardsByImageKey_[card->imageKey()].append(card);
        cardsByRef_[card->ref()].append(card);
    }
    // One task per card: the image pool paints a whole feed at once instead
    // of waiting for the slowest poster in it.
    for (TitleCard* card : cards) {
        requestThumb(card->imageKey());
    }

    QWidget* track = section->scroll->widget();
    section->cards->activate();
    const QMargins trackMargins = section->cards->contentsMargins();
    const int width = trackMargins.left() + trackMargins.right() +
                      shown.size() * theme::kHomeCardWidth +
                      qMax(0, shown.size() - 1) * section->cards->spacing();
    track->setFixedSize(width, theme::kHomeCardHeight + theme::kSpacingXs);
    section->scroll->show();

    for (const backend::AnimeSummary& anime : shown) {
        requestCardDetail(anime.ref);
    }
}

void SearchPage::requestCardDetail(const QString& ref) {
    // A list response carries only the planned episode count, so every card
    // needs one detail request for its aired fraction. Each goes out as its own
    // task, so the pool works through a whole feed at the client's advertised
    // rate. A recent card persists a bare poster URL with no other artwork
    // source, so this same lookup is also what heals a rotted one.
    if (ref.isEmpty() || detailLookups_.contains(ref)) {
        const auto cached = cardDetails_.constFind(ref);
        if (ref.isEmpty()) {
            return;
        }
        if (cached != cardDetails_.constEnd()) {
            applyCardDetail(ref, cached.value());
        }
        return;
    }
    detailLookups_.insert(ref);
    runner_.runBulk(
        [this, ref] { return QVariant::fromValue(client_.getAnime(ref)); },
        [this, ref](const QVariant& result) {
            applyCardDetail(ref, result.value<backend::AnimeDetail>());
        },
        [](const QString&) {});
}

void SearchPage::applyCardDetail(const QString& ref, const backend::AnimeDetail& detail) {
    cardDetails_.insert(ref, detail);
    queueRecentPosterHeal(ref, detail);
    for (TitleCard* card : cardsByRef_.value(ref)) {
        card->applyDetail(detail);
    }
}

void SearchPage::queueRecentPosterHeal(const QString& ref,
                                       const backend::AnimeDetail& detail) {
    // A burst of detail results would otherwise re-render the recent feed once
    // per title; one timer collapses them into a single repaint.
    const QString cover = backend::coverUrl(detail);
    if (!isRemoteUrl(cover)) {
        return;
    }
    backend::PosterSet poster;
    poster.big.webp = cover;
    // A card keys on thumbUrl, i.e. poster.small. Left empty it falls back to
    // the big cover, so every healed recent card pulls full-size artwork.
    const QString small = detail.poster.small.webp.isEmpty() ? detail.poster.small.jpg
                                                             : detail.poster.small.webp;
    poster.small.webp = isRemoteUrl(small) ? small : cover;
    const QList<backend::AnimeSummary>& recent = carouselEntries_.value(kRecentKey);
    const bool isRecent = std::any_of(recent.cbegin(), recent.cend(),
                                      [&ref](const backend::AnimeSummary& item) {
                                          return item.ref == ref;
                                      });
    if (!isRecent) {
        return;
    }
    healedPosters_.insert(ref, poster);
    healTimer_->start();
}

void SearchPage::flushRecentPosters() {
    const QHash<QString, backend::PosterSet> healed = healedPosters_;
    healedPosters_.clear();
    if (healed.isEmpty()) {
        return;
    }
    QList<backend::AnimeSummary> refreshed = carouselEntries_.value(kRecentKey);
    // A heal that lands on the url already stored changes nothing, and a
    // rebuild for nothing is what blanks the carousel in the first place.
    bool changed = false;
    for (backend::AnimeSummary& item : refreshed) {
        const auto it = healed.constFind(item.ref);
        if (it == healed.constEnd() || item.poster.big.webp == it.value().big.webp) {
            continue;
        }
        item.poster = it.value();
        changed = true;
    }
    if (!changed) {
        return;
    }
    carouselEntries_.insert(kRecentKey, refreshed);
    recentItems_ = refreshed.mid(0, theme::kHomeRecentMaxTitles);
    persistRecentTitles();
    showCarousel(kRecentKey, recentItems_);
}

void SearchPage::refreshFeedSettings() {
    QSettings settings;
    const QSet<QString> previous = enabledFeeds_;
    enabledFeeds_ = feeds::enabled(settings.value(feeds::settingsKey()).toString());
    if (previous == enabledFeeds_) {
        return;
    }
    for (const QString& key : feeds::all()) {
        const bool on = enabledFeeds_.contains(key);
        // Bumping the sequence drops an in-flight result, so it belongs only
        // to a feed being switched off. Bumping a feed that stays on threw away
        // the answer to the request it was already waiting on and left the
        // block on its loading line with nothing left to finish it.
        if (previous.contains(key) && !on) {
            feedSeq_[key] += 1;
        }
        CarouselSection* carousel = carouselSection(key);
        FeedSection* feed = feedSection(key);
        if (carousel != nullptr) {
            carousel->section->setVisible(on);
        }
        if (feed != nullptr) {
            feed->section->setVisible(on);
        }
    }
    // A feed switched on has to load; one switched off is simply never asked.
    for (const QString& key : feeds::all()) {
        if (enabledFeeds_.contains(key) && !previous.contains(key)) {
            loadFeed(key);
        }
    }
}

void SearchPage::loadFeed(const QString& key) {
    if (key == QLatin1String("ongoing") || key == QLatin1String("popular")) {
        requestRecommendation(key);
    } else if (key == QLatin1String("updates")) {
        loadUpdates();
    } else if (key == QLatin1String("schedule")) {
        loadSchedule();
    }
}

void SearchPage::loadOngoing() {
    query_->clear();
    browseMode_ = true;
    showDiscovery();
    currentQuery_.clear();
    pageSize_ = theme::kSearchResultsPerPage;
    page_ = 1;
    pages_ = 1;
    total_ = 0;
    entries_.clear();
    results_->clear();
    pager_->hide();
    resultsHeading_->setText(tr("Recommendations"));
    status_->setText(
        tr("Click a card to open a title · search by title is available above"));
    for (const QString& key : feeds::all()) {
        loadFeed(key);
    }
}

void SearchPage::requestRecommendation(const QString& key) {
    CarouselSection* section = carouselSection(key);
    if (section == nullptr) {
        return;
    }
    feedSeq_[key] += 1;
    const int seq = feedSeq_[key];
    if (!enabledFeeds_.contains(key)) {
        return;
    }
    section->state->setText(tr("Loading collection…"));
    section->retry->hide();
    if (carouselEntries_.value(key).isEmpty()) {
        section->scroll->hide();
    }
    const bool popular = key == QLatin1String("popular");
    runner_.run(
        [this, popular] {
            return QVariant::fromValue(popular ? client_.browsePopular(1, theme::kHomeCarouselMaxTitles)
                                               : client_.browseOngoing(1, theme::kHomeCarouselMaxTitles));
        },
        [this, key, seq](const QVariant& result) {
            if (seq != feedSeq_[key]) {
                return;
            }
            rateLabel_->refresh();
            showCarousel(key, result.value<backend::SearchResult>().items);
        },
        [this, key, seq](const QString& message) {
            if (seq != feedSeq_[key]) {
                return;
            }
            rateLabel_->refresh();
            CarouselSection* current = carouselSection(key);
            current->state->setText(tr("Failed to load: %1").arg(message));
            current->retry->show();
        });
}

void SearchPage::loadUpdates() {
    if (!enabledFeeds_.contains(QStringLiteral("updates"))) {
        return;
    }
    FeedSection* section = feedSection(QStringLiteral("updates"));
    feedSeq_[QStringLiteral("updates")] += 1;
    const int seq = feedSeq_[QStringLiteral("updates")];
    section->state->setText(tr("Loading new episodes…"));
    section->retry->setEnabled(false);
    runner_.run(
        [this] { return QVariant::fromValue(client_.getUpdates(1, 12)); },
        [this, seq](const QVariant& result) {
            if (seq != feedSeq_[QStringLiteral("updates")]) {
                return;
            }
            rateLabel_->refresh();
            FeedSection* current = feedSection(QStringLiteral("updates"));
            current->retry->setEnabled(true);
            QList<QPair<backend::AnimeSummary, QString>> rows;
            const backend::UpdatesResult updates = result.value<backend::UpdatesResult>();
            for (int index = 0; index < updates.items.size() && index < theme::kHomeFeedMaxRows;
                 ++index) {
                const backend::AnimeUpdate& update = updates.items.at(index);
                if (update.anime.ref.isEmpty()) {
                    continue;
                }
                QString team = update.team_name;
                if (team.isEmpty()) {
                    QStringList names;
                    for (const backend::Team& entry : update.teams) {
                        if (!entry.name.isEmpty()) {
                            names << entry.name;
                        }
                    }
                    team = names.join(QLatin1String(", "));
                }
                const QString releaseKind =
                    update.kind == QLatin1String("voice")
                        ? tr("dub")
                        : (update.kind == QLatin1String("sub") ? tr("subs") : update.kind);
                const QString episode = labels::variant(update.episode);
                QStringList parts;
                parts << (episode.isEmpty() ? tr("New episode")
                                           : tr("Episode %1").arg(episode));
                if (!team.isEmpty()) {
                    parts << team;
                }
                if (!releaseKind.isEmpty()) {
                    parts << releaseKind;
                }
                if (!update.provider.isEmpty()) {
                    QString capitalized = update.provider;
                    capitalized[0] = capitalized.at(0).toUpper();
                    parts << capitalized;
                }
                const QString when = formatFeedTime(update.published_at);
                if (!when.isEmpty()) {
                    parts << when;
                }
                rows.append({update.anime, parts.join(QStringLiteral(" · "))});
            }
            showFeedRows(QStringLiteral("updates"), rows,
                         rows.isEmpty()
                             ? tr("No updates yet.")
                             : tr("%n recent update(s)", nullptr, static_cast<int>(rows.size())));
        },
        [this, seq](const QString& message) {
            if (seq != feedSeq_[QStringLiteral("updates")]) {
                return;
            }
            rateLabel_->refresh();
            FeedSection* current = feedSection(QStringLiteral("updates"));
            current->retry->setEnabled(true);
            current->state->setText(tr("Failed to load updates: %1").arg(message));
            current->rowsHost->hide();
        });
}

void SearchPage::loadSchedule() {
    if (!enabledFeeds_.contains(QStringLiteral("schedule"))) {
        return;
    }
    FeedSection* section = feedSection(QStringLiteral("schedule"));
    feedSeq_[QStringLiteral("schedule")] += 1;
    const int seq = feedSeq_[QStringLiteral("schedule")];
    section->state->setText(tr("Loading schedule…"));
    section->retry->setEnabled(false);
    runner_.run(
        [this] { return QVariant::fromValue(client_.getSchedule()); },
        [this, seq](const QVariant& result) {
            if (seq != feedSeq_[QStringLiteral("schedule")]) {
                return;
            }
            rateLabel_->refresh();
            FeedSection* current = feedSection(QStringLiteral("schedule"));
            current->retry->setEnabled(true);
            // Today first, then the rest of the week in order: the top of a
            // schedule is only useful if it starts with what airs next.
            static const char* kDayKeys[] = {"monday", "tuesday", "wednesday", "thursday",
                                             "friday",  "saturday", "sunday"};
            const QStringList kDayLabels = {tr("Mon"), tr("Tue"), tr("Wed"), tr("Thu"),
                                             tr("Fri"), tr("Sat"), tr("Sun")};
            const int today = QDate::currentDate().dayOfWeek() - 1;
            const backend::ScheduleResult schedule = result.value<backend::ScheduleResult>();
            QList<QPair<backend::AnimeSummary, QString>> rows;
            for (int offset = 0; offset < 7 && rows.size() < theme::kHomeFeedMaxRows; ++offset) {
                const int day = (today + offset) % 7;
                QVector<backend::AiringEntry> airing = schedule.days.value(QLatin1String(kDayKeys[day]));
                std::sort(airing.begin(), airing.end(),
                          [](const backend::AiringEntry& left, const backend::AiringEntry& right) {
                              return left.airing < right.airing;
                          });
                for (const backend::AiringEntry& entry : airing) {
                    if (entry.finished || entry.anime.ref.isEmpty()) {
                        continue;
                    }
                    QStringList parts;
                    parts << kDayLabels.at(day);
                    const QString when = formatFeedTime(entry.airing, true);
                    if (!when.isEmpty()) {
                        parts << when;
                    }
                    const QString episode = labels::variant(entry.episode);
                    if (!episode.isEmpty()) {
                        parts << tr("Episode %1").arg(episode);
                    }
                    rows.append({entry.anime, parts.join(QStringLiteral(" · "))});
                    if (rows.size() >= theme::kHomeFeedMaxRows) {
                        break;
                    }
                }
            }
            showFeedRows(QStringLiteral("schedule"), rows,
                         rows.isEmpty()
                             ? tr("No airings in the schedule yet.")
                             : tr("%n upcoming airing(s)", nullptr,
                                  static_cast<int>(rows.size())));
        },
        [this, seq](const QString& message) {
            if (seq != feedSeq_[QStringLiteral("schedule")]) {
                return;
            }
            rateLabel_->refresh();
            FeedSection* current = feedSection(QStringLiteral("schedule"));
            current->retry->setEnabled(true);
            current->state->setText(tr("Failed to load schedule: %1").arg(message));
            current->rowsHost->hide();
        });
}

void SearchPage::showFeedRows(
    const QString& key, const QList<QPair<backend::AnimeSummary, QString>>& entries,
    const QString& emptyText) {
    FeedSection* section = feedSection(key);
    while (section->rows->count() > 0) {
        QLayoutItem* item = section->rows->takeAt(0);
        if (item->widget() != nullptr) {
            item->widget()->deleteLater();
        }
    }
    feedPosterLabels_.insert(key, {});
    if (entries.isEmpty()) {
        section->state->setText(emptyText);
        section->state->show();
        section->rowsHost->hide();
        return;
    }
    section->state->setText(emptyText);
    section->state->hide();

    QVector<FeedPosterSlot> posters;
    for (const auto& entry : entries) {
        const backend::AnimeSummary& anime = entry.first;
        auto* row = new QWidget(section->rowsHost);
        row->setObjectName(QStringLiteral("homeFeedRow"));
        row->setContextMenuPolicy(Qt::CustomContextMenu);
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(theme::kSpacingSmall + 2, theme::kSpacingXs,
                                      theme::kSpacingSmall + 2, theme::kSpacingXs);
        rowLayout->setSpacing(theme::kSpacingMedium);
        const QString thumb = backend::thumbUrl(anime);
        if (!thumb.isEmpty()) {
            auto* poster = new QLabel(row);
            poster->setObjectName(QStringLiteral("homeFeedPoster"));
            poster->setFixedSize(theme::kHomeFeedPosterWidth, theme::kHomeFeedPosterHeight);
            poster->setAlignment(Qt::AlignCenter);
            poster->setAttribute(Qt::WA_TransparentForMouseEvents);
            posters.append({thumb, poster, {}});
            rowLayout->addWidget(poster, 0, Qt::AlignVCenter);
        }
        const QString name = backend::displayName(anime.titles);
        auto* title = new QPushButton(name, row);
        title->setObjectName(QStringLiteral("homeFeedTitle"));
        title->setToolTip(name);
        const QString ref = anime.ref;
        const QString posterUrl = backend::posterUrl(anime);
        connect(title, &QPushButton::clicked, this,
                [this, ref, posterUrl] { Q_EMIT openRequested(ref, posterUrl); });
        connect(row, &QWidget::customContextMenuRequested, this,
                [this, ref, row](const QPoint& position) {
                    animeCardMenu(ref, row->mapToGlobal(position));
                });
        auto* meta = new QLabel(entry.second, row);
        meta->setObjectName(QStringLiteral("homeFeedMeta"));
        meta->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        rowLayout->addWidget(title, 1);
        rowLayout->addWidget(meta);
        section->rows->addWidget(row);
    }
    section->rowsHost->show();
    feedPosterLabels_.insert(key, posters);
    for (const FeedPosterSlot& poster : posters) {
        requestThumb(poster.url);
    }
}

void SearchPage::fetchPage(int page) {
    status_->setText(browseMode_ ? tr("Loading collection…") : tr("Searching…"));
    runResultsRequest(page);
}

void SearchPage::runResultsRequest(int page) {
    requestSeq_ += 1;
    const int seq = requestSeq_;
    const int pageSize = pageSize_;
    const QString query = currentQuery_;
    const bool browse = browseMode_;
    runner_.run(
        [this, query, page, pageSize, browse] {
            const backend::SearchResult result = browse ? client_.browseOngoing(page, pageSize)
                                                        : client_.search(query, page, pageSize);
            return QVariant::fromValue(result);
        },
        [this, seq, page](const QVariant& result) {
            if (seq == requestSeq_) {
                onResults(result.value<backend::SearchResult>(), page);
            }
        },
        [this, seq](const QString& message) {
            if (seq == requestSeq_) {
                onError(message);
            }
        });
}

void SearchPage::search() {
    const QString query = query_->text().trimmed();
    if (query.isEmpty()) {
        loadOngoing();
        return;
    }
    browseMode_ = false;
    currentQuery_ = query;
    page_ = 1;
    pages_ = 1;
    total_ = 0;
    entries_.clear();
    results_->clear();
    pager_->hide();
    resultsHeading_->setText(tr("Search results"));
    showSearchResults();
    fetchPage(1);
}

void SearchPage::showDiscovery() {
    discoveryScroll_->show();
    results_->hide();
    formatBar_->hide();
    resultsHeading_->show();
    // Thumbs that landed while the feeds were hidden were left unpainted, so
    // the switch back is the moment to catch them up.
    applyFeedPosters();
}

void SearchPage::showSearchResults() {
    discoveryScroll_->hide();
    results_->show();
    resultsHeading_->show();
    applyThumbs();
}

void SearchPage::onResults(const backend::SearchResult& result, int requestedPage) {
    rateLabel_->refresh();
    page_ = result.page.value_or(requestedPage);
    entries_ = result.items;
    total_ = result.total.value_or(entries_.size());
    // The server counts pages at its own page size, so the count is ours to
    // make: asked for 55, it answers pages:15 for a 132-title result set.
    pages_ = qMax(1, (total_ + pageSize_ - 1) / pageSize_);
    status_->setText(entries_.isEmpty() ? tr("Nothing found")
                                        : tr("Found: %1").arg(total_));
    rebuildFormatBar();
    populateRows();
    updatePager();
}

void SearchPage::rebuildFormatBar() {
    static const QStringList kOrder = {QStringLiteral("tv"),    QStringLiteral("movie"),
                                       QStringLiteral("special"), QStringLiteral("ova"),
                                       QStringLiteral("ona"),   QStringLiteral("music"),
                                       QStringLiteral("other"),  QStringLiteral("unknown")};
    QHash<QString, int> counts;
    for (const backend::AnimeSummary& entry : entries_) {
        const QString format = entry.format.isEmpty() ? QStringLiteral("unknown") : entry.format;
        counts[format] += 1;
    }
    QStringList present;
    for (const QString& format : kOrder) {
        if (counts.value(format) > 0) {
            present << format;
        }
    }
    formatBar_->blockSignals(true);
    while (formatBar_->count() > 0) {
        formatBar_->removeTab(0);
    }
    if (present.size() >= 2) {
        formatBar_->addTab(tr("All (%1)").arg(entries_.size()));
        formatBar_->setTabData(0, QStringLiteral("all"));
        for (const QString& format : present) {
            formatBar_->addTab(QStringLiteral("%1 (%2)").arg(format.toUpper()).arg(counts[format]));
            formatBar_->setTabData(formatBar_->count() - 1, format);
        }
        formatBar_->setCurrentIndex(0);
        formatBar_->show();
    } else {
        formatBar_->hide();
    }
    formatBar_->blockSignals(false);
}

void SearchPage::applyFormatFilter() { populateRows(); }

void SearchPage::populateRows() {
    results_->clear();
    QString wanted = QStringLiteral("all");
    if (formatBar_->isVisible()) {
        const int index = formatBar_->currentIndex();
        if (index >= 0) {
            wanted = formatBar_->tabData(index).toString();
        }
    }
    for (const backend::AnimeSummary& entry : entries_) {
        const QString format = entry.format.isEmpty() ? QStringLiteral("unknown") : entry.format;
        if (wanted != QLatin1String("all") && format != wanted) {
            continue;
        }
        // List responses already carry the airing status and the planned episode
        // count, so a search row reads like a home card without a detail call.
        QStringList parts;
        parts << backend::displayName(entry.titles);
        if (entry.year.has_value()) {
            parts << QString::number(*entry.year);
        }
        if (!entry.format.isEmpty()) {
            parts << entry.format.toUpper();
        }
        const QString statusText = labels::status(entry.status);
        if (!statusText.isEmpty()) {
            parts << statusText;
        }
        if (entry.episodes.has_value() && *entry.episodes > 0) {
            parts << tr("%n episode(s)", nullptr, *entry.episodes);
        }
        const QString label = parts.join(QStringLiteral(" · "));
        auto* item = new QListWidgetItem(label, results_);
        item->setToolTip(label);
        QVariantMap data;
        data.insert(QStringLiteral("ref"), entry.ref);
        data.insert(QStringLiteral("poster"), backend::posterUrl(entry));
        data.insert(QStringLiteral("thumb"), backend::thumbUrl(entry));
        item->setData(Qt::UserRole, data);
        const QString thumb = backend::thumbUrl(entry);
        const auto cached = icons_.constFind(thumb);
        if (!thumb.isEmpty() && cached != icons_.constEnd()) {
            item->setIcon(QIcon(cached.value()));
        }
        // Reading the cache is not enough: nothing ever put a search row's
        // thumb in it, because this is the only place that can ask for it.
        // requestThumb is a no-op once the url is cached or in flight, so one
        // call per row is safe and a repaint costs nothing.
        requestThumb(thumb);
    }
}

void SearchPage::onError(const QString& message) {
    rateLabel_->refresh();
    status_->setText(message);
    updatePager();
}

void SearchPage::updatePager() {
    pager_->setVisible(pages_ > 1);
    pager_->setState(page_, pages_, total_);
}

void SearchPage::goToPage(int page) {
    if (page >= 1 && page <= pages_ && page != page_) {
        fetchPage(page);
    }
}

void SearchPage::openItem(QListWidgetItem* item) {
    if (item == nullptr) {
        return;
    }
    const QVariantMap data = item->data(Qt::UserRole).toMap();
    const QString ref = data.value(QStringLiteral("ref")).toString();
    if (!ref.isEmpty()) {
        Q_EMIT openRequested(ref, data.value(QStringLiteral("poster")).toString());
    }
}

void SearchPage::resultsContextMenu(const QPoint& position) {
    QListWidgetItem* item = results_->itemAt(position);
    if (item == nullptr) {
        return;
    }
    const QVariantMap data = item->data(Qt::UserRole).toMap();
    const QString ref = data.value(QStringLiteral("ref")).toString();
    if (ref.isEmpty()) {
        return;
    }
    QMenu menu(this);
    QAction* open = menu.addAction(tr("Open"));
    open->setToolTip(tr("Open the title page"));
    connect(open, &QAction::triggered, this, [this, item] { openItem(item); });
    QAction* copyLink = menu.addAction(tr("Copy link"));
    copyLink->setToolTip(tr("Copy the page address on mikai.me"));
    connect(copyLink, &QAction::triggered, this, [this, ref] { copyText(pageUrlFor(ref)); });
    QAction* copyRef = menu.addAction(tr("Copy ID"));
    copyRef->setToolTip(tr("Copy the title identifier"));
    connect(copyRef, &QAction::triggered, this, [ref] { copyText(ref); });
    menu.exec(results_->mapToGlobal(position));
}

void SearchPage::animeCardMenu(const QString& ref, const QPoint& globalPos) {
    QMenu menu(this);
    QAction* open = menu.addAction(tr("Open"));
    open->setToolTip(tr("Open the title page"));
    connect(open, &QAction::triggered, this, [this, ref] { openByRef(ref); });
    QAction* copyLink = menu.addAction(tr("Copy link"));
    copyLink->setToolTip(tr("Copy the page address on mikai.me"));
    connect(copyLink, &QAction::triggered, this, [this, ref] { copyText(pageUrlFor(ref)); });
    QAction* copyRef = menu.addAction(tr("Copy ID"));
    copyRef->setToolTip(tr("Copy the title identifier"));
    connect(copyRef, &QAction::triggered, this, [ref] { copyText(ref); });
    QAction* browser = menu.addAction(tr("Open in browser"));
    browser->setToolTip(tr("Open the page on mikai.me"));
    connect(browser, &QAction::triggered, this,
            [this, ref] { QDesktopServices::openUrl(QUrl(pageUrlFor(ref))); });
    menu.exec(globalPos);
}

QString SearchPage::pageUrlFor(const QString& ref) const {
    // The card menus only carry a ref, but the detail response that filled the
    // card also carries the slug the public page is served at. A link without
    // it is a 404, so the cached detail is what turns the id into a page.
    const auto it = cardDetails_.constFind(ref);
    if (it == cardDetails_.constEnd()) {
        return backend::animePageUrl(ref);
    }
    return backend::animePageUrl(ref, it.value().ids.value(QStringLiteral("slug")).toString());
}

void SearchPage::openByRef(const QString& ref) {
    // The card that raised the menu knows its own poster; look it up so the
    // title page can paint its cover before the detail call lands.
    for (const QString& key : {QStringLiteral("ongoing"), QStringLiteral("popular"),
                               kRecentKey}) {
        for (const backend::AnimeSummary& entry : carouselEntries_.value(key)) {
            if (entry.ref == ref) {
                Q_EMIT openRequested(ref, backend::posterUrl(entry));
                return;
            }
        }
    }
    for (const backend::AnimeSummary& entry : entries_) {
        if (entry.ref == ref) {
            Q_EMIT openRequested(ref, backend::posterUrl(entry));
            return;
        }
    }
    Q_EMIT openRequested(ref, QString());
}

void SearchPage::requestThumb(const QString& url) {
    // One task per image. A single task looping over the URLs made sixty
    // downloads wait on each other inside one worker, which is what the home
    // page felt like.
    if (url.isEmpty() || thumbsInFlight_.contains(url)) {
        return;
    }
    // A render rebuilds its widgets, so a cached image has to paint the ones
    // standing now. Skipping here left every rebuilt card on its placeholder
    // until the next full reload of that url.
    const auto painted = rounded_.constFind(url);
    if (painted != rounded_.constEnd()) {
        for (TitleCard* card : cardsByImageKey_.value(url)) {
            card->applyPixmap(painted.value());
        }
        applyThumbs();
        applyFeedPosters();
        return;
    }
    const auto icon = icons_.constFind(url);
    if (icon != icons_.constEnd()) {
        const QPixmap ready = TitleCard::rounded(
            icon.value(), QSize(theme::kHomePosterWidth, theme::kHomePosterHeight));
        rounded_.insert(url, ready);
        for (TitleCard* card : cardsByImageKey_.value(url)) {
            card->applyPixmap(ready);
        }
        applyThumbs();
        applyFeedPosters();
        return;
    }
    const auto cached = thumbCache_.constFind(url);
    if (cached != thumbCache_.constEnd()) {
        applyThumb(url, cached.value());
        return;
    }
    thumbsInFlight_.insert(url);
    runner_.runImage(
        [this, url] { return client_.fetchBytes(url); },
        [this, url](const QVariant& result) {
            thumbsInFlight_.remove(url);
            thumbCache_.insert(url, result.toByteArray());
            applyThumb(url, result.toByteArray());
        },
        [this, url](const QString&) { thumbsInFlight_.remove(url); });
}

void SearchPage::applyThumb(const QString& url, const QByteArray& data) {
    // A whole feed's worth of these lands in one burst. Each one used to decode
    // twice, rescan every card, walk every result row and re-scale every feed
    // poster, all before the next one; queueing them lets one pass over the
    // whole batch do each of those jobs once, and each visible surface is
    // touched once instead of once per arrival.
    pendingThumbs_.insert(url, data);
    thumbTimer_->start();
}

void SearchPage::flushThumbs() {
    if (pendingThumbs_.isEmpty()) {
        return;
    }
    // One poster per event-loop turn, never the whole queue: a JPEG decode
    // plus two smooth scales costs milliseconds each, and decoding a full feed
    // in one timer tick froze every menu, click and hover until the queue
    // drained. The timer re-arms while work remains, so posters trickle in and
    // the UI stays responsive between decodes.
    auto it = pendingThumbs_.constBegin();
    const QString key = it.key();
    const QByteArray data = it.value();
    pendingThumbs_.erase(pendingThumbs_.find(key));
    if (!icons_.contains(key)) {
        const QPixmap pixmap = pixmapFrom(data, kThumbDecodeSize);
        if (!pixmap.isNull()) {
            icons_.insert(key, pixmap);
            // Bytes served their purpose: the decoded icon is what every
            // consumer reads from here on, so holding both doubles poster
            // memory per feed.
            thumbCache_.remove(key);
            // A poster URL is shared by several cards; the index turns that
            // into a lookup instead of a scan of every card on the page. The
            // rounded variant is painted once per URL, not once per card.
            const QPixmap ready = TitleCard::rounded(
                pixmap, QSize(theme::kHomePosterWidth, theme::kHomePosterHeight));
            rounded_.insert(key, ready);
            const QList<TitleCard*> cards = cardsByImageKey_.value(key);
            for (TitleCard* card : cards) {
                card->applyPixmap(ready);
            }
        }
    }
    applyThumbs();
    applyFeedPosters();
    if (!pendingThumbs_.isEmpty()) {
        thumbTimer_->start();
    }
}

void SearchPage::applyThumbs() {
    // In browse mode the result list is empty and hidden, and a per-row map
    // copy there is the whole cost of a thumb landing.
    if (!results_->isVisible()) {
        return;
    }
    for (int row = 0; row < results_->count(); ++row) {
        QListWidgetItem* item = results_->item(row);
        const QVariantMap data = item->data(Qt::UserRole).toMap();
        const QString thumb = data.value(QStringLiteral("thumb")).toString();
        const auto cached = icons_.constFind(thumb);
        if (!thumb.isEmpty() && cached != icons_.constEnd()) {
            item->setIcon(QIcon(cached.value()));
        }
    }
}

void SearchPage::applyFeedPosters() {
    if (!discoveryScroll_->isVisible()) {
        return;
    }
    for (auto labels = feedPosterLabels_.begin(); labels != feedPosterLabels_.end(); ++labels) {
        for (FeedPosterSlot& entry : labels.value()) {
            if (entry.label.isNull()) {
                continue;
            }
            // A label already showing its thumb has nothing to gain from another
            // pass over the feed's icon table.
            if (entry.shown == entry.url) {
                continue;
            }
            const auto cached = icons_.constFind(entry.url);
            if (cached == icons_.constEnd()) {
                continue;
            }
            entry.label->setPixmap(cached.value().scaled(
                entry.label->size(), Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation));
            entry.shown = entry.url;
        }
    }
}

}  // namespace mikai::app
