#include <utility>
#pragma once

#include <backend/Errors.hpp>
#include <backend/Models.hpp>

#include <native/bundle/NativeRuntimeSelection.hpp>

#include <QMap>
#include <QPair>
#include <QPoint>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVector>
#include <player/shell/ProcessingSettingsIntent.hpp>
#include <QWidget>

#include <functional>
#include <memory>

class QButtonGroup;
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
class FailureView;
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

    /// Save the quality cache and close every open player. False when a player
    /// refused its close because native work is still stopping: nothing is
    /// forgotten or deleted and the page stays usable for a retry. Safe to call
    /// twice. `forced` is the destructor path, where destruction cannot be
    /// refused; it still takes every player through its own close first, so each
    /// core dies before its widget releases what it held.
    bool shutdown(bool forced = false);

Q_SIGNALS:
    void backRequested();
    void settingsRequested();
    /// One of this page's open players raised a processing-failure remedy that needs the
    /// application's Settings surface. `player` is the dialog that raised it, and the
    /// owner answers only while that dialog still owns a stream: the page never keeps it
    /// alive and never reads anything off it itself. Nothing here re-extracts a URL or
    /// re-probes a quality.
    void playerProcessingSettingsRequested(mplayer::shell::PlayerDialog* player,
                                          mplayer::shell::ProcessingSettingsIntent intent);
    /// One episode player needs the AnimeJaNai component setup surface.
    void playerComponentSetupRequested();
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

    /// What the page's own status line currently stands for. The line is
    /// restated from this after a language switch instead of keeping translated
    /// text that would then be stale.
    enum class BaseStatus { Empty, Loading, NoReleases, NoSources, Raw };

    /// What the cover is showing, so a switch can re-word the fallback without
    /// touching a pixmap that has already landed.
    enum class CoverState { Loading, Missing, Ready };

    void buildUi();
    void load();
    /// Hand the page its title once both halves of a two-task load have landed.
    void joinLoad(const std::shared_ptr<LoadState>& state);
    void onLoaded(const backend::AnimeDetail& detail, const backend::PlayerResult& player);
    void onError(const backend::Failure& failure);

    void populateDetail(const backend::AnimeDetail& detail);
    void populateRelated(const backend::AnimeDetail& detail);
    void loadRecommendations(const backend::AnimeDetail& detail);
    /// `headingKey` is a semantic key ("related", "similar", "recommendation"),
    /// not a word: the heading is worded from it and re-worded from it after a
    /// language switch.
    QWidget* buildRelatedCarousel(
        const QString& headingKey,
        const QVector<std::pair<QString, backend::AnimeSummary>>& entries);
    /// `fromUser` is false when data landing expands the block itself. Only a
    /// toggle the reader pressed scrolls the overview: the recommendations
    /// carousel arriving is not a request to be scrolled away from whatever
    /// they are reading.
    void setRelatedExpanded(bool expanded, bool fromUser = true);
    void toggleDescription();
    QString descriptionPreview() const;
    void toggleMetadata(bool expanded);

    static QString scoresText(const backend::AnimeDetail& detail);
    QString releaseChipLabel(const backend::Release& release) const;
    QString releaseChipToolTip(const backend::Release& release) const;
    void rebuildReleaseChips();
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
    void probeReleaseQualities();
    QString firstEmbed(const backend::Release& release) const;
    void applyReleaseQuality(int index, const QMap<int, QString>& qualities, int seq);

    QVector<QVariantMap> episodeVariants(const QString& number) const;
    QVector<QVariantMap> tabPlaylist(int stateIndex, const QString& number, int* currentIndex);

    void openPlayer(EpisodeEntry* entry, const QMap<int, QString>& qualities);
    /// False when the session's player refused its close because native work is
    /// still stopping: the session stays registered so the removal can be
    /// retried, and nothing of it is deleted or forgotten.
    bool closeSession(PlayerSession* session);
    /// Disposes a session that is still registered. Idempotent: a session the
    /// player's own `playerClosed()` already disposed is left alone.
    void disposeSession(PlayerSession* session, bool forced);
    void requestRuntime(PlayerSession* session);
    /// Runs the resolution this session answered by starting, and reports its
    /// terminal state to the same session: play, or name the reason.
    void runSessionRuntimeResolution(PlayerSession* session);
    /// Subscribes the session to the resolution the app is already running. The
    /// subscription is registered against the page, so a page that goes away
    /// discards its waiters instead of touching players that are gone.
    void answerSessionWhenSettled(PlayerSession* session);
    /// Answers one session's runtime wait once the resolution has published its
    /// terminal state: the session's own source opens, or the named reason is
    /// shown. Runs at most once per wait.
    void answerSessionResolved(PlayerSession* session,
                               const mplayer::native::RuntimeSelection& selection,
                               const QString& reason = {});
    /// Releases the resolution a session holds, on its terminal answer, on its
    /// disposal and on a superseded completion. A stale release is a no-op, so it
    /// can never clear the resolution a newer owner is running.
    void releaseSessionResolution(PlayerSession* session);
    void recoverStream(PlayerSession* session);
    void switchPlayerVariant(PlayerSession* session, const QString& embed, const QString& label);
    void probeTrackQuality(PlayerSession* session, const QString& embed);
    void playEpisodeFrom(PlayerSession* session, const QVariantMap& item, int index);

    void loadCover(const QString& url, bool allowHint);
    void coverContextMenu(const QPoint& globalPos);
    void copyCoverUrl();
    void saveCover();
    void openCoverViewer();

    /// Raw, untranslated status line: the failure line the backend produced.
    void setStatus(const QString& text);
    /// The status line for a state the app words itself, kept as a state so a
    /// language switch restates it.
    void setStatusKind(BaseStatus kind);
    QString baseStatusText(BaseStatus kind) const;
    QString relatedHeadingText(const QString& key) const;
    /// Re-words the cover's loading/missing fallback; a landed pixmap is left.
    void applyCoverFallback();
    void flashStatus(const QString& text, bool failure = false);

    /// Restates every static caption in place after a LanguageChange. The loaded
    /// title, the release/episode selection, the expansion toggles, the scroll
    /// position, the open players and every task in flight are left exactly as
    /// they were: nothing is reloaded, refetched or rebuilt from data.
    void retranslateUi();
    /// Facts, scores, details rows and links, rebuilt from the retained detail.
    void refreshDetailTexts();
    /// Carousel headings, pager tooltips, relation captions and poster
    /// fallbacks, keeping every painted poster and the carousels themselves.
    void refreshRelatedTexts();
    /// Release kind labels and chips, keeping the selection and any quality
    /// suffix a probe already appended.
    void refreshReleaseTexts();
    /// Provider tab names and every episode pill's label and tooltip.
    void refreshEpisodeTexts();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void changeEvent(QEvent* event) override;

    Runner& runner_;
    backend::MikaiClient& client_;
    QualityCache& qualityCache_;
    QString ref_;
    QString posterHint_;
    bool closed_ = false;

    QVector<backend::Release> releases_;
    QVector<TabState> tabStates_;
    /// Owns every episode record ever built for this page; freed on shutdown.
    QVector<EpisodeEntry*> ownedEntries_;

    CoverLabel* cover_ = nullptr;
    QLabel* heading_ = nullptr;
    QLabel* subtitle_ = nullptr;
    /// The page's own failure surface: a title that could not be loaded says so
    /// where the title would have been, with the code and a retry, instead of
    /// leaving the cover on its loading text.
    FailureView* failure_ = nullptr;
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
    QWidget* releaseChipsHost_ = nullptr;
    QVBoxLayout* releaseRowsLayout_ = nullptr;
    QButtonGroup* releaseGroup_ = nullptr;
    QVector<QPushButton*> releaseChips_;
    /// The row's kind captions, keyed by the raw kind, so a switch re-words
    /// them without rebuilding the rows.
    QVector<QPair<QString, QPointer<QLabel>>> releaseKindLabels_;
    QPushButton* backButton_ = nullptr;
    QPushButton* settingsButton_ = nullptr;
    QLabel* episodesHeading_ = nullptr;
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
    CoverState coverState_ = CoverState::Loading;
    /// The already-fetched detail for ref_, if the caller had one. When it is
    /// set, load() skips the redundant getAnime call.
    backend::AnimeDetail detailHint_;
    bool hasDetailHint_ = false;
    /// The detail the page painted from, retained so a language switch can
    /// re-word its captions without asking the backend for it again.
    backend::AnimeDetail detail_;
    bool hasDetail_ = false;
    /// Whether the page is currently showing a failure, so a language switch
    /// re-words only that failure's context clause (the widget re-words the
    /// rest itself) and never re-shows or re-classifies it.
    bool hasFailure_ = false;
    CoverViewer* coverViewer_ = nullptr;
    /// The description as the backend sent it; the shown text is derived from
    /// it, including the missing-description fallback.
    QString rawDescription_;
    QString fullDescription_;
    bool descriptionExpanded_ = false;
    QString baseStatus_;
    BaseStatus statusKind_ = BaseStatus::Empty;

    /// Bumped by every runtime request, so a completion belonging to a
    /// superseded attempt is dropped instead of starting a player the user has
    /// already moved on from.
    int runtimeSeq_ = 0;
    int releaseProbeSeq_ = 0;
    int activeRelease_ = -1;
    QMap<int, int> releaseQuality_;
    QSet<int> releaseProbed_;

    QVector<PlayerSession*> sessions_;
    QMap<QString, QVector<EpisodeEntry*>> fetchWaiters_;
    QMap<QString, QVector<PlayerSession*>> trackWaiters_;
    /// The caller waiting on an in-flight extraction, kept per episode because
    /// the same embed can be awaited by a pill and by an open player at once.
    QMap<EpisodeEntry*,
         QVector<std::function<void(EpisodeEntry*, const QMap<int, QString>&)>>>
        entryCallbacks_;

    /// What each related carousel registered when it was built, so a language
    /// switch re-words the captions in place. Rebuilding the carousels instead
    /// would drop every poster already fetched and restart their downloads, so
    /// the headings, pager tooltips, relation captions and still-empty poster
    /// fallbacks are all that is kept here.
    QVector<QPair<QString, QPointer<QLabel>>> relatedHeadings_;
    QVector<QPair<QPointer<QPushButton>, QPointer<QPushButton>>> relatedPagerButtons_;
    QVector<QPair<QString, QPointer<QLabel>>> relatedKindLabels_;
    QVector<QPointer<QLabel>> relatedPosterLabels_;
};

}  // namespace mikai::app
