#pragma once

#include <QList>
#include <QPair>
#include <QPointer>
#include <QString>
#include <QWidget>
#include <optional>
#include <player/ProcessingSettingsIntent.hpp>

class QStackedWidget;

namespace mikai::backend {
class MikaiClient;
}

namespace mplayer::shell {
class PlayerDialog;
}

namespace mikai::app {

class SettingsDialog;
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
    /// The standalone player's version of the title page's runtime request: it
    /// provisions the mpv runtime when the core could not be created, then
    /// starts the player again.
    void requestMediaRuntime();
    /// False only when the previous page's own shutdown was refused, in which
    /// case the page is kept and the navigation does not happen.
    bool openTitle(const QString& ref, const QString& poster, bool preserveHistory = false);
    void openRelated(const QString& ref, const QString& poster);
    void goSearch();
    /// Disposes the title page and every player it owns. False when a player
    /// refused its close: the page and its sessions are kept for a retry.
    bool disposeTitle();

    backend::MikaiClient& client_;
    Runner* runner_ = nullptr;
    NativeComponentProvisioner* componentProvisioner_ = nullptr;
    QualityCache* qualityCache_ = nullptr;
    QStackedWidget* stack_ = nullptr;
    SearchPage* search_ = nullptr;
    TitlePage* title_ = nullptr;
    QPointer<mplayer::shell::PlayerDialog> mediaPlayer_;
    /// One runtime request at a time for the standalone player. `runtimeSeq_`
    /// is bumped whenever that dialog goes away, so a completion belonging to a
    /// superseded dialog is dropped instead of starting the new one.
    int runtimeSeq_ = 0;
    bool runtimePending_ = false;
    bool runtimeRetrying_ = false;
    bool setupDeclined_ = false;
    bool setupPromptOffered_ = false;
    QString titleRef_;
    QString titlePoster_;
    QList<QPair<QString, QString>> titleHistory_;
};

}  // namespace mikai::app
