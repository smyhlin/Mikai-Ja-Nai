#pragma once

#include <backend/Models.hpp>

#include <QHash>
#include <QList>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

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

    /// Put a title at the head of the "recently opened" carousel and persist it.
    void recordRecentTitle(const QString& ref, const QString& title, const QString& poster);

    /// The detail already fetched for `ref`, if any. A card's detail lookup
    /// lands for every title on the page, so opening one usually has the answer
    /// in hand and the title page can skip asking for it again.
    const backend::AnimeDetail* cachedDetail(const QString& ref) const {
        const auto it = cardDetails_.constFind(ref);
        return it == cardDetails_.constEnd() ? nullptr : &it.value();
    }

Q_SIGNALS:
    void openRequested(const QString& ref, const QString& poster);
    void settingsRequested();
    void mediaFileRequested();
    void mediaUrlRequested();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    /// One carousel block: heading, status line, retry button and the track.
    struct CarouselSection {
        QWidget* section = nullptr;
        QHBoxLayout* cards = nullptr;
        QScrollArea* scroll = nullptr;
        QLabel* state = nullptr;
        QPushButton* retry = nullptr;
        QPushButton* previous = nullptr;
        QPushButton* next = nullptr;
    };

    /// One list feed block (updates, schedule): heading, state line and rows.
    struct FeedSection {
        QWidget* section = nullptr;
        QLabel* state = nullptr;
        QPushButton* retry = nullptr;
        QWidget* rowsHost = nullptr;
        QVBoxLayout* rows = nullptr;
    };

    /// A feed poster waiting on one thumb URL, remembering which URL its label
    /// already shows so a repeated pass over the icon table is a no-op.
    struct FeedPosterSlot {
        QString url;
        QPointer<QLabel> label;
        QString shown;
    };

    void smoothScrollCarousel(QScrollBar* bar, int delta);
    CarouselSection* buildCarousel(const QString& heading, const QString& key);
    FeedSection* buildFeedSection(const QString& heading, const QString& key);
    CarouselSection* carouselSection(const QString& key) const;
    FeedSection* feedSection(const QString& key) const;

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
                      const QList<QPair<backend::AnimeSummary, QString>>& entries,
                      const QString& emptyText);

    void fetchPage(int page);
    void runResultsRequest(int page);
    void search();
    void showDiscovery();
    void showSearchResults();
    void onResults(const backend::SearchResult& result, int requestedPage);
    void onError(const QString& message);
    void rebuildFormatBar();
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

    QHash<QString, QByteArray> thumbCache_;
    QSet<QString> thumbsInFlight_;
    /// Thumb bytes waiting for the coalescing timer, keyed by url.
    QHash<QString, QByteArray> pendingThumbs_;
    QHash<QString, QPixmap> icons_;
    /// Rounded card pixmaps per poster URL. Several cards share one feed
    /// poster and one box, so the scale-crop-clip runs once per URL.
    QHash<QString, QPixmap> rounded_;
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

    QTimer* healTimer_ = nullptr;
    QTimer* thumbTimer_ = nullptr;
    QLineEdit* query_ = nullptr;
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
