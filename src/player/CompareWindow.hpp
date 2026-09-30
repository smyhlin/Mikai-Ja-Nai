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
#include <QString>
#include <QStringList>

#include "CompareLayout.hpp"
#include "PlayerPopups.hpp"

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QStackedWidget;
class QTimer;
class QToolButton;

namespace mplayer {

class VideoSurface;

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
    QString sideA() const;
    QString sideB() const;
    QString labelFor(const QString& id) const;

    mplayer::VideoSurface* surface_ = nullptr;
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
    /// What the badge menus offer: the same candidates, each with the menu path
    /// that puts the presets and the loose shader families in their own
    /// sections. Built once with `candidates_`.
    QVector<ChoiceEntry> choices_;
    /// The ids the viewer has picked, A first. Empty when nothing is on disk.
    QStringList selection_;
    QString currentCandidateId_;
    CompareMode mode_ = CompareMode::SideBySide;
    bool flickerShowA_ = true;
};

}  // namespace shell
}  // namespace mplayer
