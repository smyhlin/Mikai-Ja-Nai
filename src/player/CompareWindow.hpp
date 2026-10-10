#pragma once

#include <QDialog>
#include <QHash>
#include <QImage>
#include <QPointer>
#include <QString>
#include <QStringList>

#include <memory>
#include <optional>

#include "CompareLayout.hpp"
#include "PlayerPopups.hpp"
#include "VideoSurface.hpp"

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QSpinBox;
class QStackedWidget;
class QTimer;

namespace mplayer::shell {

struct AjiPreparationTask;
class CompareRenderer;
class CompareCanvas;
class CompareGrid;

// Each candidate renders in an isolated core from one immutable source PNG.
class CompareWindow : public QDialog {
    Q_OBJECT

public:
    CompareWindow(mplayer::VideoSurface* surface, const QString& currentCandidateId,
                  QWidget* parent = nullptr,
                  native::Backend ajiBackend = native::Backend::TensorRT);
    ~CompareWindow() override;

    QImage frozenFrame() const { return isolatedOriginal_; }
    std::optional<double> frozenPlaybackTimestamp() const { return frozenPlaybackTimestamp_; }
    uint64_t frozenSourceEpoch() const { return frozenSourceEpoch_; }
    QImage renderedSideA() const { return renderedA_; }
    QImage renderedSideB() const { return renderedB_; }
    bool comparisonPending() const {
        return rendering_ || captureFrameActive_ || pendingAjiPreparation_ ||
               !pendingAjiId_.isEmpty() || playbackReleasePending_ || playbackNeedsRecapture_;
    }
    /// True only while the frame capture is running. The show path captures
    /// synchronously and finishes with either a frozen frame or a named
    /// refusal, so a caller reads this to tell those two apart instead of
    /// guessing from a delay.
    bool captureInProgress() const { return captureFrameActive_; }
    /// Whether the window still owns the playback hold it took for its frozen
    /// frame. A capture can succeed and still lose the hold, which is a
    /// different fault from a refusal and must be named separately.
    bool playbackHeld() const { return playbackHoldActive_; }
    QString candidateError(const QString& id) const { return ajiErrors_.value(id); }
    QString captureError() const { return captureError_; }
    QString lockError() const { return lockError_; }
    int queuedNativeChanges() const {
        return surface_ != nullptr ? surface_->queuedNativeTransitions() : 0;
    }
    void captureNow();
    void setAjiBackend(native::Backend backend);

private slots:
    void onModeChanged(int index);
    void onSplitChanged(double value);
    void onFlickerChanged(int value);
    void onGainChanged(int value);
    void onFlickerTimeout();
    void onCandidateChosen(int side, const QString& id);
    void onHoverPixel(int x, int y, bool valid);

protected:
    void showEvent(QShowEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void buildUi();
    void populateCandidates();
    void acquirePlaybackHold();
    bool playbackHoldOwned() const;
    void requestPlaybackHoldRelease();
    void finishDeferredPlaybackHoldRelease();
    void onPlaybackStateChanged(bool playing);
    void maintainPlaybackHold();
    void tryRecaptureAfterPause();
    void invalidateFrozenPlayback(const QString& reason);
    void clearGridPanes();
    void flushDeferredGridClear();
    void captureFrame();
    QImage grabSettledFrame();
    std::optional<double> readPlaybackTimestamp();
    void render();
    void renderOnce();
    QImage renderIsolated(const QString& id);
    void dismissCandidateMenus();
    void updateInfo();
    void updateFlickerTimer();
    bool ensurePlanLock();
    void releasePlanLock();
    void onNativeStatusSettled();
    QString sideA() const;
    QString sideB() const;
    QString labelFor(const QString& id) const;

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
    ProcessingOverlay* processingOverlay_ = nullptr;
    bool buildOperationActive_ = false;

    // Exact live pixels are retained only to guard main playback preservation;
    // canonical comparison input comes from the isolated unprocessed core.
    QImage frame_;
    std::shared_ptr<CompareRenderer> isolatedRenderer_;
    QImage isolatedOriginal_;
    uint64_t frozenSourceEpoch_ = 0;
    std::optional<double> frozenPlaybackTimestamp_;
    QImage renderedA_;
    QImage renderedB_;
    QString renderedAId_;
    QString renderedBId_;
    native::Backend renderedBackend_ = native::Backend::TensorRT;
    QString captureError_;
    quint64 positionEpoch_ = 0;
    QStringList candidates_;
    QVector<ChoiceEntry> choices_;
    QHash<QString, native::Backend> ajiBackends_;
    QHash<QString, QString> ajiErrors_;
    QStringList selection_;
    QString currentCandidateId_;
    native::Backend ajiBackend_ = native::Backend::TensorRT;
    CompareMode mode_ = CompareMode::SideBySide;
    bool flickerShowA_ = true;

    std::optional<mplayer::VideoSurface::NativePlanLock> nativeLock_;
    QString lockError_;
    bool lockRetryArmed_ = false;
    bool rendering_ = false;
    bool renderQueued_ = false;
    bool playbackHoldActive_ = false;
    bool playbackHoldOriginalPlaying_ = false;
    bool playbackHoldMutation_ = false;
    bool playbackReleasePending_ = false;
    bool deferredPlanLockRelease_ = false;
    bool playbackHoldFailed_ = false;
    bool playbackNeedsRecapture_ = false;
    bool captureFrameActive_ = false;
    bool playbackHoldRetryScheduled_ = false;
    int playbackHoldRetryCount_ = 0;
    uint64_t playbackHoldSourceEpoch_ = 0;
    quint64 playbackHoldGeneration_ = 0;
    quint64 selectionGeneration_ = 0;
    quint64 activeRenderGeneration_ = 0;
    QString pendingAjiId_;
    bool pendingAjiPreparation_ = false;
    std::shared_ptr<AjiPreparationTask> preparationTask_;
    int menuActivationDepth_ = 0;
    bool gridTeardownDeferred_ = false;
};

}  // namespace mplayer::shell
