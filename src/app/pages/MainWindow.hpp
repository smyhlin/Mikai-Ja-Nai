#pragma once

#include <QList>
#include <QPair>
#include <QPointer>
#include <QString>
#include <QWidget>
#include <optional>
#include <player/ProcessingSettingsIntent.hpp>

#include <native/bundle/NativeRuntimeSelection.hpp>

class QLabel;
class QProgressBar;
class QStackedWidget;

namespace mikai::backend {
class MikaiClient;
}

namespace mplayer::shell {
class PlayerDialog;
}

namespace mikai::app {

class SettingsDialog;
class ComponentStoreTab;
class NativeComponentProvisioner;
class QualityCache;
class Runner;
class SearchPage;
class TitlePage;

/// The application shell: a `QStackedWidget` holding one `SearchPage` and at
/// most one `TitlePage`.
///
/// Opening a title disposes the previous one, which saves the quality cache and
/// closes every player it opened. A related title pushes onto a small history
/// so "back" walks the chain instead of dropping to the search page.
class MainWindow : public QWidget {
    Q_OBJECT

public:
    explicit MainWindow(backend::MikaiClient& client, QWidget* parent = nullptr);
    ~MainWindow() override;

    /// The standalone video player the page currently owns, or nullptr. The
    /// media probes address the stream they just opened through this rather
    /// than guessing it from child order.
    mplayer::shell::PlayerDialog* currentMediaPlayer() const;

    /// The first-run TensorRT component check. A no-op unless the Native runtime
    /// is selected and the bundle does not carry the builder resource this GPU
    /// needs; a machine whose bundle already fits downloads nothing. The named
    /// refusal is logged when nothing is needed.
    void ensureNativeComponents();

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void openSettings();
    /// Opens Settings on the Components tab, landing on the element the startup
    /// prompt warned about. Plain dialog, no intent: nothing is armed.
    void openComponentSettings();
    /// The processing-failure remedy surface. `intent` is what the viewer
    /// actually asked for: an explicit Prepare runs the existing preparation
    /// command once the live context callbacks are installed, Providers only
    /// focuses the existing choice. Opening settings plain starts nothing.
    void openSettingsForIntent(mplayer::shell::ProcessingSettingsIntent intent,
                               QPointer<mplayer::shell::PlayerDialog> core);
    /// Offers the AnimeJaNai component setup once while it is recommended.
    void offerAnimeJanaiSetup(SettingsDialog* settings = nullptr);
    void showAnimeJanaiSetupPrompt(SettingsDialog* settings);
    /// Pauses the player that is actually playing, once, for a component prompt.
    /// `releaseAnimeJanai` turns AnimeJaNai off when the prompt is finished so
    /// playback can continue on a shader while the packs are still downloading.
    /// A prompt that finds nothing playing and does not release AnimeJaNai
    /// records nothing.
    void pausePlaybackForComponentAsk(bool releaseAnimeJanai);
    /// Undoes `pausePlaybackForComponentAsk` on the player it paused. A player
    /// that is already playing is left playing. Safe to call when no prompt held
    /// anything.
    void resumePlaybackAfterComponentAsk();
    /// Shows the download on the main window. The first tick of a run closes
    /// Settings when it is open, so the catalogue can take clicks while the
    /// packs keep downloading.
    void noteComponentAcquisition(const QString& status, int percent, bool started);
    void noteComponentAcquisitionIdle();
    /// Hides Settings without cancelling the download it started. Queued so a
    /// download that begins before `exec()` still closes the dialog on the
    /// first turn of that loop, instead of being shown again by it.
    void releaseSettingsForAcquisition();
    /// The one settings surface. `intent` is present only when the viewer
    /// pressed a processing-failure remedy, and `preferredCore` then names the
    /// dialog that raised it; it is consulted only while it still owns a
    /// stream, so preparation facts are never read off a dead core.
    /// `showComponentsFirst` lands on the Components tab for the startup
    /// prompt; it changes nothing else about the dialog.
    void openSettingsImpl(std::optional<mplayer::shell::ProcessingSettingsIntent> intent,
                          QPointer<mplayer::shell::PlayerDialog> preferredCore,
                          bool showComponentsFirst = false,
                          bool installAnimeJanaiRecommended = false);
    void chooseMediaFile();
    void chooseMediaUrl();
    /// Opens `source` in a fresh player. Refuses, keeping the current player,
    /// when that player has not acknowledged its own native stop yet.
    bool openMedia(const QString& source);
    /// Closes the media player, honouring its own close barrier. False when the
    /// player refused because native work is still stopping: nothing is deleted
    /// and `mediaPlayer_` is kept so the close can be retried. `forced` is the
    /// destructor path, where destruction cannot be refused; it names the forced
    /// stop and still takes the player through its own close first, so the core
    /// dies before the widget releases what it held.
    bool closeMedia(bool forced = false);
    void requestMediaRuntime();
    /// False only when the previous page's own shutdown was refused, in which
    /// case the page is kept and the navigation does not happen.
    bool openTitle(const QString& ref, const QString& poster, bool preserveHistory = false);
    void openRelated(const QString& ref, const QString& poster);
    void goSearch();
    /// Disposes the title page and every player it owns. False when a player
    /// refused its close: the page and its sessions are kept for a retry.
    bool disposeTitle();
    /// The runtime wait this window's standalone player is on, answered on the
    /// GUI thread once the app's resolution has published its terminal state.
    /// Registered against this window, so a window that goes away takes its
    /// waiters with it instead of touching a dialog that is gone.
    void answerRuntimeWhenSettled();
    /// Runs the resolution the standalone player is waiting on and reports its
    /// terminal state: plays, or names the reason playback is not possible.
    void runMediaRuntimeResolution();
    /// Releases the resolution this window holds when its last waiter is gone or
    /// the wait was superseded. A stale release never clears a newer owner's
    /// resolution.
    void releaseRuntimeResolution();
    /// The GUI-thread answer to a wait this window joined: play the source that
    /// is still open, or name the terminal reason on the dialog that is still
    /// current. Runs once, after the resolution has published its outcome.
    void answerMediaRuntimeResolved();

    backend::MikaiClient& client_;
    Runner* runner_ = nullptr;
    NativeComponentProvisioner* componentProvisioner_ = nullptr;
    /// Outlives Settings. Closing the dialog gives this tab back instead of
    /// destroying it, so a download the viewer started keeps running.
    ComponentStoreTab* componentsTab_ = nullptr;
    /// The settings dialog `openSettingsImpl` is inside, when one is open.
    /// A second open raises this one instead of nesting another modal loop.
    QPointer<SettingsDialog> settingsDialog_;
    QWidget* acquisitionBar_ = nullptr;
    QLabel* acquisitionStatus_ = nullptr;
    QProgressBar* acquisitionProgress_ = nullptr;
    QualityCache* qualityCache_ = nullptr;
    QStackedWidget* stack_ = nullptr;
    SearchPage* search_ = nullptr;
    TitlePage* title_ = nullptr;
    QPointer<mplayer::shell::PlayerDialog> mediaPlayer_;
    /// One runtime request at a time for the standalone player. `runtimeSeq_`
    /// is bumped whenever that dialog goes away, so a completion belonging to a
    /// superseded dialog is dropped instead of starting the new one.
    int runtimeSeq_ = 0;
    /// True from the moment the standalone player asks for a runtime until the
    /// wait has been answered. A request that arrives while the app's own
    /// startup resolution is still running joins that resolution instead of
    /// starting a second one, so the dialog shows the pending line and plays as
    /// soon as the module is pinned rather than reporting it as missing.
    bool runtimePending_ = false;
    /// The resolution this window owns, valid only while the standalone player
    /// is waiting on it. A stale owner can never release the resolution that
    /// replaced it, so a discarded player cannot unlock a newer wait.
    mplayer::native::ResolutionLease runtimeResolution_;
    /// Whether this window is holding the resolution lease, which is what keeps
    /// the lease from being released twice.
    bool runtimeResolutionHeld_ = false;
    bool setupDeclined_ = false;
    bool setupPromptOffered_ = false;
    /// The player a component prompt paused, and whether that prompt must turn
    /// AnimeJaNai off when it finishes. Cleared by the matching resume.
    QPointer<mplayer::shell::PlayerDialog> componentAskPlayer_;
    bool componentAskHeld_ = false;
    bool componentAskPausedPlayback_ = false;
    bool componentAskReleaseAnimeJanai_ = false;
    QString titleRef_;
    QString titlePoster_;
    QList<QPair<QString, QString>> titleHistory_;
};

}  // namespace mikai::app
