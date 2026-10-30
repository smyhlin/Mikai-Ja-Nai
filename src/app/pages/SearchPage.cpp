#include <utility>
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
#include <QDialog>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QSpinBox>
#include <QGuiApplication>
#include <QStyleHints>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QEasingCurve>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPixmap>
#include <QPainter>
#include <QPalette>
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

#include <memory>
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

/// Enable a carousel's arrows only where there is somewhere to scroll to.
void syncArrows(QScrollBar* bar, QPushButton* previous, QPushButton* next) {
    previous->setEnabled(bar->value() > bar->minimum());
    next->setEnabled(bar->value() < bar->maximum());
}

bool hasExplicitCatalogueCriteria(const backend::AnimeQuery& query) {
    if (!query.genres.isEmpty() || !query.genres_exclude.isEmpty()) return true;
    if (!query.tags.isEmpty() || !query.tags_exclude.isEmpty()) return true;
    if (!query.years.isEmpty() || query.year_from || query.year_to) return true;
    if (!query.seasons.isEmpty() || !query.formats.isEmpty() || !query.statuses.isEmpty()) return true;
    if (!query.studios.isEmpty() || !query.teams.isEmpty()) return true;
    if (query.genres_match != QLatin1String("all") || query.tags_match != QLatin1String("all")) return true;
    return query.sort != QStringLiteral("name") || !query.order.isEmpty();
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
    auto* heading = new QLabel(tr("Mikai Ja Nai"), this);
    heading->setObjectName(QStringLiteral("heading"));
    topRow->addWidget(heading);
    headerLogo_ = new QLabel(this);
    headerLogo_->setObjectName(QStringLiteral("headerLogo"));
    headerLogo_->setAccessibleName(QString());
    headerLogo_->setAccessibleDescription(QString());
    headerLogo_->setFocusPolicy(Qt::NoFocus);
    headerLogo_->installEventFilter(this);
    headerLogo_->setMinimumSize(24, 24);
    headerLogo_->setFixedSize(36, 36);
    headerLogoSource_.load(QStringLiteral(":/icons/app/mikai-mono.png"));
    topRow->insertWidget(topRow->indexOf(heading), headerLogo_);
    refreshHeaderLogo();
    homeButton_ = new QPushButton(tr("Home"), this);
    homeButton_->setObjectName(QStringLiteral("homeButton"));
    connect(homeButton_, &QPushButton::clicked, this, &SearchPage::returnHome);
    topRow->addWidget(homeButton_);
    homeButton_->hide();
    topRow->addStretch();
    mediaButton_ = new QPushButton(tr("Open video"), this);
    mediaButton_->setObjectName(QStringLiteral("secondary"));
    mediaButton_->setAccessibleName(tr("Open video"));
    // The menu and its actions are owned by the button; the actions are kept
    // because their captions are this page's to restate.
    auto* mediaMenu = new QMenu(mediaButton_);
    mediaMenu->setObjectName(QStringLiteral("openMediaMenu"));
    openFileAction_ = mediaMenu->addAction(tr("Video file…"));
    openFileAction_->setObjectName(QStringLiteral("openVideoFile"));
    connect(openFileAction_, &QAction::triggered, this, &SearchPage::mediaFileRequested);
    openUrlAction_ = mediaMenu->addAction(tr("Video URL…"));
    openUrlAction_->setObjectName(QStringLiteral("openVideoUrl"));
    connect(openUrlAction_, &QAction::triggered, this, &SearchPage::mediaUrlRequested);
    mediaButton_->setMenu(mediaMenu);
    topRow->addWidget(mediaButton_);
    rateLabel_ = new RateLimitLabel(client_, this);
    topRow->addWidget(rateLabel_);

    keysButton_ = new QPushButton(this);
    keysButton_->setObjectName(QStringLiteral("settingsChip"));
    keysButton_->setIcon(theme::keyboardIcon());
    keysButton_->setIconSize(QSize(theme::kIconSize, theme::kIconSize));
    keysButton_->setToolTip(tr("Keyboard shortcuts (?)"));
    keysButton_->setAccessibleName(tr("Keyboard shortcuts"));
    connect(keysButton_, &QPushButton::clicked, this, [this] {
        if (keyHints_ == nullptr) {
            keyHints_ = new mplayer::shell::KeyHints(this);
        }
        keyHints_->popupNear(keysButton_->mapToGlobal(
            QPoint(keysButton_->width() / 2, keysButton_->height())));
    });
    topRow->addWidget(keysButton_);

    settingsButton_ = new QPushButton(this);
    settingsButton_->setObjectName(QStringLiteral("settingsChip"));
    settingsButton_->setIcon(theme::gearIcon());
    settingsButton_->setIconSize(QSize(theme::kIconSize, theme::kIconSize));
    settingsButton_->setToolTip(tr("API token settings"));
    settingsButton_->setAccessibleName(tr("API token settings"));
    connect(settingsButton_, &QPushButton::clicked, this, &SearchPage::settingsRequested);
    topRow->addWidget(settingsButton_);
    root->addLayout(topRow);

    auto* searchRow = new QHBoxLayout();
    query_ = new QLineEdit(this);
    query_->setObjectName(QStringLiteral("searchQuery"));
    query_->setPlaceholderText(tr("Search by title — Enter or the button"));
    query_->setClearButtonEnabled(true);
    filtersButton_ = new QPushButton(tr("Filters"), this);
    filtersButton_->setObjectName(QStringLiteral("catalogueFiltersButton"));
    teamsButton_ = new QPushButton(tr("Teams…"), this);
    teamsButton_->setObjectName(QStringLiteral("catalogueTeamsButton"));
    searchButton_ = new QPushButton(tr("Search"), this);
    searchButton_->setObjectName(QStringLiteral("primary"));
    query_->installEventFilter(this);
    connect(query_, &QLineEdit::returnPressed, this, &SearchPage::search);
    connect(searchButton_, &QPushButton::clicked, this, &SearchPage::search);
    connect(query_, &QLineEdit::textChanged, this, [this](const QString& text) {
        if (!text.trimmed().isEmpty() || browseMode_) {
            return;
        }
        // Clearing the text returns to the home collection only when no other
        // criterion survives: a sort the viewer chose is one of them, and
        // dropping it here would answer a different question than the one the
        // filters hold. A criterion that does survive is asked for again, so
        // the results on screen are the ones the surviving criteria name.
        if (hasExplicitCatalogueCriteria(catalogueQuery_)) {
            applyCatalogueFilters();
        } else {
            loadOngoing();
        }
    });
    searchRow->addWidget(query_);
    searchRow->addWidget(filtersButton_);
    searchRow->addWidget(teamsButton_);
    searchRow->addWidget(searchButton_);
    connect(filtersButton_, &QPushButton::clicked, this, &SearchPage::showCatalogueFilters);
    connect(teamsButton_, &QPushButton::clicked, this, &SearchPage::openTeamPicker);
    loadDictionaries();

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
    CarouselSection* recent = buildCarousel(kRecentKey);
    CarouselSection* ongoing = buildCarousel(QStringLiteral("ongoing"));
    CarouselSection* popular = buildCarousel(QStringLiteral("popular"));
    FeedSection* updates = buildFeedSection(QStringLiteral("updates"));
    FeedSection* schedule = buildFeedSection(QStringLiteral("schedule"));
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
    pageGlow_ = new PageGlow(this);
    root->addWidget(discoveryScroll_, 1);

    formatBar_ = new QTabBar(this);
    formatBar_->setExpanding(false);
    formatBar_->setDrawBase(false);
    formatBar_->hide();
    connect(formatBar_, &QTabBar::currentChanged, this, [this](int) { applyFormatFilter(); });
    root->addWidget(formatBar_);

    results_ = new QListWidget(this);
    results_->setObjectName(QStringLiteral("searchResults"));
    results_->setIconSize(QSize(theme::kSearchPosterWidth, theme::kSearchPosterHeight));
    results_->setTextElideMode(Qt::ElideRight);
    results_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    results_->setContextMenuPolicy(Qt::CustomContextMenu);
    results_->installEventFilter(this);
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

    connect(&appearanceNotifier(), &AppearanceNotifier::changed, this,
            [this](const theme::Appearance& previous) {
                const auto& appearance = currentAppearance();
                if (previous.radiusStep != appearance.radiusStep ||
                    previous.coverGradient != appearance.coverGradient) {
                    refreshCardFrames();
                }
                refreshHeaderLogo();
            });

    QTimer::singleShot(0, this, &SearchPage::loadOngoing);
    if (!recentItems_.isEmpty()) {
        showCarousel(kRecentKey, recentItems_);
    }
}

SearchPage::~SearchPage() {
    qDeleteAll(carousels_);
    qDeleteAll(feeds_);
}

QString SearchPage::carouselHeading(const QString& key) const {
    if (key == kRecentKey) return tr("Recently opened");
    if (key == QLatin1String("ongoing")) return tr("Currently airing");
    if (key == QLatin1String("popular")) return tr("Popular on Mikai");
    return {};
}

QString SearchPage::feedHeading(const QString& key) const {
    if (key == QLatin1String("updates")) return tr("Latest updates");
    if (key == QLatin1String("schedule")) return tr("Release schedule");
    return {};
}

QString SearchPage::feedEmptyLine(const QString& key) const {
    return key == QLatin1String("updates") ? tr("No updates yet.")
                                           : tr("No airings in the schedule yet.");
}

QString SearchPage::feedCountLine(const QString& key, int count) const {
    return key == QLatin1String("updates") ? tr("%n recent update(s)", nullptr, count)
                                           : tr("%n upcoming airing(s)", nullptr, count);
}

QString SearchPage::blockFailureContext(const QString& key) const {
    if (key == QLatin1String("updates")) return tr("Failed to load updates");
    if (key == QLatin1String("schedule")) return tr("Failed to load schedule");
    return tr("Failed to load");
}

QString SearchPage::feedDayLabel(const QString& dayKey) const {
    if (dayKey == QLatin1String("monday")) return tr("Mon");
    if (dayKey == QLatin1String("tuesday")) return tr("Tue");
    if (dayKey == QLatin1String("wednesday")) return tr("Wed");
    if (dayKey == QLatin1String("thursday")) return tr("Thu");
    if (dayKey == QLatin1String("friday")) return tr("Fri");
    if (dayKey == QLatin1String("saturday")) return tr("Sat");
    if (dayKey == QLatin1String("sunday")) return tr("Sun");
    return {};
}

void SearchPage::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
    } else if (event->type() == QEvent::PaletteChange ||
               event->type() == QEvent::ApplicationPaletteChange ||
               event->type() == QEvent::ScreenChangeInternal ||
               event->type() == QEvent::DevicePixelRatioChange) {
        refreshHeaderLogo();
    }
}

void SearchPage::refreshHeaderLogo() {
    if (headerLogo_ == nullptr || headerLogoSource_.isNull()) return;
    const QColor tint = headerLogo_->palette().color(QPalette::WindowText);
    const qreal dpr = headerLogo_->devicePixelRatioF();
    if (!headerLogo_->pixmap().isNull() && tint == headerLogoTint_ &&
        qFuzzyCompare(dpr, headerLogoDpr_)) {
        return;
    }
    const QSize pixelSize = headerLogo_->size() * dpr;
    QImage tinted = headerLogoSource_.scaled(pixelSize, Qt::KeepAspectRatio,
                                              Qt::SmoothTransformation)
                        .convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QPainter painter(&tinted);
    painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
    painter.fillRect(tinted.rect(), tint);
    painter.end();
    QPixmap pixmap = QPixmap::fromImage(tinted);
    pixmap.setDevicePixelRatio(dpr);
    headerLogo_->setPixmap(pixmap);
    headerLogoTint_ = tint;
    headerLogoDpr_ = dpr;
}

void SearchPage::retranslateUi() {
    // Only words move. The query keeps its text and its focus, the result page
    // its rows, the format bar its filter, the carousels their cards and the
    // feeds the rows they already hold - and nothing below reaches the network.
    if (query_ != nullptr) {
        query_->setPlaceholderText(tr("Search by title — Enter or the button"));
    }
    if (homeButton_ != nullptr) homeButton_->setText(tr("Home"));
    if (mediaButton_ != nullptr) {
        mediaButton_->setText(tr("Open video"));
        mediaButton_->setAccessibleName(tr("Open video"));
    }
    if (openFileAction_ != nullptr) openFileAction_->setText(tr("Video file…"));
    if (openUrlAction_ != nullptr) openUrlAction_->setText(tr("Video URL…"));
    if (keysButton_ != nullptr) {
        keysButton_->setToolTip(tr("Keyboard shortcuts (?)"));
        keysButton_->setAccessibleName(tr("Keyboard shortcuts"));
    }
    if (settingsButton_ != nullptr) {
        settingsButton_->setToolTip(tr("API token settings"));
        settingsButton_->setAccessibleName(tr("API token settings"));
    }
    if (searchButton_ != nullptr) searchButton_->setText(tr("Search"));
    if (filtersButton_ != nullptr) filtersButton_->setText(tr("Filters"));
    if (teamsButton_ != nullptr) teamsButton_->setText(tr("Teams…"));
    if (resultsHeading_ != nullptr) {
        // The heading names the mode the page is in, never the request that is
        // running: browse shows a recommendation set, everything else a search.
        resultsHeading_->setText(teamBrowseMode_
                                     ? tr("Anime from %1").arg(activeTeam_.name)
                                     : browseMode_ ? tr("Recommendations") : tr("Search results"));
    }
    if (teamBackButton_ != nullptr) teamBackButton_->setText(tr("All catalogue"));
    if (teamBrowseMode_) renderTeamProfile();
    // The bar's own "All (n)" caption is app wording; its counts, its tabs and
    // the active filter are read back from the state they already hold.
    if (formatBar_ != nullptr) {
        rebuildFormatBar();
        if (teamBrowseMode_) formatBar_->hide();
    }
    renderStatus();
    if (results_ != nullptr && results_->count() == static_cast<int>(entries_.size())) {
        // Re-worded where they stand. Rebuilding the list would drop the
        // selection and the scroll position the viewer is reading, and would
        // also re-ask for any thumb whose fetch had failed.
        for (int row = 0; row < results_->count(); ++row) {
            QListWidgetItem* item = results_->item(row);
            const QString text = resultRowText(entries_.at(row));
            item->setText(text);
            item->setToolTip(text);
        }
    }
    QStringList sectionKeys{kRecentKey};
    sectionKeys += feeds::all();
    for (const QString& key : sectionKeys) {
        if (CarouselSection* section = carouselSection(key)) {
            if (section->heading != nullptr) section->heading->setText(carouselHeading(key));
            section->previous->setToolTip(tr("Previous page (or Alt + mouse wheel)"));
            section->next->setToolTip(tr("Next page (or Alt + mouse wheel)"));
            renderCarouselLine(*section);
            if (section->hasFailure) {
                // The failure itself belongs to the surface, which re-localizes
                // what the app words and keeps what the backend produced. Only
                // the clause this page wrote is re-sent.
                section->failure->setContext(blockFailureContext(key));
            }
        }
        if (FeedSection* section = feedSection(key)) {
            if (section->heading != nullptr) section->heading->setText(feedHeading(key));
            section->retry->setText(tr("Refresh"));
            renderFeedLine(key, *section);
            renderFeedRows(*section);
            if (section->hasFailure) {
                section->failure->setContext(blockFailureContext(key));
            }
        }
    }
}

void SearchPage::renderCarouselLine(CarouselSection& section) {
    if (section.state == nullptr) return;
    switch (section.line) {
        case CarouselSection::Line::Unloaded:
            // The recently-opened block has no request behind it: it says so
            // until a title is recorded.
            section.state->setText(tr("Collection not loaded yet"));
            break;
        case CarouselSection::Line::Loading:
            section.state->setText(tr("Loading collection…"));
            break;
        case CarouselSection::Line::Empty:
            section.state->setText(tr("No titles in this collection yet."));
            break;
        case CarouselSection::Line::Count:
            section.state->setText(tr("%n title(s)", nullptr, section.lineCount));
            break;
    }
}

void SearchPage::renderFeedLine(const QString& key, FeedSection& section) {
    if (section.state == nullptr) return;
    switch (section.line) {
        case FeedSection::Line::Loading:
            // Each feed words its own progress; a load has already installed
            // the state this restates.
            section.state->setText(key == QLatin1String("schedule") ? tr("Loading schedule…")
                                                                    : tr("Loading new episodes…"));
            break;
        case FeedSection::Line::Empty:
            section.state->setText(feedEmptyLine(key));
            break;
        case FeedSection::Line::Count:
            section.state->setText(feedCountLine(key, section.lineCount));
            break;
    }
}

void SearchPage::renderFeedRows(FeedSection& section) {
    // One pass over the labels the rows already hold: the row widgets, their
    // order and the host's scroll stay exactly where they are.
    const qsizetype count = qMin(section.rowMeta.size(), section.rowMetaLabels.size());
    for (qsizetype index = 0; index < count; ++index) {
        QLabel* label = section.rowMetaLabels.at(index).data();
        if (label == nullptr) continue;
        label->setText(composeFeedMeta(section.rowMeta.at(index)));
    }
}

void SearchPage::renderStatus() {
    if (status_ == nullptr) return;
    switch (statusLine_) {
        case Status::BrowseHint:
            status_->setText(
                tr("Click a card to open a title · search by title is available above"));
            break;
        case Status::LoadingBrowse:
            status_->setText(tr("Loading collection…"));
            break;
        case Status::LoadingSearch:
            status_->setText(tr("Searching…"));
            break;
        case Status::Results:
            status_->setText(entries_.isEmpty() ? tr("Nothing found")
                                                : tr("Found: %1").arg(total_));
            break;
        case Status::Failure: {
            // The same rule the failure surface uses: only a code the app words
            // itself is re-localized, a line the backend produced stays as it
            // arrived.
            backend::Failure failure = statusFailure_;
            const QString localized = localizedApiError(failure.code);
            if (!localized.isEmpty()) failure.message = localized;
            status_->setText(backend::failureLine(failure));
            break;
        }
        case Status::Notice:
            status_->setText(noticeText(statusNotice_));
            break;
    }
}

QString SearchPage::noticeText(Notice notice) const {
    switch (notice) {
        case Notice::CopyLink:
            return tr("Failed to copy link");
        case Notice::CopyId:
            return tr("Failed to copy ID");
        case Notice::Browser:
            return tr("Failed to open in browser");
    }
    return {};
}

QString SearchPage::composeFeedMeta(const FeedRowMeta& meta) const {
    QStringList parts;
    // A schedule row is the one that names its weekday first, and that is also
    // what decides where the clock text goes: after the weekday there, last for
    // an updates row.
    const bool scheduleRow = !meta.dayKey.isEmpty();
    const QString episode =
        meta.episode.isEmpty() ? (meta.newEpisode ? tr("New episode") : QString())
                               : tr("Episode %1").arg(meta.episode);
    if (scheduleRow) {
        parts << feedDayLabel(meta.dayKey);
        if (!meta.when.isEmpty()) parts << meta.when;
        if (!episode.isEmpty()) parts << episode;
    } else if (!episode.isEmpty()) {
        parts << episode;
    }
    if (!meta.team.isEmpty()) parts << meta.team;
    if (!meta.kind.isEmpty()) {
        parts << (meta.kind == QLatin1String("voice")
                      ? tr("dub")
                      : (meta.kind == QLatin1String("sub") ? tr("subs") : meta.kind));
    }
    if (!meta.provider.isEmpty()) {
        QString capitalized = meta.provider;
        capitalized[0] = capitalized.at(0).toUpper();
        parts << capitalized;
    }
    if (!scheduleRow && !meta.when.isEmpty()) parts << meta.when;
    // A weekday or episode label that did not resolve leaves an empty part
    // rather than a stray separator.
    parts.removeAll(QString());
    return parts.join(QStringLiteral(" · "));
}

QString SearchPage::resultRowText(const backend::AnimeSummary& entry) const {
    // List responses already carry the airing status and the planned episode
    // count, so a search row reads like a home card without a detail call.
    QStringList parts;
    parts << labels::titleName(entry.titles);
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
    return parts.join(QStringLiteral(" · "));
}

void SearchPage::closeTransientOverlays() {
    if (keyHints_ != nullptr) keyHints_->close();
}

void SearchPage::returnHome() {
    closeTransientOverlays();
    if (browseMode_ && !teamBrowseMode_ && !discoveryScroll_->isHidden() &&
        query_->text().isEmpty()) {
        ++requestSeq_;
        ++catalogueRequestSeq_;
        ++teamRequestSeq_;
        ++formatTotalsSeq_;
    } else {
        loadOngoing();
    }
    query_->clearFocus();
}

bool SearchPage::escapeSearch() {
    if (keyHints_ != nullptr && keyHints_->isVisible()) {
        keyHints_->close();
        return true;
    }
    returnHome();
    return true;
}

SearchPage::CarouselSection* SearchPage::buildCarousel(const QString& key) {
    auto* section = new CarouselSection();
    section->section = new QWidget(this);
    auto* layout = new QVBoxLayout(section->section);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(theme::kSpacingSmall - theme::kSpacingXs);

    auto* header = new QHBoxLayout();
    section->heading = new QLabel(carouselHeading(key), section->section);
    section->heading->setObjectName(QStringLiteral("sectionHeading"));
    header->addWidget(section->heading);
    header->addStretch();
    section->previous = new QPushButton(section->section);
    section->previous->setIcon(theme::chevronLeftIcon(16));
    section->next = new QPushButton(section->section);
    section->next->setIcon(theme::chevronRightIcon(16));
    for (QPushButton* button : {section->previous, section->next}) {
        button->setObjectName(QStringLiteral("carouselControl"));
        button->setIconSize(QSize(16, 16));
        button->setCursor(Qt::PointingHandCursor);
        header->addWidget(button);
    }
    layout->addLayout(header);

    auto* stateRow = new QHBoxLayout();
    section->state = new QLabel(tr("Collection not loaded yet"), section->section);
    section->state->setObjectName(QStringLiteral("muted"));
    section->state->setWordWrap(true);
    section->failure = new FailureView(section->section);
    connect(section->failure, &FailureView::retryRequested, this,
            [this, key] { requestRecommendation(key); });
    stateRow->addWidget(section->state, 1);
    stateRow->addWidget(section->failure, 1);
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
    // Qt 6.12 surfaces the system reduced-motion preference through
    // QStyleHints::accessibility(); when it is set, jump instead of animating
    // so the carousel respects the same OS setting the rest of the UI does. Both
    // pointers are platform-owned and checked rather than assumed.
    const QStyleHints* hints = QGuiApplication::styleHints();
    const QAccessibilityHints* accessibility = hints != nullptr ? hints->accessibility() : nullptr;
    if (accessibility != nullptr &&
        accessibility->motionPreference() == Qt::MotionPreference::ReducedMotion) {
        bar->setValue(target);
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
    if (watched == headerLogo_ &&
        (event->type() == QEvent::PaletteChange ||
         event->type() == QEvent::ApplicationPaletteChange ||
         event->type() == QEvent::ScreenChangeInternal ||
         event->type() == QEvent::DevicePixelRatioChange)) {
        refreshHeaderLogo();
    }
    if (event->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(event);
        if (ke->key() == Qt::Key_Escape && escapeSearch()) {
            ke->accept();
            return true;
        }
    }
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

void SearchPage::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape && escapeSearch()) {
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

SearchPage::FeedSection* SearchPage::buildFeedSection(const QString& key) {
    auto* section = new FeedSection();
    section->section = new QWidget(this);
    auto* layout = new QVBoxLayout(section->section);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(theme::kSpacingSmall - theme::kSpacingXs);

    auto* header = new QHBoxLayout();
    section->heading = new QLabel(feedHeading(key), section->section);
    section->heading->setObjectName(QStringLiteral("sectionHeading"));
    header->addWidget(section->heading);
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

    // The row already carries its own always-visible Refresh, so this surface
    // names the cause without putting a second retry beside it.
    section->failure = new FailureView(section->section);
    section->failure->setRetryVisible(false);
    layout->addWidget(section->failure);

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
        record.insert(QStringLiteral("title"), labels::titleName(recent.titles));
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
    clearBlockFailure(section->failure, section->state);
    section->hasFailure = false;
    QList<backend::AnimeSummary> shown = items;
    while (shown.size() > theme::kHomeCarouselMaxTitles) {
        shown.removeLast();
    }
    carouselEntries_.insert(key, shown);
    if (shown.isEmpty()) {
        section->line = CarouselSection::Line::Empty;
        section->lineCount = 0;
        renderCarouselLine(*section);
        section->scroll->hide();
        return;
    }
    section->line = CarouselSection::Line::Count;
    section->lineCount = static_cast<int>(shown.size());
    renderCarouselLine(*section);

    while (section->cards->count() > 0) {
        QLayoutItem* item = section->cards->takeAt(0);
        if (item->widget() != nullptr) {
            item->widget()->deleteLater();
        }
        delete item;
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
    // The client is taken on the GUI thread and carried by value into the
    // worker: a worker that reached through `this` would read a page that may
    // already be gone. The completion is guarded the same way, so a result
    // that lands after the page was closed is dropped instead of touching it.
    backend::MikaiClient* client = &client_;
    const QPointer<SearchPage> self(this);
    runner_.runBulk(
        [client, ref] { return QVariant::fromValue(client->getAnime(ref)); },
        [self, ref](const QVariant& result) {
            if (self.isNull()) {
                return;
            }
            self->applyCardDetail(ref, result.value<backend::AnimeDetail>());
        },
        [](const backend::Failure&) {});
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
    browseMode_ = true;
    teamBrowseMode_ = false;
    ++requestSeq_;
    ++catalogueRequestSeq_;
    ++teamRequestSeq_;
    ++formatTotalsSeq_;
    if (teamBackButton_ != nullptr) teamBackButton_->hide();
    if (teamDetailLabel_ != nullptr) teamDetailLabel_->hide();
    query_->clear();
    showDiscovery();
    currentQuery_.clear();
    activeFormat_.clear();
    pageSize_ = theme::kSearchResultsPerPage;
    page_ = 1;
    pages_ = 1;
    total_ = 0;
    entries_.clear();
    results_->clear();
    pager_->hide();
    resultsHeading_->setText(tr("Recommendations"));
    statusLine_ = Status::BrowseHint;
    renderStatus();
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
    section->line = CarouselSection::Line::Loading;
    section->lineCount = 0;
    renderCarouselLine(*section);
    clearBlockFailure(section->failure, section->state);
    section->hasFailure = false;
    if (carouselEntries_.value(key).isEmpty()) {
        section->scroll->hide();
    }
    const bool popular = key == QLatin1String("popular");
    // The client travels by value into the worker and the page is reached only
    // through a guard, so neither outlives the page that asked for the work.
    backend::MikaiClient* client = &client_;
    const QPointer<SearchPage> self(this);
    runner_.run(
        [client, popular] {
            return QVariant::fromValue(popular ? client->browsePopular(1, theme::kHomeCarouselMaxTitles)
                                               : client->browseOngoing(1, theme::kHomeCarouselMaxTitles));
        },
        [self, key, seq](const QVariant& result) {
            if (self.isNull() || seq != self->feedSeq_[key]) {
                return;
            }
            self->rateLabel_->refresh();
            self->showCarousel(key, result.value<backend::SearchResult>().items);
        },
        [self, key, seq](const backend::Failure& failure) {
            if (self.isNull() || seq != self->feedSeq_[key]) {
                return;
            }
            self->rateLabel_->refresh();
            CarouselSection* current = self->carouselSection(key);
            current->hasFailure = true;
            self->showBlockFailure(current->failure, current->state, failure,
                                   self->blockFailureContext(key));
        });
}

void SearchPage::loadUpdates() {
    if (!enabledFeeds_.contains(QStringLiteral("updates"))) {
        return;
    }
    FeedSection* section = feedSection(QStringLiteral("updates"));
    feedSeq_[QStringLiteral("updates")] += 1;
    const int seq = feedSeq_[QStringLiteral("updates")];
    section->line = FeedSection::Line::Loading;
    section->lineCount = 0;
    renderFeedLine(QStringLiteral("updates"), *section);
    section->retry->setEnabled(false);
    const backend::UpdateFeedState previousState = updateFeedState_;
    // The incremental feed refresh runs against the client the page was given,
    // carried into the worker by value: the page itself is not read from the
    // worker thread, and the completion only touches the page through a guard.
    backend::MikaiClient* client = &client_;
    const QPointer<SearchPage> self(this);
    runner_.run(
        [client, previousState] {
            backend::UpdateFeedState updated = backend::refreshUpdateFeed(*client, previousState, 12);
            return QVariant::fromValue(updated);
        },
        [self, seq](const QVariant& result) {
            if (self.isNull() || seq != self->feedSeq_[QStringLiteral("updates")]) {
                return;
            }
            self->rateLabel_->refresh();
            FeedSection* current = self->feedSection(QStringLiteral("updates"));
            current->retry->setEnabled(true);
            // The row line is kept as the parts it was built from, so a
            // language switch re-words it without asking the backend again.
            QList<std::pair<backend::AnimeSummary, FeedRowMeta>> rows;
            self->updateFeedState_ = result.value<backend::UpdateFeedState>();
            for (int index = 0; index < self->updateFeedState_.items.size() &&
                                 index < theme::kHomeFeedMaxRows;
                 ++index) {
                const backend::AnimeUpdate& update = self->updateFeedState_.items.at(index);
                if (update.anime.ref.isEmpty()) {
                    continue;
                }
                FeedRowMeta meta;
                meta.episode = labels::variant(update.episode);
                meta.newEpisode = meta.episode.isEmpty();
                meta.team = update.team_name;
                if (meta.team.isEmpty()) {
                    QStringList names;
                    for (const backend::Team& entry : update.teams) {
                        if (!entry.name.isEmpty()) {
                            names << entry.name;
                        }
                    }
                    meta.team = names.join(QLatin1String(", "));
                }
                meta.kind = update.kind;
                meta.provider = update.provider;
                meta.when = formatFeedTime(update.published_at);
                rows.append({update.anime, meta});
            }
            self->showFeedRows(QStringLiteral("updates"), rows);
        },
        [self, seq](const backend::Failure& failure) {
            if (self.isNull() || seq != self->feedSeq_[QStringLiteral("updates")]) {
                return;
            }
            self->rateLabel_->refresh();
            FeedSection* current = self->feedSection(QStringLiteral("updates"));
            current->retry->setEnabled(true);
            current->rowsHost->setVisible(!current->rowMeta.isEmpty());
            current->hasFailure = true;
            self->showBlockFailure(current->failure, current->state, failure,
                                   self->blockFailureContext(QStringLiteral("updates")));
        });
}

void SearchPage::loadSchedule() {
    if (!enabledFeeds_.contains(QStringLiteral("schedule"))) {
        return;
    }
    FeedSection* section = feedSection(QStringLiteral("schedule"));
    feedSeq_[QStringLiteral("schedule")] += 1;
    const int seq = feedSeq_[QStringLiteral("schedule")];
    section->line = FeedSection::Line::Loading;
    section->lineCount = 0;
    renderFeedLine(QStringLiteral("schedule"), *section);
    section->retry->setEnabled(false);
    // The client is carried into the worker by value; the completion reaches
    // the page only through a guard.
    backend::MikaiClient* client = &client_;
    const QPointer<SearchPage> self(this);
    runner_.run(
        [client] { return QVariant::fromValue(client->getSchedule()); },
        [self, seq](const QVariant& result) {
            if (self.isNull() || seq != self->feedSeq_[QStringLiteral("schedule")]) {
                return;
            }
            self->rateLabel_->refresh();
            FeedSection* current = self->feedSection(QStringLiteral("schedule"));
            current->retry->setEnabled(true);
            // Today first, then the rest of the week in order: the top of a
            // schedule is only useful if it starts with what airs next.
            static const char* kDayKeys[] = {"monday", "tuesday", "wednesday", "thursday",
                                             "friday",  "saturday", "sunday"};
            const int today = QDate::currentDate().dayOfWeek() - 1;
            const backend::ScheduleResult schedule = result.value<backend::ScheduleResult>();
            // Every row keeps its weekday key rather than the label it was
            // painted with, so a language switch re-words the whole list.
            QList<std::pair<backend::AnimeSummary, FeedRowMeta>> rows;
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
                    FeedRowMeta meta;
                    meta.dayKey = QLatin1String(kDayKeys[day]);
                    meta.when = formatFeedTime(entry.airing, true);
                    meta.episode = labels::variant(entry.episode);
                    rows.append({entry.anime, meta});
                    if (rows.size() >= theme::kHomeFeedMaxRows) {
                        break;
                    }
                }
            }
            self->showFeedRows(QStringLiteral("schedule"), rows);
        },
        [self, seq](const backend::Failure& failure) {
            if (self.isNull() || seq != self->feedSeq_[QStringLiteral("schedule")]) {
                return;
            }
            self->rateLabel_->refresh();
            FeedSection* current = self->feedSection(QStringLiteral("schedule"));
            current->retry->setEnabled(true);
            current->rowsHost->hide();
            current->hasFailure = true;
            self->showBlockFailure(current->failure, current->state, failure,
                                   self->blockFailureContext(QStringLiteral("schedule")));
        });
}

void SearchPage::showFeedRows(
    const QString& key,
    const QList<std::pair<backend::AnimeSummary, FeedRowMeta>>& entries) {
    FeedSection* section = feedSection(key);
    while (section->rows->count() > 0) {
        QLayoutItem* item = section->rows->takeAt(0);
        if (item->widget() != nullptr) {
            item->widget()->deleteLater();
        }
        delete item;
    }
    feedPosterLabels_.insert(key, {});
    section->rowMeta.clear();
    section->rowMetaLabels.clear();
    section->failure->clear();
    section->hasFailure = false;
    section->line = entries.isEmpty() ? FeedSection::Line::Empty : FeedSection::Line::Count;
    section->lineCount = static_cast<int>(entries.size());
    renderFeedLine(key, *section);
    if (entries.isEmpty()) {
        section->state->show();
        section->rowsHost->hide();
        return;
    }
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
        const QString name = labels::titleName(anime.titles);
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
        auto* meta = new QLabel(composeFeedMeta(entry.second), row);
        meta->setObjectName(QStringLiteral("homeFeedMeta"));
        meta->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        rowLayout->addWidget(title, 1);
        rowLayout->addWidget(meta);
        section->rows->addWidget(row);
        // Kept with the row so the line can be re-worded in place: the row
        // widget itself, its poster and its order never change for a language.
        section->rowMeta.append(entry.second);
        section->rowMetaLabels.append(meta);
    }
    section->rowsHost->show();
    feedPosterLabels_.insert(key, posters);
    for (const FeedPosterSlot& poster : posters) {
        requestThumb(poster.url);
    }
}

void SearchPage::loadDictionaries() {
    const int seq = ++dictionarySeq_;
    backend::MikaiClient* client = &client_;
    const QPointer<SearchPage> self(this);
    runner_.run(
        [client] {
            QVariantMap data;
            data.insert(QStringLiteral("meta"), QVariant::fromValue(client->getMeta()));
            data.insert(QStringLiteral("genres"), QVariant::fromValue(client->getGenres()));
            data.insert(QStringLiteral("tags"), QVariant::fromValue(client->getTags()));
            data.insert(QStringLiteral("studios"), QVariant::fromValue(client->getStudios()));
            return data;
        },
        [self, seq](const QVariant& value) {
            if (self.isNull() || seq != self->dictionarySeq_) return;
            const QVariantMap data = value.toMap();
            self->apiMeta_ = data.value(QStringLiteral("meta")).value<backend::ApiMeta>();
            self->dictionaryError_.clear();
            self->genres_ = data.value(QStringLiteral("genres")).value<QVector<backend::GenreTag>>();
            self->tags_ = data.value(QStringLiteral("tags")).value<QVector<backend::GenreTag>>();
            self->studios_ = data.value(QStringLiteral("studios")).toStringList();
            self->filtersButton_->setToolTip(tr("Catalogue dictionaries loaded"));
            self->filtersButton_->setEnabled(true);
        },
        [self, seq](const backend::Failure& failure) {
            if (self.isNull() || seq != self->dictionarySeq_) return;
            self->dictionaryError_ = failure.message;
            self->filtersButton_->setToolTip(failure.message);
            self->filtersButton_->setEnabled(true);
            self->statusLine_ = Status::Failure;
            self->statusFailure_ = failure;
            self->renderStatus();
        });
    filtersButton_->setEnabled(false);
}

void SearchPage::showCatalogueFilters() {
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Catalogue filters"));
    dialog.setObjectName(QStringLiteral("catalogueFiltersDialog"));
    dialog.setMinimumSize(theme::kSettingsDialogMinWidth, theme::kSettingsDialogMinHeight);
    dialog.resize(theme::kCatalogueFiltersWidth, theme::kCatalogueFiltersHeight);
    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(theme::kSpacingLarge, theme::kSpacingMedium,
                               theme::kSpacingLarge, theme::kSpacingMedium);
    layout->setSpacing(theme::kSpacingMedium);
    auto* filterScroll = new QScrollArea(&dialog);
    if (!dictionaryError_.isEmpty()) {
        auto* dictionaryFailure = new QLabel(tr("Catalogue dictionaries failed: %1").arg(dictionaryError_), &dialog);
        dictionaryFailure->setObjectName(QStringLiteral("catalogueDictionaryFailure"));
        dictionaryFailure->setWordWrap(true);
        auto* retry = new QPushButton(tr("Retry dictionaries"), &dialog);
        retry->setObjectName(QStringLiteral("catalogueDictionaryRetryButton"));
        connect(retry, &QPushButton::clicked, this, [this, &dialog] {
            loadDictionaries();
            dialog.reject();
        });
        layout->addWidget(dictionaryFailure);
        layout->addWidget(retry);
    }
    filterScroll->setWidgetResizable(true);
    filterScroll->setFrameShape(QFrame::NoFrame);
    filterScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    filterScroll->setObjectName(QStringLiteral("catalogueFilterScroll"));
    auto* filterContent = new QWidget(filterScroll);
    auto* sections = new QVBoxLayout(filterContent);
    sections->setContentsMargins(0, theme::kSpacingSmall, theme::kSpacingSmall, 0);
    sections->setSpacing(theme::kSpacingMedium);
    filterScroll->setWidget(filterContent);
    const auto addSection = [&](const QString& title) {
        auto* box = new QGroupBox(title, filterContent);
        box->setObjectName(QStringLiteral("catalogueFilterSection"));
        auto* inner = new QVBoxLayout(box);
        inner->setContentsMargins(theme::kSpacingMedium, theme::kSpacingLarge,
                                  theme::kSpacingMedium, theme::kSpacingMedium);
        inner->setSpacing(theme::kSpacingSmall);
        sections->addWidget(box);
        return inner;
    };
    const auto addField = [](QVBoxLayout* section, const QString& caption, QWidget* field) {
        QWidget* owner = section->parentWidget();
        auto* block = new QWidget(owner);
        auto* column = new QVBoxLayout(block);
        column->setContentsMargins(0, 0, 0, 0);
        column->setSpacing(theme::kSpacingXs);
        auto* label = new QLabel(caption, block);
        label->setObjectName(QStringLiteral("muted"));
        column->addWidget(label);
        column->addWidget(field);
        section->addWidget(block);
    };
    // A filtered list keeps only the height its visible rows need, so narrowing
    // a long dictionary never leaves a scroll box of blank space behind.
    const auto fitChoiceList = [](QListWidget* list) {
        int shown = 0;
        for (int row = 0; row < list->count(); ++row) {
            if (!list->item(row)->isHidden()) ++shown;
        }
        list->setFixedHeight(theme::kCatalogueChoiceRowHeight * qBound(1, shown, 6) +
                             theme::kSpacingLarge);
    };
    const auto filterChoices = [fitChoiceList](QListWidget* list, const QString& term) {
        const QString needle = term.trimmed();
        for (int row = 0; row < list->count(); ++row) {
            QListWidgetItem* item = list->item(row);
            const bool matches =
                needle.isEmpty() ||
                item->text().contains(needle, Qt::CaseInsensitive) ||
                item->data(Qt::UserRole).toString().contains(needle, Qt::CaseInsensitive);
            item->setHidden(!matches);
        }
        fitChoiceList(list);
    };
    const auto addSectionSearch = [&](QVBoxLayout* section, const QString& objectName,
                                      const QString& placeholder,
                                      const QList<QListWidget*>& lists) {
        QWidget* owner = section->parentWidget();
        auto* search = new QLineEdit(owner);
        search->setObjectName(objectName);
        search->setPlaceholderText(placeholder);
        search->setClearButtonEnabled(true);
        section->addWidget(search);
        connect(search, &QLineEdit::textChanged, owner, [filterChoices, lists](const QString& term) {
            for (QListWidget* list : lists) filterChoices(list, term);
        });
        return search;
    };
    layout->addWidget(filterScroll, 1);
    const auto optionLabel = [](const QString& value) {
        if (value == QLatin1String("all")) return tr("Match all");
        if (value == QLatin1String("any")) return tr("Match any");
        if (value == QLatin1String("name")) return tr("Name");
        if (value == QLatin1String("year")) return tr("Year");
        if (value == QLatin1String("updated")) return tr("Recently updated");
        if (value == QLatin1String("views")) return tr("Views");
        if (value == QLatin1String("mal_rating")) return tr("MAL rating");
        if (value == QLatin1String("asc")) return tr("Ascending");
        if (value == QLatin1String("desc")) return tr("Descending");
        if (value == QLatin1String("winter")) return QCoreApplication::translate("Labels", "Winter");
        if (value == QLatin1String("spring")) return QCoreApplication::translate("Labels", "Spring");
        if (value == QLatin1String("summer")) return QCoreApplication::translate("Labels", "Summer");
        if (value == QLatin1String("autumn")) return QCoreApplication::translate("Labels", "Autumn");
        if (value == QLatin1String("unknown")) return labels::status(value);
        return value;
    };
    auto selectionMode = [&dialog, &optionLabel](const QString& objectName, const QStringList& options,
                                                const QString& current) {
        auto* box = new QComboBox(&dialog);
        box->setObjectName(objectName);
        QStringList values = options;
        if (!current.isEmpty() && !values.contains(current)) values << current;
        for (const QString& option : values) box->addItem(optionLabel(option), option);
        const int index = box->findData(current);
        if (index >= 0) box->setCurrentIndex(index);
        return box;
    };
    auto checkedList = [&dialog, fitChoiceList](const QString& objectName,
                                        const QStringList& labels,
                                        const QStringList& canonical,
                                        const QStringList& selected) {
        auto* list = new QListWidget(&dialog);
        list->setObjectName(objectName);
        list->setUniformItemSizes(true);
        QStringList seen;
        for (int i = 0; i < canonical.size(); ++i) {
            const QString value = canonical.at(i);
            auto* item = new QListWidgetItem(labels.value(i, value), list);
            item->setData(Qt::UserRole, value);
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(selected.contains(value) ? Qt::Checked : Qt::Unchecked);
            seen << value;
        }
        for (const QString& value : selected) {
            if (seen.contains(value)) continue;
            auto* item = new QListWidgetItem(value, list);
            item->setData(Qt::UserRole, value);
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(Qt::Checked);
        }
        fitChoiceList(list);
        return list;
    };
    const auto namesFor = [](const QVector<backend::GenreTag>& values) {
        QStringList labels, canonical;
        for (const backend::GenreTag& value : values) {
            labels << (value.ua.isEmpty() ? value.name : value.ua);
            canonical << value.name;
        }
        return qMakePair(labels, canonical);
    };
    const auto genreNames = namesFor(genres_);
    const auto tagNames = namesFor(tags_);
    auto* genreInclude = checkedList(QStringLiteral("genreIncludeList"), genreNames.first,
                                      genreNames.second, catalogueQuery_.genres);
    auto* genreExclude = checkedList(QStringLiteral("genreExcludeList"), genreNames.first,
                                      genreNames.second, catalogueQuery_.genres_exclude);
    auto* tagInclude = checkedList(QStringLiteral("tagIncludeList"), tagNames.first,
                                    tagNames.second, catalogueQuery_.tags);
    auto* tagExclude = checkedList(QStringLiteral("tagExcludeList"), tagNames.first,
                                    tagNames.second, catalogueQuery_.tags_exclude);
    QStringList matchModes = apiMeta_.match_modes;
    if (matchModes.isEmpty()) matchModes << QStringLiteral("all") << QStringLiteral("any");
    auto* genreMatch = selectionMode(QStringLiteral("genreMatchMode"), matchModes,
                                     catalogueQuery_.genres_match);
    auto* tagMatch = selectionMode(QStringLiteral("tagMatchMode"), matchModes,
                                   catalogueQuery_.tags_match);
    auto* genres = addSection(tr("Genres"));
    addSectionSearch(genres, QStringLiteral("genreFilterSearch"), tr("Search genres"),
                     {genreInclude, genreExclude});
    addField(genres, tr("Genres include"), genreInclude);
    addField(genres, tr("Genres exclude"), genreExclude);
    addField(genres, tr("Genre matching"), genreMatch);
    auto* tagsSection = addSection(tr("Tags"));
    addSectionSearch(tagsSection, QStringLiteral("tagFilterSearch"), tr("Search tags"),
                     {tagInclude, tagExclude});
    addField(tagsSection, tr("Tags include"), tagInclude);
    addField(tagsSection, tr("Tags exclude"), tagExclude);
    addField(tagsSection, tr("Tag matching"), tagMatch);

    auto* yearFrom = new QSpinBox(&dialog);
    yearFrom->setObjectName(QStringLiteral("yearFromSpin"));
    yearFrom->setRange(0, 9999);
    yearFrom->setSpecialValueText(tr("Any"));
    yearFrom->setValue(catalogueQuery_.year_from.value_or(0));
    auto* yearTo = new QSpinBox(&dialog);
    yearTo->setObjectName(QStringLiteral("yearToSpin"));
    yearTo->setRange(0, 9999);
    yearTo->setSpecialValueText(tr("Any"));
    yearTo->setValue(catalogueQuery_.year_to.value_or(0));
    auto* years = new QLineEdit(&dialog);
    years->setObjectName(QStringLiteral("yearsEdit"));
    years->setPlaceholderText(QStringLiteral("2020, 2021"));
    QStringList yearStrings;
    for (int year : catalogueQuery_.years) yearStrings << QString::number(year);
    years->setText(yearStrings.join(QLatin1Char(',')));
    auto* release = addSection(tr("Release"));
    auto* yearSpan = new QWidget(filterContent);
    auto* yearRow = new QHBoxLayout(yearSpan);
    yearRow->setContentsMargins(0, 0, 0, 0);
    yearRow->setSpacing(theme::kSpacingMedium);
    const auto addYear = [&](const QString& caption, QSpinBox* box) {
        box->setFixedWidth(theme::kCatalogueYearFieldWidth);
        auto* column = new QVBoxLayout();
        column->setSpacing(theme::kSpacingXs);
        auto* label = new QLabel(caption, yearSpan);
        label->setObjectName(QStringLiteral("muted"));
        column->addWidget(label);
        column->addWidget(box);
        yearRow->addLayout(column);
    };
    addYear(tr("Year from"), yearFrom);
    addYear(tr("Year to"), yearTo);
    yearRow->addStretch();
    release->addWidget(yearSpan);
    addField(release, tr("Years (comma separated)"), years);

    const auto translatedValues = [](const QStringList& values, const auto& label) {
        QStringList names;
        names.reserve(values.size());
        for (const QString& value : values) names.append(label(value));
        return names;
    };
    auto* formatList = checkedList(QStringLiteral("formatFilterList"),
                                    translatedValues(apiMeta_.formats, labels::format),
                                    apiMeta_.formats, catalogueQuery_.formats);
    auto* statusList = checkedList(QStringLiteral("statusFilterList"),
                                    translatedValues(apiMeta_.statuses, labels::status),
                                    apiMeta_.statuses, catalogueQuery_.statuses);
    auto* seasonList = checkedList(QStringLiteral("seasonFilterList"),
                                    translatedValues(apiMeta_.seasons, optionLabel),
                                    apiMeta_.seasons, catalogueQuery_.seasons);
    auto* studioList = checkedList(QStringLiteral("studioFilterList"), studios_, studios_,
                                    catalogueQuery_.studios);
    addField(release, tr("Formats"), formatList);
    addField(release, tr("Statuses"), statusList);
    addField(release, tr("Seasons"), seasonList);
    addField(release, tr("Studios"), studioList);
    // The lists this field narrows are built after the year range, so the field
    // is moved above them instead of being laid out where it was created.
    QLineEdit* releaseSearch =
        addSectionSearch(release, QStringLiteral("releaseFilterSearch"), tr("Search release options"),
                         {formatList, statusList, seasonList, studioList});
    release->insertWidget(0, releaseSearch);
    // Preserve selections when a newer API value is absent from the cached dictionary.
    QStringList sortFields = apiMeta_.anime_sort_fields;
    QStringList sortOrders = apiMeta_.sort_orders;
    if (sortFields.isEmpty()) sortFields << catalogueQuery_.sort;
    if (sortOrders.isEmpty()) sortOrders << QStringLiteral("asc") << QStringLiteral("desc");
    auto* sort = selectionMode(QStringLiteral("catalogueSortField"), sortFields,
                               catalogueQuery_.sort);
    auto* order = selectionMode(QStringLiteral("catalogueSortOrder"), sortOrders,
                                catalogueQuery_.order);
    auto* ordering = addSection(tr("Sorting"));
    addField(ordering, tr("Sort by"), sort);
    addField(ordering, tr("Order"), order);
    auto* relevance = new QLabel(tr("Text search uses relevance ranking; sort selection is retained but may be overridden."), &dialog);
    relevance->setObjectName(QStringLiteral("catalogueRelevanceHint"));
    relevance->setWordWrap(true);
    layout->addWidget(relevance);
    const auto* currentSearch = query_;
    relevance->setVisible(!currentSearch->text().trimmed().isEmpty());

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Reset |
                                          QDialogButtonBox::Cancel, &dialog);
    buttons->setObjectName(QStringLiteral("catalogueFilterButtons"));
    if (auto* apply = buttons->button(QDialogButtonBox::Apply)) {
        apply->setObjectName(QStringLiteral("catalogueApplyButton"));
        apply->setProperty("accentAction", true);
        const theme::Palette palette = theme::paletteFor(currentAppearance());
        apply->setStyleSheet(QStringLiteral(
            "QPushButton { background: %1; color: %2; border: none; font-weight: 600; "
            "border-radius: %3px; padding: 6px 16px; }"
            "QPushButton:hover { background: %4; }")
            .arg(palette.accent, palette.accentOnFill)
            .arg(theme::radiusSmall(currentAppearance()))
            .arg(palette.accentHover));
    }
    if (auto* reset = buttons->button(QDialogButtonBox::Reset))
        reset->setObjectName(QStringLiteral("catalogueResetButton"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::clicked, &dialog, [&dialog, buttons](QAbstractButton* button) {
        if (buttons->standardButton(button) == QDialogButtonBox::Reset) {
            dialog.setProperty("resetRequested", true);
            dialog.accept();
        } else if (buttons->standardButton(button) == QDialogButtonBox::Apply) {
            dialog.setProperty("applyRequested", true);
            dialog.accept();
        }
    });
    if (dialog.exec() == QDialog::Accepted) {
        if (dialog.property("resetRequested").toBool()) resetCatalogueFilters();
        else if (dialog.property("applyRequested").toBool()) {
            auto selected = [](QListWidget* list) {
                QStringList values;
                for (int row = 0; row < list->count(); ++row) {
                    auto* item = list->item(row);
                    if (item->checkState() == Qt::Checked) values << item->data(Qt::UserRole).toString();
                }
                return values;
            };
            catalogueQuery_.genres = selected(genreInclude);
            catalogueQuery_.genres_exclude = selected(genreExclude);
            catalogueQuery_.genres_match = genreMatch->currentData().toString();
            catalogueQuery_.tags = selected(tagInclude);
            catalogueQuery_.tags_exclude = selected(tagExclude);
            catalogueQuery_.tags_match = tagMatch->currentData().toString();
            catalogueQuery_.year_from = yearFrom->value() ? std::optional<int>(yearFrom->value()) : std::nullopt;
            catalogueQuery_.year_to = yearTo->value() ? std::optional<int>(yearTo->value()) : std::nullopt;
            catalogueQuery_.years.clear();
            for (const QString& part : years->text().split(QLatin1Char(','), Qt::SkipEmptyParts)) {
                bool ok = false; const int year = part.trimmed().toInt(&ok);
                if (ok) catalogueQuery_.years.append(year);
            }
            catalogueQuery_.formats = selected(formatList);
            catalogueQuery_.statuses = selected(statusList);
            catalogueQuery_.seasons = selected(seasonList);
            catalogueQuery_.studios = selected(studioList);
            catalogueQuery_.sort = sort->currentData().toString();
            catalogueQuery_.order = order->currentData().toString();
            applyCatalogueFilters();
        }
    }
}

void SearchPage::resetCatalogueFilters() {
    catalogueQuery_ = backend::AnimeQuery{};
    activeFormat_.clear();
    applyCatalogueFilters();
}

void SearchPage::applyCatalogueFilters() {
    ++requestSeq_;
    ++catalogueRequestSeq_;
    ++teamRequestSeq_;
    catalogueQuery_.search = query_->text().trimmed();
    catalogueQuery_.page = 1;
    catalogueQuery_.limit = pageSize_;
    currentQuery_ = catalogueQuery_.search;
    teamBrowseMode_ = false;
    if (teamBackButton_ != nullptr) teamBackButton_->hide();
    if (teamDetailLabel_ != nullptr) teamDetailLabel_->hide();
    browseMode_ = currentQuery_.isEmpty() && !hasExplicitCatalogueCriteria(catalogueQuery_);
    page_ = 1;
    fetchPage(1);
    showSearchResults();
}

void SearchPage::openTeamPicker() {
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Select a team"));
    dialog.setObjectName(QStringLiteral("teamPickerDialog"));
    dialog.setMinimumSize(theme::kSettingsDialogMinWidth, theme::kTeamPickerMinHeight);
    dialog.resize(theme::kSettingsDialogMinWidth, theme::kTeamPickerDefaultHeight);
    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(theme::kSpacingLarge, theme::kSpacingMedium,
                               theme::kSpacingLarge, theme::kSpacingMedium);
    layout->setSpacing(theme::kSpacingSmall);
    auto* search = new QLineEdit(&dialog);
    search->setObjectName(QStringLiteral("teamSearchEdit"));
    search->setPlaceholderText(tr("Search teams"));
    search->setClearButtonEnabled(true);
    auto* rows = new QListWidget(&dialog);
    rows->setObjectName(QStringLiteral("teamResultsList"));
    rows->setUniformItemSizes(true);
    auto* more = new QPushButton(tr("Load more"), &dialog);
    more->setObjectName(QStringLiteral("teamLoadMoreButton"));
    auto* browse = new QPushButton(tr("Browse anime"), &dialog);
    browse->setObjectName(QStringLiteral("teamBrowseButton"));
    browse->setEnabled(false);
    auto* filterTeam = new QPushButton(tr("Filter catalogue"), &dialog);
    filterTeam->setObjectName(QStringLiteral("teamFilterButton"));
    auto* state = new QLabel(&dialog);
    state->setObjectName(QStringLiteral("muted"));
    layout->addWidget(search);
    layout->addWidget(state);
    layout->addWidget(rows, 1);
    auto* buttons = new QHBoxLayout();
    buttons->setSpacing(theme::kSpacingSmall);
    buttons->addWidget(more);
    buttons->addStretch();
    buttons->addWidget(browse);
    buttons->addWidget(filterTeam);
    layout->addLayout(buttons);
    auto paging = std::make_shared<int>(1);
    auto totalPages = std::make_shared<int>(1);
    auto currentTerm = std::make_shared<QString>();
    auto request = std::make_shared<int>(0);
    auto selected = std::make_shared<backend::Team>();
    auto pickedTeamIds = std::make_shared<QSet<qint64>>();
    for (qint64 id : catalogueQuery_.teams) pickedTeamIds->insert(id);
    const QPointer<QListWidget> guardedRows(rows);
    const QPointer<QPushButton> guardedMore(more);
    const QPointer<QLabel> guardedState(state);
    backend::MikaiClient* client = &client_;
    auto fetch = [this, client, guardedRows, guardedMore, guardedState, paging, totalPages,
                  currentTerm, request, pickedTeamIds]
                 (const QString& term, int page) {
        *currentTerm = term;
        if (page == 1) {
            if (guardedRows) guardedRows->clear();
            *paging = 0;
            *totalPages = 1;
        }
        const int seq = ++*request;
        if (guardedState) guardedState->setText(tr("Loading…"));
        if (guardedMore) guardedMore->setEnabled(false);
        runner_.run([client, term, page] {
            return QVariant::fromValue(client->getTeams(term, page, 50));
        }, [guardedRows, guardedMore, guardedState, paging, totalPages, request, seq, page,
            pickedTeamIds]
             (const QVariant& v) {
            if (seq != *request || !guardedRows) return;
            const backend::TeamsResult result = v.value<backend::TeamsResult>();
            for (const auto& profile : result.items) {
                // Populate off-list: itemChanged would erase seeded checks during initialization.
                auto* item = new QListWidgetItem(profile.team.name);
                item->setData(Qt::UserRole, QVariantMap{
                    {QStringLiteral("id"), profile.team.id},
                    {QStringLiteral("slug"), profile.team.slug},
                    {QStringLiteral("name"), profile.team.name}});
                item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
                item->setCheckState(pickedTeamIds->contains(profile.team.id)
                                        ? Qt::Checked : Qt::Unchecked);
                guardedRows->addItem(item);
            }
            *paging = page;
            const int total = result.total.value_or(guardedRows->count());
            *totalPages = result.pages.value_or(
                result.total.has_value() ? qMax(1, (total + 49) / 50)
                                         : page + (result.items.size() == 50 ? 1 : 0));
            if (guardedState)
                guardedState->setText(QObject::tr("%1 teams · page %2 of %3")
                                          .arg(total).arg(page).arg(*totalPages));
            if (guardedMore) guardedMore->setEnabled(page < *totalPages);
        }, [guardedState, guardedMore, request, seq](const backend::Failure& failure) {
            if (seq != *request) return;
            if (guardedState) guardedState->setText(failure.message);
            if (guardedMore) guardedMore->setEnabled(true);
        });
    };
    connect(search, &QLineEdit::textChanged, &dialog,
            [fetch](const QString& term) { fetch(term, 1); });
    connect(more, &QPushButton::clicked, &dialog, [fetch, paging, totalPages, currentTerm] {
        if (*paging < *totalPages) fetch(*currentTerm, *paging + 1);
    });
    connect(rows, &QListWidget::itemChanged, &dialog, [pickedTeamIds](QListWidgetItem* item) {
        const qint64 id = item->data(Qt::UserRole).toMap().value(QStringLiteral("id")).toLongLong();
        if (item->checkState() == Qt::Checked) pickedTeamIds->insert(id);
        else pickedTeamIds->remove(id);
    });
    connect(rows, &QListWidget::currentItemChanged, &dialog, [selected, browse](QListWidgetItem* item) {
        *selected = backend::Team{};
        browse->setEnabled(item != nullptr);
        if (item == nullptr) return;
        const QVariantMap data = item->data(Qt::UserRole).toMap();
        selected->id = data.value(QStringLiteral("id")).toLongLong();
        selected->slug = data.value(QStringLiteral("slug")).toString();
        selected->name = data.value(QStringLiteral("name")).toString();
    });
    bool filterSelected = false;
    connect(browse, &QPushButton::clicked, &dialog, [&dialog, selected] {
        if (!selected->name.isEmpty()) dialog.accept();
    });
    fetch(QString(), 1);
    connect(filterTeam, &QPushButton::clicked, &dialog, [&dialog, &filterSelected] {
        filterSelected = true;
        dialog.accept();
    });
    if (dialog.exec() == QDialog::Accepted && (filterSelected || !selected->name.isEmpty())) {
        if (filterSelected) {
            catalogueQuery_.teams.clear();
            for (qint64 id : *pickedTeamIds) catalogueQuery_.teams.append(id);
            applyCatalogueFilters();
        } else {
            browseSelectedTeam(*selected);
        }
    }
}

void SearchPage::browseTeam(const QString& ref) {
    if (ref.trimmed().isEmpty()) return;
    const int seq = ++teamRequestSeq_;
    backend::MikaiClient* client = &client_;
    const QPointer<SearchPage> self(this);
    runner_.run([client, ref] { return QVariant::fromValue(client->getTeam(ref)); },
        [self, seq](const QVariant& value) {
            if (self.isNull() || seq != self->teamRequestSeq_) return;
            const backend::TeamProfile profile = value.value<backend::TeamProfile>();
            self->activeTeamProfile_ = profile;
            self->browseSelectedTeam(profile.team);
        }, [self, seq](const backend::Failure& failure) {
            if (self.isNull() || seq != self->teamRequestSeq_) return;
            self->statusLine_ = Status::Failure;
            self->statusFailure_ = failure;
            self->renderStatus();
        });
}

void SearchPage::browseSelectedTeam(const backend::Team& team) {
    ++requestSeq_;
    ++catalogueRequestSeq_;
    const bool hasCachedProfile = activeTeamProfile_.team.id == team.id &&
                                  !activeTeamProfile_.team.name.isEmpty();
    if (!hasCachedProfile) activeTeamProfile_ = backend::TeamProfile{};
    activeTeam_ = team;
    teamBrowseMode_ = true;
    resultsHeading_->setText(tr("Anime from %1").arg(team.name));
    showSearchResults();
    if (teamBackButton_ == nullptr) {
        teamBackButton_ = new QPushButton(tr("All catalogue"), this);
        teamBackButton_->setObjectName(QStringLiteral("teamBackButton"));
        teamDetailLabel_ = new QLabel(this);
        teamDetailLabel_->setObjectName(QStringLiteral("teamDetailLabel"));
        teamDetailLabel_->setWordWrap(true);
        teamDetailLabel_->setTextFormat(Qt::RichText);
        teamDetailLabel_->setOpenExternalLinks(false);
        teamDetailLabel_->setTextInteractionFlags(Qt::TextBrowserInteraction);
        connect(teamDetailLabel_, &QLabel::linkActivated, this, [this](const QString& target) {
            const QUrl url(target);
            if (!url.isValid() || url.host().isEmpty() ||
                (url.scheme() != QLatin1String("http") && url.scheme() != QLatin1String("https"))) {
                return;
            }
            if (!QDesktopServices::openUrl(url)) {
                reportFailure(Notice::Browser);
            }
        });
        if (auto* root = qobject_cast<QVBoxLayout*>(layout())) {
            root->insertWidget(2, teamBackButton_);
            root->insertWidget(3, teamDetailLabel_);
        }
        connect(teamBackButton_, &QPushButton::clicked, this, [this] {
            teamBrowseMode_ = false;
            ++requestSeq_;
            ++catalogueRequestSeq_;
            teamBackButton_->hide();
            teamDetailLabel_->hide();
            applyCatalogueFilters();
        });
    }
    teamBackButton_->setText(tr("All catalogue"));
    teamBackButton_->show();
    teamDetailLabel_->show();
    if (hasCachedProfile) {
        renderTeamProfile();
    } else {
        teamDetailLabel_->setText(tr("Loading team…").toHtmlEscaped());
        const int seq = ++teamRequestSeq_;
        const QString ref = team.slug.isEmpty() ? QString::number(team.id) : team.slug;
        // The client travels into the worker by value; the page is reached only
        // through a guard.
        backend::MikaiClient* client = &client_;
        const QPointer<SearchPage> self(this);
        runner_.run([client, ref] { return QVariant::fromValue(client->getTeam(ref)); },
            [self, seq](const QVariant& value) {
                if (self.isNull() || seq != self->teamRequestSeq_ || !self->teamBrowseMode_) return;
                self->activeTeamProfile_ = value.value<backend::TeamProfile>();
                self->renderTeamProfile();
            }, [self, seq](const backend::Failure& failure) {
                if (self.isNull() || seq != self->teamRequestSeq_) return;
                self->teamDetailLabel_->setText(failure.message.toHtmlEscaped());
            });
    }
    loadTeamAnime(1);
}

void SearchPage::renderTeamProfile() {
    if (teamDetailLabel_ == nullptr || activeTeamProfile_.team.name.isEmpty()) return;
    const backend::TeamProfile& profile = activeTeamProfile_;
    QStringList details{profile.team.name.toHtmlEscaped()};
    if (!profile.description.isEmpty()) {
        details << profile.description.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>"));
    }
    if (!profile.birthday.isEmpty()) details << tr("Birthday: %1").arg(profile.birthday).toHtmlEscaped();
    details << tr("%1 anime · %2 total views · %3 views this month")
                   .arg(profile.anime_count).arg(profile.views_total).arg(profile.views_last_30_days)
                   .toHtmlEscaped();
    if (profile.has_voice) details << tr("Voice").toHtmlEscaped();
    if (profile.has_subs) details << tr("Subtitles").toHtmlEscaped();
    details << renderTeamLinks();
    teamDetailLabel_->setText(details.join(QStringLiteral("<br>")));
}

QStringList SearchPage::renderTeamLinks() const {
    QStringList rows;
    for (auto it = activeTeamProfile_.links.cbegin(); it != activeTeamProfile_.links.cend(); ++it) {
        QString service;
        if (it.key() == QLatin1String("site") || it.key() == QLatin1String("website")) {
            service = tr("Website");
        } else if (it.key() == QLatin1String("donate")) {
            service = tr("Donate");
        } else {
            service = backend::teamLinkServiceName(it.key());
        }
        service = service.toHtmlEscaped();
        const QUrl url(it.value());
        const bool openable = url.isValid() && !url.host().isEmpty() &&
                              (url.scheme() == QLatin1String("http") ||
                               url.scheme() == QLatin1String("https"));
        if (!openable) {
            rows << QStringLiteral("%1: %2").arg(service, it.value().toHtmlEscaped());
            continue;
        }
        rows << QStringLiteral("<a style=\"color: %1; text-decoration: none;\" href=\"%2\">%3</a>")
                    .arg(theme::paletteFor(currentAppearance()).accent,
                         it.value().toHtmlEscaped(), service);
    }
    return rows;
}

void SearchPage::loadTeamAnime(int page) {
    const int seq = ++catalogueRequestSeq_;
    const backend::Team team = activeTeam_;
    const int limit = pageSize_;
    statusLine_ = Status::LoadingSearch;
    renderStatus();
    backend::MikaiClient* client = &client_;
    const QPointer<SearchPage> self(this);
    runner_.run([client, team, page, limit] {
        return QVariant::fromValue(client->getTeamAnime(team.slug.isEmpty() ? QString::number(team.id) : team.slug,
                                                        page, limit));
    }, [self, seq, page](const QVariant& value) {
        if (self.isNull() || seq != self->catalogueRequestSeq_ || !self->teamBrowseMode_) return;
        self->onResults(value.value<backend::SearchResult>(), page);
    }, [self, seq](const backend::Failure& failure) {
        if (self.isNull()) return;
        if (seq == self->catalogueRequestSeq_) self->onError(failure);
    });
}
void SearchPage::fetchPage(int page) {
    statusLine_ = browseMode_ ? Status::LoadingBrowse : Status::LoadingSearch;
    renderStatus();
    runResultsRequest(page);
}

void SearchPage::runResultsRequest(int page) {
    if (teamBrowseMode_) { loadTeamAnime(page); return; }
    requestSeq_ += 1;
    const int seq = requestSeq_;
    // The request is put together on the GUI thread, so the worker carries the
    // finished query instead of rebuilding it from the page.
    backend::AnimeQuery request = catalogueQuery_;
    request.search = currentQuery_;
    request.page = page;
    request.limit = pageSize_;
    if (!activeFormat_.isEmpty() && !request.formats.contains(activeFormat_)) {
        request.formats << activeFormat_;
    }
    // Every catalogue request names its own criteria. The browse calls carry
    // the home carousels' fixed criteria - currently airing, newest episode
    // first - as values of their own, so a catalogue request routed through
    // one of them answered with the home ordering and the criterion the
    // viewer chose (a sort, an order, a filter) never applied. The home
    // carousel blocks keep their own browse calls; the catalogue does not use
    // them.
    backend::MikaiClient* client = &client_;
    const QPointer<SearchPage> self(this);
    runner_.run(
        [client, request] { return QVariant::fromValue(client->search(request)); },
        [self, seq, page](const QVariant& result) {
            if (self.isNull() || seq != self->requestSeq_) {
                return;
            }
            self->onResults(result.value<backend::SearchResult>(), page);
        },
        [self, seq](const backend::Failure& failure) {
            if (self.isNull() || seq != self->requestSeq_) {
                return;
            }
            self->onError(failure);
        });
}

void SearchPage::search() {
    const QString query = query_->text().trimmed();
    if (query.isEmpty()) {
        // An empty text with a criterion still chosen is a catalogue query, not
        // a return to the home collection: the sort or the order the filters
        // hold would be dropped otherwise.
        if (hasExplicitCatalogueCriteria(catalogueQuery_)) applyCatalogueFilters();
        else loadOngoing();
        return;
    }
    browseMode_ = false;
    ++teamRequestSeq_;
    teamBrowseMode_ = false;
    if (teamBackButton_ != nullptr) teamBackButton_->hide();
    if (teamDetailLabel_ != nullptr) teamDetailLabel_->hide();
    currentQuery_ = query;
    activeFormat_.clear();
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
    if (homeButton_ != nullptr) homeButton_->hide();
    // Thumbs that landed while the feeds were hidden were left unpainted, so
    // the switch back is the moment to catch them up.
    applyFeedPosters();
}

void SearchPage::showSearchResults() {
    discoveryScroll_->hide();
    results_->show();
    resultsHeading_->show();
    if (homeButton_ != nullptr) homeButton_->show();
    applyThumbs();
}

void SearchPage::onResults(const backend::SearchResult& result, int requestedPage) {
    rateLabel_->refresh();
    page_ = result.page.value_or(requestedPage);
    entries_ = result.items;
    total_ = result.total.value_or(entries_.size());
    // The server strides pages at the size it actually served, which is not
    // always the one asked for, so its own count is the authority. Falling back
    // to a local count is only safe when the same size was served back.
    pages_ = result.pages.value_or(qMax(1, (total_ + pageSize_ - 1) / pageSize_));
    if (pages_ < page_) {
        pages_ = page_;
    }
    // A different query, or the same query under a filter, is a different result
    // set, so its cached counts describe nothing the bar now shows.
    if (!activeFormat_.isEmpty() || formatTotalsQuery_ != currentQuery_) {
        formatTotals_.clear();
        formatTotalsSeq_ += 1;
        formatTotalsQuery_ = currentQuery_;
    }
    statusLine_ = Status::Results;
    renderStatus();
    if (teamBrowseMode_) {
        formatBar_->hide();
    } else {
        rebuildFormatBar();
        refreshFormatTotals();
    }
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
        // The API's own filter values are lower case, and the server only
        // accepts those, so a row's spelling is normalised before it is offered
        // as a tab or sent back as a filter.
        const QString format =
            entry.format.isEmpty() ? QStringLiteral("unknown") : entry.format.toLower();
        counts[format] += 1;
    }
    QStringList present;
    for (const QString& format : kOrder) {
        if (counts.value(format) > 0) {
            present << format;
        }
    }
    // A filtered answer carries the format that was asked for and nothing else.
    // Hiding the bar on that single-format response would strand the reader in
    // the filter: the control that asked for it, and the All escape that undoes
    // it, would both leave the screen. The active format therefore keeps its tab
    // (counting zero when the filtered page came back empty) and All sits beside
    // it, so the bar stays on screen exactly while there is a filter to escape.
    const bool filtering = !activeFormat_.isEmpty();
    if (filtering && !present.contains(activeFormat_)) {
        present << activeFormat_;
    }
    // Signals stay blocked for the whole rebuild: removing and re-adding tabs
    // moves the current index, and an unblocked move would look like the reader
    // picking a filter and start a new request over a language switch.
    formatBar_->blockSignals(true);
    while (formatBar_->count() > 0) {
        formatBar_->removeTab(0);
    }
    if (present.size() >= 2 || filtering) {
        formatBar_->addTab(tr("All (%1)").arg(total_));
        formatBar_->setTabData(0, QStringLiteral("all"));
        for (const QString& format : present) {
            // The count for a format is the result set's own once it has been
            // asked for; until then the page's figure is the honest answer.
            const int count = formatTotals_.value(format, counts.value(format));
            formatBar_->addTab(QStringLiteral("%1 (%2)").arg(format.toUpper()).arg(count));
            formatBar_->setTabData(formatBar_->count() - 1, format);
        }
        // The request that just landed may have carried the filter, so the tab
        // it belongs to has to be the one shown rather than "All".
        int restore = 0;
        if (filtering) {
            restore = formatBar_->count() - 1;
            while (restore > 0 && formatBar_->tabData(restore).toString() != activeFormat_) {
                --restore;
            }
            if (formatBar_->tabData(restore).toString() != activeFormat_) {
                // Unreachable while the active format is offered above, but the
                // state must never claim a filter the bar cannot show.
                restore = 0;
                activeFormat_.clear();
            }
        }
        formatBar_->setCurrentIndex(restore);
        formatBar_->show();
    } else {
        formatBar_->hide();
    }
    formatBar_->blockSignals(false);
}

void SearchPage::applyFormatFilter() {
    const int index = formatBar_->currentIndex();
    if (index < 0) {
        return;
    }
    // The filter is the server's, so picking a format is a new request over the
    // whole result set, not a re-read of the single page in hand.
    activeFormat_ = formatBar_->tabData(index).toString();
    if (activeFormat_ == QLatin1String("all")) {
        activeFormat_.clear();
    }
    fetchPage(1);
}

void SearchPage::refreshFormatTotals() {
    // Browse and filtered results already describe their own total, and only a
    // search over several formats has counts worth asking for.
    if (browseMode_ || currentQuery_.isEmpty() || !formatBar_->isVisible()) {
        return;
    }
    QStringList formats;
    for (int index = 0; index < formatBar_->count(); ++index) {
        const QString value = formatBar_->tabData(index).toString();
        if (value != QLatin1String("all")) {
            formats << value;
        }
    }
    if (formats.isEmpty()) {
        return;
    }
    const QString query = currentQuery_;
    const backend::AnimeQuery baseQuery = catalogueQuery_;
    const int seq = ++formatTotalsSeq_;
    formatTotalsQuery_ = query;
    backend::MikaiClient* client = &client_;
    const QPointer<SearchPage> self(this);
    runner_.run(
        [client, query, formats, baseQuery] {
            QHash<QString, int> totals;
            for (const QString& format : formats) {
                backend::AnimeQuery request = baseQuery;
                request.search = query;
                request.page = 1;
                request.limit = 1;
                request.formats = {format};
                const backend::SearchResult one = client->search(request);
                if (one.total.has_value()) {
                    totals.insert(format, *one.total);
                }
            }
            return QVariant::fromValue(totals);
        },
        [self, seq, query](const QVariant& result) {
            if (self.isNull() || seq != self->formatTotalsSeq_ || query != self->formatTotalsQuery_) {
                return;
            }
            self->formatTotals_ = result.value<QHash<QString, int>>();
            self->rebuildFormatBar();
        },
        [](const backend::Failure&) {
            // A count is a label, not a result: leaving the page-local figure in
            // place beats an empty bar.
        });
}

void SearchPage::populateRows() {
    results_->clear();
    for (const backend::AnimeSummary& entry : entries_) {
        const QString label = resultRowText(entry);
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

void SearchPage::onError(const backend::Failure& failure) {
    rateLabel_->refresh();
    reportFailure(failure);
    updatePager();
}

void SearchPage::reportFailure(const backend::Failure& failure) {
    // The failure itself is kept, not just its line: a language switch restates
    // the app's own wording for the code and leaves the backend's line alone.
    statusLine_ = Status::Failure;
    statusFailure_ = failure;
    renderStatus();
    QAccessibleEvent event(status_, QAccessible::Alert);
    QAccessible::updateAccessibility(&event);
}

void SearchPage::reportFailure(Notice notice) {
    statusLine_ = Status::Notice;
    statusNotice_ = notice;
    renderStatus();
    QAccessibleEvent event(status_, QAccessible::Alert);
    QAccessible::updateAccessibility(&event);
}

void SearchPage::showBlockFailure(FailureView* view, QLabel* state, const backend::Failure& failure,
                                  const QString& context) {
    state->hide();
    view->showFailure(failure, context);
}

void SearchPage::clearBlockFailure(FailureView* view, QLabel* state) {
    view->clear();
    state->show();
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
    connect(copyLink, &QAction::triggered, this, [this, ref] {
        if (!mikai::gui::copyText(pageUrlFor(ref))) reportFailure(Notice::CopyLink);
    });
    QAction* copyRef = menu.addAction(tr("Copy ID"));
    copyRef->setToolTip(tr("Copy the title identifier"));
    connect(copyRef, &QAction::triggered, this, [this, ref] {
        if (!mikai::gui::copyText(ref)) reportFailure(Notice::CopyId);
    });
    menu.exec(results_->mapToGlobal(position));
}

void SearchPage::animeCardMenu(const QString& ref, const QPoint& globalPos) {
    QMenu menu(this);
    QAction* open = menu.addAction(tr("Open"));
    open->setToolTip(tr("Open the title page"));
    connect(open, &QAction::triggered, this, [this, ref] { openByRef(ref); });
    QAction* copyLink = menu.addAction(tr("Copy link"));
    copyLink->setToolTip(tr("Copy the page address on mikai.me"));
    connect(copyLink, &QAction::triggered, this, [this, ref] {
        if (!mikai::gui::copyText(pageUrlFor(ref))) reportFailure(Notice::CopyLink);
    });
    QAction* copyRef = menu.addAction(tr("Copy ID"));
    copyRef->setToolTip(tr("Copy the title identifier"));
    connect(copyRef, &QAction::triggered, this, [this, ref] {
        if (!mikai::gui::copyText(ref)) reportFailure(Notice::CopyId);
    });
    QAction* browser = menu.addAction(tr("Open in browser"));
    browser->setToolTip(tr("Open the page on mikai.me"));
    connect(browser, &QAction::triggered, this, [this, ref] {
        if (!QDesktopServices::openUrl(QUrl(pageUrlFor(ref)))) {
            reportFailure(Notice::Browser);
        }
    });
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
    if (pendingThumbs_.contains(url)) return;
    thumbsInFlight_.insert(url);
    // The client travels into the worker by value; the page is reached only
    // through a guard, so a poster that lands after the page was closed is
    // dropped rather than painted into a page that is gone.
    backend::MikaiClient* client = &client_;
    const QPointer<SearchPage> self(this);
    runner_.runImage(
        [client, url] {
            return QVariant::fromValue(gui::decodeImage(client->fetchBytes(url), kThumbDecodeSize));
        },
        [self, url](const QVariant& result) {
            if (self.isNull()) {
                return;
            }
            self->thumbsInFlight_.remove(url);
            auto decoded = result.value<gui::DecodedImage>();
            if (decoded.image.isNull()) {
                qWarning().noquote() << "SearchPage: thumbnail decode failed:" << decoded.error;
                return;
            }
            self->applyThumb(url, std::move(decoded.image));
        },
        [self, url](const backend::Failure&) {
            if (self.isNull()) {
                return;
            }
            self->thumbsInFlight_.remove(url);
        });
}

void SearchPage::applyThumb(const QString& url, QImage image) {
    pendingThumbs_.insert(url, std::move(image));
    thumbTimer_->start();
}

void SearchPage::flushThumbs() {
    if (pendingThumbs_.isEmpty()) {
        return;
    }
    // QPixmap creation stays on the GUI thread, with one image published per turn.
    auto it = pendingThumbs_.begin();
    const QString key = it.key();
    QImage image = std::move(it.value());
    pendingThumbs_.erase(it);
    if (!icons_.contains(key)) {
        const QPixmap pixmap = QPixmap::fromImage(std::move(image));
        if (!pixmap.isNull()) {
            icons_.insert(key, pixmap);
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

void SearchPage::refreshCardFrames() {
    // Rounded frames bake the corner radius and the cover shade, so a change to
    // either invalidates every cached one; the decoded icons stay reusable.
    rounded_.clear();
    for (auto it = cardsByImageKey_.cbegin(); it != cardsByImageKey_.cend(); ++it) {
        const auto icon = icons_.constFind(it.key());
        if (icon == icons_.constEnd()) continue;
        const QPixmap ready = TitleCard::rounded(
            icon.value(), QSize(theme::kHomePosterWidth, theme::kHomePosterHeight));
        rounded_.insert(it.key(), ready);
        for (TitleCard* card : it.value()) {
            card->applyPixmap(ready);
        }
    }
    applyThumbs();
    applyFeedPosters();
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
