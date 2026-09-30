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

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QListWidget;
class QPushButton;
class QSlider;
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

private slots:
    void onSelectionChanged();
    void onModeChanged(int index);
    void onSplitChanged(double value);
    void onOpacityChanged(int value);
    void onFlickerChanged(int value);
    void onGainChanged(int value);
    void onFlickerTimeout();
    void onPlayPause();
    void onStepForward();
    void onStepBack();
    void onCandidateChosen(int side, const QString& id);
    void onHoverPixel(int x, int y, bool valid);

private:
    void buildUi();
    void populateList();
    void captureFrame();
    /// Seeks to `seconds`, waits for the seek to land, and keeps that frame.
    void captureAt(double seconds);
    void render();
    /// Applies `id` to the borrowed core and returns the settled frame that
    /// comes back. Empty when there is no surface or no frame to work from.
    QImage renderWithChain(const QString& id);
    void updateInfo();
    void updateTransport();
    void updateFlickerTimer();
    /// The picked ids in press order: first is A, second is B. A selection is a
    /// set, so the order the viewer chose is read explicitly.
    QStringList selection() const;
    QString sideA() const;
    QString sideB() const;
    QString labelFor(const QString& id) const;

    mplayer::VideoSurface* surface_ = nullptr;
    CompareCanvas* canvas_ = nullptr;
    CompareGrid* grid_ = nullptr;
    QStackedWidget* stack_ = nullptr;
    QListWidget* list_ = nullptr;
    QComboBox* modeBox_ = nullptr;
    QLabel* splitLabel_ = nullptr;
    QLabel* opacityLabel_ = nullptr;
    QLabel* flickerLabel_ = nullptr;
    QLabel* gainLabel_ = nullptr;
    QDoubleSpinBox* split_ = nullptr;
    QSlider* opacity_ = nullptr;
    QSpinBox* flicker_ = nullptr;
    QSpinBox* gain_ = nullptr;
    QLabel* info_ = nullptr;
    QLabel* transport_ = nullptr;
    QLabel* hover_ = nullptr;
    QToolButton* play_ = nullptr;
    QTimer* flickerTimer_ = nullptr;

    QImage frame_;
    QImage renderedA_;
    QImage renderedB_;
    QStringList candidates_;
    QString currentCandidateId_;
    /// Frozen position, in seconds, so both sides render the same frame.
    double position_ = 0.0;
    CompareMode mode_ = CompareMode::SideBySide;
    bool flickerShowA_ = true;
};

}  // namespace shell
}  // namespace mplayer
