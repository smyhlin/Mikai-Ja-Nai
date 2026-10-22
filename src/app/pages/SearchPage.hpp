#include <utility>
#pragma once

#include <backend/Errors.hpp>
#include <backend/Models.hpp>
#include <backend/UpdateFeed.hpp>

#include <QHash>
#include <QList>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

class QAction;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPixmap;
class QPushButton;
class QScrollArea;
class QScrollBar;
class QTabBar;
class QTimer;
class QVBoxLayout;
class QWidget;

namespace mplayer::shell {
class KeyHints;
}

namespace mikai::backend {
class MikaiClient;
}

namespace mikai::app {

class PageGlow;


class FailureView;
class RangePager;
class RateLimitLabel;
class Runner;
class TitleCard;

/// Home discovery (carousels + feed rows) and search results, with a
/// range-chip pager for the latter.
///
/// A list response carries only the planned episode count, so every card also
/// gets one detail request for its aired fraction. Those go out as their own
/// tasks on the bulk pool, so the pool works through a whole feed at the
/// client's advertised rate instead of one card blocking the rest.
class SearchPage : public QWidget {
    Q_OBJECT

public:
    SearchPage(Runner& runner, backend::MikaiClient& client, QWidget* parent = nullptr);
    ~SearchPage() override;

    /// Apply a settings-dialog change: a feed switched off stops loading and
    /// hides, one switched on loads. Results still in flight for a hidden feed
    /// are dropped.
    void refreshFeedSettings();
    /// Open a team's profile and paged anime catalogue.
    void browseTeam(const QString& ref);
    void returnHome();

    /// Put a title at the head of the "recently opened" carousel and persist it.
    void recordRecentTitle(const QString& ref, const QString& title, const QString& poster);

    /// The detail already fetched for `ref`, if any. A card's detail lookup
    /// lands for every title on the page, so opening one usually has the answer
    /// in hand and the title page can skip asking for it again.
    const backend::AnimeDetail* cachedDetail(const QString& ref) const {
        const auto it = cardDetails_.constFind(ref);
        return it == cardDetails_.constEnd() ? nullptr : &it.value();
    }

    /// Closes the keyboard-shortcuts overlay when one is open. A focus-stealing
    /// overlay left open under modal Settings keeps keyboard focus while the
    /// dialog owns the window.
    void closeTransientOverlays();
    /// Closes transient hints before returning to home discovery.
    bool escapeSearch();

Q_SIGNALS:
    void openRequested(const QString& ref, const QString& poster);
    void settingsRequested();
    void mediaFileRequested();
    void mediaUrlRequested();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    /// One carousel block: heading, status line, retry button and the track.
    struct CarouselSection {
        QWidget* section = nullptr;
        QLabel* heading = nullptr;
        QHBoxLayout* cards = nullptr;
        QScrollArea* scroll = nullptr;
        QLabel* state = nullptr;
        FailureView* failure = nullptr;
        QPushButton* previous = nullptr;
        QPushButton* next = nullptr;
        /// What the block's own line currently says, kept as the state it
        /// describes rather than as the words it was painted with, so a
        /// language switch restates the line without re-reading the collection.
        enum class Line { Unloaded, Loading, Empty, Count };
        Line line = Line::Unloaded;
        int lineCount = 0;
        /// True while the surface owns a failure. The failure itself is the
        /// surface's to keep: only the clause this page wrote is re-sent.
        bool hasFailure = false;
    };

    /// One list row's meta line as the raw parts it was built from. The parts
    /// are composed into a sentence at render time, so a language switch
    /// re-words the row where it stands instead of re-fetching the feed.
    struct FeedRowMeta {
        /// The schedule names its weekday first, and an updates row leaves this
        /// empty: that is also what decides the order of the parts.
        QString dayKey;
        /// The API's own episode label, empty when the release carries none.
        QString episode;
        /// Updates: the release is unnumbered, so it reads "New episode".
        bool newEpisode = false;
        /// API data, shown exactly as it arrived.
        QString team;
        /// Updates: the API's release kind, worded by the app.
        QString kind;
        /// API data, shown capitalised.
        QString provider;
        /// Clock text, formatted once: digits, never words.
        QString when;
    };

    /// One list feed block (updates, schedule): heading, state line and rows.
    struct FeedSection {
        QWidget* section = nullptr;
        QLabel* heading = nullptr;
        QLabel* state = nullptr;
        QPushButton* retry = nullptr;
        FailureView* failure = nullptr;
        QWidget* rowsHost = nullptr;
        QVBoxLayout* rows = nullptr;
        /// The line currently shown, kept as state so a language switch restates
        /// it instead of re-running the request that filled it.
        enum class Line { Loading, Empty, Count };
        Line line = Line::Loading;
        int lineCount = 0;
        /// True while the surface owns a failure. The failure itself is the
        /// surface's to keep: only the clause this page wrote is re-sent.
        bool hasFailure = false;
        /// Every row's meta line and the label painting it, parallel to the row
        /// widgets: enough to re-word the rows where they stand.
        QVector<FeedRowMeta> rowMeta;
        QVector<QPointer<QLabel>> rowMetaLabels;
    };

    /// What the status line currently stands for, kept as state rather than as
    /// the sentence it was rendered as.
    enum class Status { BrowseHint, LoadingBrowse, LoadingSearch, Results, Failure, Notice };

    /// The app-generated refusals a status line can name.
    enum class Notice { CopyLink, CopyId, Browser };

    /// A feed poster waiting on one thumb URL, remembering which URL its label
    /// already shows so a repeated pass over the icon table is a no-op.
    struct FeedPosterSlot {
        QString url;
        QPointer<QLabel> label;
        QString shown;
    };

    void smoothScrollCarousel(QScrollBar* bar, int delta);
    CarouselSection* buildCarousel(const QString& key);
    FeedSection* buildFeedSection(const QString& key);
    CarouselSection* carouselSection(const QString& key) const;
    FeedSection* feedSection(const QString& key) const;

    /// Restates every static caption in place after a LanguageChange: the
    /// headings, the buttons and their accessible names, the query placeholder,
    /// each block's own line and the meta line of every feed row. Nothing is
    /// re-read and nothing is requested - the query, the result page, the
    /// format filter, the cards and the loaded feeds stay exactly as they are.
    void retranslateUi();
    /// The heading of the block `key` names, in the active language. Both the
    /// constructor and a retranslate pass take it from here, so the two can
    /// never drift apart.
    QString carouselHeading(const QString& key) const;
    QString feedHeading(const QString& key) const;
    /// The empty and counted wording of a feed's own line, by key.
    QString feedEmptyLine(const QString& key) const;
    QString feedCountLine(const QString& key, int count) const;
    /// The leading clause a block's failure surface shows, by key.
    QString blockFailureContext(const QString& key) const;
    /// The three-letter weekday a schedule row starts with.
    QString feedDayLabel(const QString& dayKey) const;
    /// A block's own line, restated from the state it holds. Visibility is left
    /// alone: the failure surface, not this pass, decides which of the two is
    /// on screen.
    void renderCarouselLine(CarouselSection& section);

    /// Rebuilds cached poster frames after the appearance changed its radius
    /// or its cover shade.
    void refreshCardFrames();
    void renderFeedLine(const QString& key, FeedSection& section);
    /// The meta line of every row the block is showing, re-worded where it
    /// stands from the parts each row was built from.
    void renderFeedRows(FeedSection& section);
    /// The status line, restated from what it currently stands for.
    void renderStatus();
    /// The notice a refused hand-off reports, in the active language.
    QString noticeText(Notice notice) const;
    /// One feed row's meta line, composed from its raw parts.
    QString composeFeedMeta(const FeedRowMeta& meta) const;
    /// One search row's text, composed from the summary it shows.
    QString resultRowText(const backend::AnimeSummary& entry) const;

    QList<backend::AnimeSummary> readRecentTitles();
    void persistRecentTitles();
    void showCarousel(const QString& key, const QList<backend::AnimeSummary>& items);
    void resolveRecentPosters(const QList<backend::AnimeSummary>& entries);
    void requestCardDetail(const QString& ref);
    void applyCardDetail(const QString& ref, const backend::AnimeDetail& detail);
    void queueRecentPosterHeal(const QString& ref, const backend::AnimeDetail& detail);
    void flushRecentPosters();

    void loadFeed(const QString& key);
    void loadOngoing();
    void requestRecommendation(const QString& key);
    void loadUpdates();
    void loadSchedule();
    void showFeedRows(const QString& key,
                      const QList<std::pair<backend::AnimeSummary, FeedRowMeta>>& entries);

    void fetchPage(int page);
    void runResultsRequest(int page);
    void loadDictionaries();
    void showCatalogueFilters();
    void applyCatalogueFilters();
    void resetCatalogueFilters();
    void openTeamPicker();
    void browseSelectedTeam(const backend::Team& team);
    void renderTeamProfile();
    QStringList renderTeamLinks() const;
    void loadTeamAnime(int page);
    void search();
    void showDiscovery();
    void showSearchResults();
    void onResults(const backend::SearchResult& result, int requestedPage);
    void onError(const backend::Failure& failure);
    void reportFailure(const backend::Failure& failure);
    /// A refusal this page generated itself - a clipboard or browser hand-off
    /// that came back false. There is no backend code to name, so the line is
    /// the whole message; the notice names which one, so the line can be
    /// re-worded after a language switch.
    void reportFailure(Notice notice);

    /// A block keeps its own line for progress and empty states; the failure
    /// surface takes over while there is a failure, and hands it back after.
    void showBlockFailure(FailureView* view, QLabel* state, const backend::Failure& failure,
                          const QString& context);
    void clearBlockFailure(FailureView* view, QLabel* state);
    /// Rebuild the format tabs from the result set in hand, with the active
    /// filter's tab selected and its signals blocked throughout. The bar stays
    /// visible while a filter is active even when the filtered response carries
    /// a single format, so the All escape never disappears.
    void rebuildFormatBar();
    /// Refresh the per-format counts from the whole result set. Each count is a
    /// one-row request, so this is only worth running for the formats the
    /// current page actually shows.
    void refreshFormatTotals();
    void applyFormatFilter();
    void populateRows();
    void updatePager();
    void goToPage(int page);
    void openByRef(const QString& ref);
    /// The public mikai.me page for a ref, including the slug from the detail
    /// already cached for it. Without the slug the id alone is a dead link.
    QString pageUrlFor(const QString& ref) const;
    void openItem(QListWidgetItem* item);
    void resultsContextMenu(const QPoint& position);
    void animeCardMenu(const QString& ref, const QPoint& globalPos);
    void requestThumb(const QString& url);
    void applyThumb(const QString& url, const QByteArray& data);
    /// Decode and paint everything applyThumb queued, once per batch.
    void flushThumbs();
    void applyThumbs();
    void applyFeedPosters();

    Runner& runner_;
    backend::MikaiClient& client_;

    QList<backend::AnimeSummary> entries_;
    int requestSeq_ = 0;
    bool browseMode_ = true;
    QString currentQuery_;
    int page_ = 1;
    int pages_ = 1;
    int total_ = 0;
    int pageSize_ = 0;
    /// The criteria the catalogue request carries. Every catalogue search names
    /// them itself - the sort, the order, the filters - so the server applies
    /// the criterion the viewer chose. The home carousels keep their own
    /// browse calls, whose fixed criteria the catalogue never inherits.
    backend::AnimeQuery catalogueQuery_;
    backend::ApiMeta apiMeta_;
    QString dictionaryError_;
    QVector<backend::GenreTag> genres_;
    QVector<backend::GenreTag> tags_;
    QStringList studios_;
    backend::Team activeTeam_;
    backend::TeamProfile activeTeamProfile_;
    int teamPage_ = 1;
    int teamPages_ = 1;
    int teamRequestSeq_ = 0;
    int dictionarySeq_ = 0;
    int catalogueRequestSeq_ = 0;
    backend::UpdateFeedState updateFeedState_;
    bool teamBrowseMode_ = false;
    /// The format tab the last request was made with, as the API's own value.
    /// Empty means no filtering; the server applies it, so a result set's total
    /// and page count are the filtered ones.
    QString activeFormat_;
    /// Per-format totals for the result set in hand, keyed by the API's format
    /// value. The bar labels these; a missing key means "not counted yet".
    QHash<QString, int> formatTotals_;
    /// Guards the count requests so a tab change cannot re-label the bar with
    /// counts from the result set it just replaced.
    int formatTotalsSeq_ = 0;
    QString formatTotalsQuery_;

    QHash<QString, QByteArray> thumbCache_;
    QSet<QString> thumbsInFlight_;
    /// Thumb bytes waiting for the coalescing timer, keyed by url.
    QHash<QString, QByteArray> pendingThumbs_;
    QHash<QString, QPixmap> icons_;
    /// Rounded card pixmaps per poster URL. Several cards share one feed
    /// poster and one box, so the scale-crop-clip runs once per URL.
    QHash<QString, QPixmap> rounded_;
    PageGlow* pageGlow_ = nullptr;

    QHash<QString, QList<TitleCard*>> carouselWidgets_;
    /// Every carousel card keyed by its image url, so a landing thumb is a
    /// lookup rather than a scan over every card on the page.
    QHash<QString, QList<TitleCard*>> cardsByImageKey_;
    /// Cards by title ref, so a detail landing is a lookup rather than a scan
    /// of every carousel on the page.
    QHash<QString, QList<TitleCard*>> cardsByRef_;
    QHash<QString, QList<backend::AnimeSummary>> carouselEntries_;
    /// Thumb URL paired with the label waiting for it, per feed block.
    QHash<QString, QVector<FeedPosterSlot>> feedPosterLabels_;
    QSet<QString> detailLookups_;
    QHash<QString, backend::AnimeDetail> cardDetails_;
    QHash<QString, backend::PosterSet> healedPosters_;
    QHash<QString, int> feedSeq_;
    QHash<QString, CarouselSection*> carousels_;
    QHash<QString, FeedSection*> feeds_;
    QList<backend::AnimeSummary> recentItems_;
    QSet<QString> enabledFeeds_;

    /// The status line as the state it stands for, so a language switch can
    /// restate the sentence without re-running the request behind it. A failed
    /// line keeps the failure itself: only the app's own wording for a code is
    /// re-localized, a line the backend produced stays as it arrived.
    Status statusLine_ = Status::BrowseHint;
    backend::Failure statusFailure_;
    Notice statusNotice_ = Notice::CopyLink;

    QTimer* healTimer_ = nullptr;
    QTimer* thumbTimer_ = nullptr;
    /// Handles for the captions a language switch restates. The query holds the
    /// viewer's text and the results list its rows; neither is rebuilt.
    QPushButton* homeButton_ = nullptr;
    QPushButton* mediaButton_ = nullptr;
    QAction* openFileAction_ = nullptr;
    QAction* openUrlAction_ = nullptr;
    QPushButton* keysButton_ = nullptr;
    QPushButton* settingsButton_ = nullptr;
    QPushButton* searchButton_ = nullptr;
    QLineEdit* query_ = nullptr;
    QPushButton* filtersButton_ = nullptr;
    QPushButton* teamsButton_ = nullptr;
    QPushButton* teamBackButton_ = nullptr;
    QLabel* teamDetailLabel_ = nullptr;
    QLabel* resultsHeading_ = nullptr;
    QLabel* status_ = nullptr;
    QScrollArea* discoveryScroll_ = nullptr;
    QTabBar* formatBar_ = nullptr;
    QListWidget* results_ = nullptr;
    RangePager* pager_ = nullptr;
    RateLimitLabel* rateLabel_ = nullptr;
    mplayer::shell::KeyHints* keyHints_ = nullptr;
};

}  // namespace mikai::app
Q_DECLARE_METATYPE(mikai::backend::UpdateFeedState)
