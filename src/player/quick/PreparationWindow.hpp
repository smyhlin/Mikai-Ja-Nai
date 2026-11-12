// The player's separate engine-preparation status window.
//
// A real native QQuickView with its own scene: no video, no renderer and no
// playback state. It exists because a build is started from the settings
// dialog while the viewer keeps watching: a child of the player would sit
// behind that dialog and the work would be invisible. The window is a
// nonactivating tool window that stays above the player on Windows and X11,
// reasserted only while it is visible; on Wayland the compositor owns global
// stacking and only Qt's initial request applies.
//
// Content is the executable module's `Mikai.Player/PreparationView` root,
// bound to the controller's typed preparation fields. `loadFromModule` is the
// one load path; a failure is reported and leaves the window unshown.

#pragma once

#include <QElapsedTimer>
#include <QQuickView>
#include <QString>

#include <Qt>

QT_BEGIN_NAMESPACE
class QCloseEvent;
class QEvent;
class QHideEvent;
class QShowEvent;
class QTimer;
QT_END_NAMESPACE

namespace mplayer {
namespace quick {

class PlayerController;

class PreparationWindow : public QQuickView {
    Q_OBJECT
    /// The run lifecycle is the window's own: the controller's fields are the
    /// words, these three say which of the three presentations is showing.
    Q_PROPERTY(bool running READ isRunning NOTIFY stateChanged)
    Q_PROPERTY(bool succeeded READ isSucceeded NOTIFY stateChanged)
    Q_PROPERTY(bool failed READ isFailed NOTIFY stateChanged)
    /// Run clock, owned by the run: seconds since present(), never playback.
    Q_PROPERTY(int elapsedSeconds READ elapsedSeconds NOTIFY elapsedChanged)

public:
    explicit PreparationWindow(PlayerController* controller,
                               QWindow* transientParent = nullptr);
    ~PreparationWindow() override;

    PlayerController* controller() const { return controller_; }

    /// Starts a run: loads the scene once, shows the window without activation
    /// and begins reasserting the topmost band where the platform allows it.
    Q_INVOKABLE void present();
    /// Hides without a terminal presentation and clears the run state.
    Q_INVOKABLE void dismiss();
    /// Terminal presentation of the run the controller just settled. Success
    /// leaves on its own; a failure stays until the viewer dismisses it, because
    /// a refusal that disappears is a refusal nobody saw.
    void finish(bool ok);

    /// True while the platform's topmost-reassert timer is running. Stops with
    /// the window and with the object; a hidden window never keeps polling.
    bool stackingPollActive() const;

signals:
    void stateChanged();
    void elapsedChanged();
    /// The window hid itself: dismiss(), a success auto-hide or a window-manager
    /// close. The owner clears its visible run state here.
    void dismissed();

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    bool event(QEvent* event) override;

private:
    enum class Run { Idle, Running, Succeeded, Failed };

    bool isRunning() const { return run_ == Run::Running; }
    bool isSucceeded() const { return run_ == Run::Succeeded; }
    bool isFailed() const { return run_ == Run::Failed; }
    int elapsedSeconds() const { return elapsedSeconds_; }

    void loadView();
    /// Places the window centred near the top of the player's screen.
    void positionNearParent();
    void maintainTopmost();
    /// Drops the topmost band while a Widget popup, tooltip or modal box the
    /// viewer must answer is up; it cannot be raised above a topmost window.
    void yieldForModal();
    void syncStackingTimer();
    void stopStackingTimer();
    void setRun(Run run);

    PlayerController* controller_ = nullptr;
    Run run_ = Run::Idle;
    QTimer* finishTimer_ = nullptr;
    QTimer* elapsedTimer_ = nullptr;
    QTimer* stackingTimer_ = nullptr;
    QElapsedTimer elapsed_;
    int elapsedSeconds_ = 0;
    bool tearingDown_ = false;
#ifdef Q_OS_WIN
    bool stackingFailureReported_ = false;
#endif
    bool yieldedForModal_ = false;
};

}  // namespace quick
}  // namespace mplayer
