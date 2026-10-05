// Two shader chains, side by side, on one frozen frame.
//
// Adapted from AnimeInterp's CompareWindow. The surface is a CompareCanvas that
// takes its geometry from the pure buildLayout core, and the mode set is the
// reference's own rather than a subset: Original, Processed, SplitH, SplitV,
// SideBySide, Overlay, Flicker and Difference, each with the view controls that
// mode actually uses.
//
// It is a viewer of stills. Nothing here rewrites the chain the player is
// running; each side is rendered by applying its chain to one frozen frame
// through a borrowed surface, so a difference between the two is the chain and
// nothing else.
#pragma once

#include <QDialog>
#include <QImage>
#include <QPointer>
#include <QString>
#include <QStringList>

#include <optional>

#include "CompareLayout.hpp"
#include "PlayerPopups.hpp"
#include "VideoSurface.hpp"

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QStackedWidget;
class QTimer;
class QToolButton;

namespace mplayer {
namespace shell {

class CompareCanvas;
class CompareGrid;

class CompareWindow : public QDialog {
    Q_OBJECT

public:
    /// `surface` is borrowed, not owned: the window reads frames from it and
    /// never outlives the player that created it.
    CompareWindow(mplayer::VideoSurface* surface, const QString& currentCandidateId,
                  QWidget* parent = nullptr);
    ~CompareWindow() override;

    /// The frame both sides are currently rendered from. Public so a test can
    /// see WHICH picture the window froze, not just that something is on
    /// screen: a stale frame and a fresh one are equally non-null.
    QImage frozenFrame() const { return frame_; }
    /// Empty unless the last capture failed, in which case it says why and the
    /// window is showing the failure rather than an older picture.
    QString captureError() const { return captureError_; }
    /// Empty when the native plan is held (or when there is no source to hold),
    /// otherwise the named reason it could not be frozen - so a caller can tell
    /// "nothing to freeze" from "refused to freeze".
    QString lockError() const { return lockError_; }
    /// The surface's own native-transition queue depth, read live: the window
    /// owns no queue, and a request made while the lock is held changes no
    /// published status, so a cached copy would go stale.
    int queuedNativeChanges() const {
        return surface_ != nullptr ? surface_->queuedNativeTransitions() : 0;
    }
    /// Re-freezes the live surface at wherever the player is now. Every
    /// explicit extraction goes through here, so "capture again" can only ever
    /// mean "capture this position".
    void captureNow();

private slots:
    void onModeChanged(int index);
    void onSplitChanged(double value);
    void onFlickerChanged(int value);
    void onGainChanged(int value);
    void onFlickerTimeout();
    void onCandidateChosen(int side, const QString& id);
    void onHoverPixel(int x, int y, bool valid);

protected:
    /// The window is kept and reused by the player, so being shown again is
    /// the viewer's "compare where I am now" and has to take a new frame.
    void showEvent(QShowEvent* event) override;
    /// Releasing here, not only in the destructor, is what lets a native request
    /// made while Compare was open be applied the moment the window closes.
    void closeEvent(QCloseEvent* event) override;
    /// close() hides the window, so the lock is released here as well: a hidden
    /// Compare window never keeps the native plan frozen.
    void hideEvent(QHideEvent* event) override;

private:
    void buildUi();
    /// Fills the catalogue and opens on the running chain as A and the next
    /// preset as B. The viewer picks sides from the badges on the pictures.
    void populateCandidates();
    /// Freezes the frame the live surface is showing right now, at whatever
    /// position the player is parked on. It does not seek: a capture is a
    /// reading of the player, not a command to it.
    void captureFrame();
    /// Grabs the framebuffer once it has stopped changing, and only once the
    /// surface has stopped reporting a move. Null when no settled frame is
    /// available; never the previous picture.
    QImage grabSettledFrame(double* settledPosition);
    void render();
    /// Applies `id` to the borrowed core and returns the settled frame that
    /// comes back. Empty when there is no surface or no frame to work from.
    QImage renderWithChain(const QString& id);
    void updateInfo();
    void updateFlickerTimer();
    /// Takes the real native-plan lock the open window holds for its whole
    /// lifetime, so no admission, drain, suspend or resume can move the plan
    /// under the frozen frame. False, with `lockError_` set, when a transition is
    /// settling and the one retry also failed - the window then says so instead
    /// of freezing a plan that is about to change.
    bool ensurePlanLock();
    void releasePlanLock();
    /// The surface published a native status: refresh the on-demand queue depth
    /// and retry a lock that was refused while a transition was settling.
    void onNativeStatusSettled();
    QString sideA() const;
    QString sideB() const;
    QString labelFor(const QString& id) const;

    /// Borrowed, not owned: the player that created the window holds the
    /// surface, and it can be destroyed while this window is pumping the event
    /// loop mid-capture. A QPointer makes that a null check instead of a
    /// dangling read; every post-pump touch goes through the guard.
    QPointer<mplayer::VideoSurface> surface_;
    CompareCanvas* canvas_ = nullptr;
    CompareGrid* grid_ = nullptr;
    QStackedWidget* stack_ = nullptr;
    QComboBox* modeBox_ = nullptr;
    QLabel* splitLabel_ = nullptr;
    QLabel* flickerLabel_ = nullptr;
    QLabel* gainLabel_ = nullptr;
    QDoubleSpinBox* split_ = nullptr;
    QSpinBox* flicker_ = nullptr;
    QSpinBox* gain_ = nullptr;
    QLabel* info_ = nullptr;
    QLabel* hover_ = nullptr;
    QTimer* flickerTimer_ = nullptr;

    QImage frame_;
    QImage renderedA_;
    QImage renderedB_;
    /// Why the last capture produced no frame, shown in place of the picture.
    /// A capture that cannot read a fresh frame fails visibly rather than
    /// falling back on the previous one.
    QString captureError_;
    /// Counts the surface's position updates. The surface reports its position
    /// from a 50 ms pump, so a capture has to know whether a move is still
    /// being reported before it can trust the framebuffer: without this, a
    /// rewind followed immediately by a capture reads the position and the
    /// picture of the frame the viewer just left.
    quint64 positionEpoch_ = 0;
    QStringList candidates_;
    /// The player-menu presets the badge menus offer, with the same ids, labels
    /// and category paths. Built once with `candidates_`.
    QVector<ChoiceEntry> choices_;
    /// The ids the viewer has picked, A first. Empty when nothing is on disk.
    QStringList selection_;
    QString currentCandidateId_;
    CompareMode mode_ = CompareMode::SideBySide;
    bool flickerShowA_ = true;

    /// The real native-plan lock, held while the window is visible so no native
    /// transition can move the plan under the frozen frame.
    std::optional<mplayer::VideoSurface::NativePlanLock> nativeLock_;
    /// The named reason the lock could not be taken; empty when it was.
    QString lockError_;
    /// Set when a lock was refused because a transition was settling, so the
    /// next settled status retries it exactly once rather than spinning.
    bool lockRetryArmed_ = false;
};

}  // namespace shell
}  // namespace mplayer
