#pragma once

#include <backend/Models.hpp>

#include <QMap>
#include <QPoint>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVector>
#include <QWidget>

#include <functional>
#include <memory>

class QComboBox;
class QGridLayout;
class QLabel;
class QLineEdit;
class QMenu;
class QPixmap;
class QPushButton;
class QScrollArea;
class QTabWidget;
class QTimer;
class QVBoxLayout;

namespace mikai::backend {
class MikaiClient;
}

namespace mplayer {
namespace shell {
class PlayerDialog;
class PlayerHost;
}  // namespace shell
}  // namespace mplayer

namespace mikai::app {

class CoverLabel;
class CoverViewer;
class QualityCache;
class RangePager;
class RateLimitLabel;
class Runner;

/// One title: cover, facts, description, related carousels, and one tab per
/// release holding that release's episodes as a grid of number pills.
///
/// Quality extraction happens once per episode, only when that episode is
/// opened. The result is cached per embed URL and persisted, so replaying an
/// episode reuses the store instead of re-extracting. The player is driven
/// in-process through `mplayer::shell::PlayerDialog`.
class TitlePage : public QWidget {
    Q_OBJECT

public:
    /// `detailHint` is an already-fetched AnimeDetail for `ref` (e.g. from
    /// SearchPage's card detail cache). When its ref matches, the redundant
    /// getAnime call is skipped and the player payload is fetched alone.
    TitlePage(Runner& runner, backend::MikaiClient& client, QualityCache& qualityCache,
              const QString& ref, const QString& posterHint = {},
              const backend::AnimeDetail& detailHint = {}, QWidget* parent = nullptr);
    ~TitlePage() override;

    /// Save the quality cache and close every open player. Safe to call twice.
    void shutdown();

Q_SIGNALS:
    void backRequested();
    void settingsRequested();
    void relatedOpenRequested(const QString& ref, const QString& poster);
    void recentTitleLoaded(const QString& ref, const QString& title, const QString& poster);

private:
    /// One episode pill: the number, its embed URL and the button showing it.
    struct EpisodeEntry {
        QString number;
        QString embedUrl;
        QString label;
        QPushButton* button = nullptr;
        int stateIndex = -1;
    };

    /// One release tab: its episodes, the page of them on screen, and the pager.
    ///
    /// Entries are held by pointer, not by value: a pill's click handler and an
    /// open player's recovery path both keep a handle across re-layouts, and a
    /// QVector that reallocates would dangle them.
    struct TabState {
        QString provider;
        QVector<EpisodeEntry*> entries;
        int page = 0;
        int pageSize = 0;
        QWidget* grid = nullptr;
        QScrollArea* scroll = nullptr;
        QGridLayout* gridLayout = nullptr;
        RangePager* pager = nullptr;
        int columns = 0;
        int rows = 0;
        /// False until the tab's first page is built. A release has one tab per
        /// provider and only one is ever on screen, so the rest stay unbuilt
        /// until they are first shown.
        bool rendered = false;
    };

    /// One open player window and the episode it is currently on.
    struct PlayerSession;
    /// The `PlayerHost` bridge answering the dialog's intent signals.
    class Host;

    /// What a two-task title load has collected so far. Shared by both halves
    /// and read only on the GUI thread, so it needs no lock.
    struct LoadState {
        backend::AnimeDetail detail;
        backend::PlayerResult player;
        QString error;
        bool hasDetail = false;
        bool hasPlayer = false;
    };

    void buildUi();
    void load();
    /// Hand the page its title once both halves of a two-task load have landed.
    void joinLoad(const std::shared_ptr<LoadState>& state);
    void onLoaded(const backend::AnimeDetail& detail, const backend::PlayerResult& player);
    void onError(const QString& message);

    void populateDetail(const backend::AnimeDetail& detail);
    void populateRelated(const backend::AnimeDetail& detail);
    void loadRecommendations(const backend::AnimeDetail& detail);
    QWidget* buildRelatedCarousel(
        const QString& heading, const QVector<QPair<QString, backend::AnimeSummary>>& entries);
    /// `fromUser` is false when data landing expands the block itself. Only a
    /// toggle the reader pressed scrolls the overview: the recommendations
    /// carousel arriving is not a request to be scrolled away from whatever
    /// they are reading.
    void setRelatedExpanded(bool expanded, bool fromUser = true);
    void toggleDescription();
    QString descriptionPreview() const;
    void toggleMetadata(bool expanded);

    static QString scoresText(const backend::AnimeDetail& detail);
    QString releaseLabel(const backend::Release& release, bool withCount = true) const;
    /// Track-picker label: the team only, no kind and no episode count.
    QString releaseTrackLabel(const backend::Release& release) const;

    void fillEpisodes(int index);
    void fitEpisodeSection();
    /// Queue one fit pass for the end of the current event batch.
    void scheduleFit();
    void renderTabPage(int stateIndex);
    void goToEpisodePage(int stateIndex, int page);
    /// Creates the pill for `entry` and points `entry->button` at it.
    QPushButton* buildEpisodeButton(EpisodeEntry* entry, QWidget* grid);
    void reflowGrid(int stateIndex);

    void playEpisode(EpisodeEntry* entry);
    void ensureQualities(EpisodeEntry* entry,
                         const std::function<void(EpisodeEntry*, const QMap<int, QString>&)>&
                             callback);
    void finishFetch(const QString& embed, const QMap<int, QString>& qualities,
                     const QString& message);
    void markNoSource(EpisodeEntry* entry);

    void episodeContextMenu(EpisodeEntry* entry, const QPoint& globalPos);
    void copyEpisode(EpisodeEntry* entry);
    /// Copies a direct stream link of a known height and says so in the status
    /// line, so a quality row behaves like the rest of the page.
    void copyStreamUrl(const QString& url, int height);
    void openExternalPlayer(EpisodeEntry* entry);
    void openExternalStream(EpisodeEntry* entry, const QMap<int, QString>& qualities);
    void openEmbedInBrowser(EpisodeEntry* entry);
    void copyAllUrls();
    void probeReleaseQualities();
    QString firstEmbed(const backend::Release& release) const;
    void applyReleaseQuality(int index, const QMap<int, QString>& qualities, int seq);

    QVector<QVariantMap> episodeVariants(const QString& number) const;
    QVector<QVariantMap> tabPlaylist(int stateIndex, const QString& number, int* currentIndex);

    void openPlayer(EpisodeEntry* entry, const QMap<int, QString>& qualities);
    void closeSession(PlayerSession* session);
    void requestRuntime(PlayerSession* session);
    void recoverStream(PlayerSession* session);
    void switchPlayerVariant(PlayerSession* session, const QString& embed, const QString& label);
    void probeTrackQuality(PlayerSession* session, const QString& embed);
    void playEpisodeFrom(PlayerSession* session, const QVariantMap& item, int index);

    void loadCover(const QString& url, bool allowHint);
    void coverContextMenu(const QPoint& globalPos);
    void copyCoverUrl();
    void saveCover();
    void openCoverViewer();

    void setStatus(const QString& text);
    void flashStatus(const QString& text);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

    Runner& runner_;
    backend::MikaiClient& client_;
    QualityCache& qualityCache_;
    QString ref_;
    QString posterHint_;
    bool closed_ = false;

    QVector<backend::Release> releases_;
    QVector<TabState> tabStates_;
    QVector<QVector<QString>> tabEmbeds_;
    /// Owns every episode record ever built for this page; freed on shutdown.
    QVector<EpisodeEntry*> ownedEntries_;

    CoverLabel* cover_ = nullptr;
    QLabel* heading_ = nullptr;
    QLabel* subtitle_ = nullptr;
    QLabel* facts_ = nullptr;
    QLabel* scores_ = nullptr;
    QLabel* description_ = nullptr;
    QPushButton* descriptionButton_ = nullptr;
    QPushButton* metaButton_ = nullptr;
    QPushButton* relatedButton_ = nullptr;
    QWidget* metaHost_ = nullptr;
    QGridLayout* metaGrid_ = nullptr;
    QLabel* links_ = nullptr;
    QWidget* relatedHost_ = nullptr;
    QVBoxLayout* relatedLayout_ = nullptr;
    QTabWidget* tabs_ = nullptr;
    QComboBox* selector_ = nullptr;
    QPushButton* copyAll_ = nullptr;
    QLabel* status_ = nullptr;
    QScrollArea* overviewScroll_ = nullptr;
    RateLimitLabel* rateLabel_ = nullptr;
    /// One coalescing timer behind fitEpisodeSection: a title load queues a fit
    /// per tab, and each fit resizes each grid, which queues another fit.
    QTimer* fitTimer_ = nullptr;
    /// The height the last fit settled on, so a repeat of the same layout does
    /// not re-measure and re-set it.
    int fittedHeight_ = 0;

    QPixmap coverPixmap_;
    QString coverUrl_;
    /// The already-fetched detail for ref_, if the caller had one. When it is
    /// set, load() skips the redundant getAnime call.
    backend::AnimeDetail detailHint_;
    bool hasDetailHint_ = false;
    CoverViewer* coverViewer_ = nullptr;
    QString fullDescription_;
    bool descriptionExpanded_ = false;
    QString baseStatus_;

    /// Bumped by every runtime request, so a completion belonging to a
    /// superseded attempt is dropped instead of starting a player the user has
    /// already moved on from.
    int runtimeSeq_ = 0;
    int releaseProbeSeq_ = 0;
    QMap<int, int> releaseQuality_;
    QSet<int> releaseProbed_;

    QVector<PlayerSession*> sessions_;
    QMap<QString, QVector<EpisodeEntry*>> fetchWaiters_;
    QMap<QString, QVector<PlayerSession*>> trackWaiters_;
    /// The caller waiting on an in-flight extraction, kept per episode because
    /// the same embed can be awaited by a pill and by an open player at once.
    QMap<EpisodeEntry*,
         std::function<void(EpisodeEntry*, const QMap<int, QString>&)>>
        entryCallbacks_;
};

}  // namespace mikai::app
