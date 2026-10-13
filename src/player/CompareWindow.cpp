#include "CompareWindow.hpp"

#include "Anime4K.hpp"
#include "CompareCanvas.hpp"
#include "CompareGrid.hpp"
#include "CompareRenderer.hpp"
#include "ShellTheme.hpp"
#include "VideoSurface.hpp"
#include "native/components/AnimeJanaiRuntime.hpp"
#include "native/status/NativePlaybackController.hpp"

#include <QCloseEvent>
#include <QKeyEvent>
#include <QColor>
#include <QComboBox>
#include <QCoreApplication>
#include <QDoubleSpinBox>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QRunnable>
#include <QSignalBlocker>
#include <QShowEvent>
#include <QSpinBox>
#include <QStackedWidget>
#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <optional>

namespace mplayer {
namespace shell {

using mplayer::VideoSurface;

namespace {

constexpr int kGap = 12;
constexpr int kGapSmall = 8;
constexpr int kWindowWidth = 1280;
constexpr int kWindowHeight = 800;
constexpr double kSettleQuietSeconds = 0.08;
constexpr int kSettleRounds = 2;
// Observe several 50 ms surface updates before trusting a paused framebuffer.
constexpr int kMinObserveRounds = 5;
constexpr int kSettleTimeoutMs = 3000;
constexpr int kPollIntervalMs = 40;
constexpr int kMaxPauseReassertions = kSettleTimeoutMs / kPollIntervalMs;

QStringList categoryPath(const QString& categoryId) {
    QStringList path;
    QString parent;
    for (const QString& component : categoryId.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        if (!parent.isEmpty()) {
            parent += QLatin1Char('/');
        }
        parent += component;
        const QString label = anime4k::categoryLabel(parent);
        if (!label.isEmpty()) {
            path.append(label);
        }
    }
    return path;
}

}  // namespace

struct AjiPreparationTask {
    std::atomic_bool cancelled{false};
    std::atomic_bool finished{false};
    std::optional<native::AnimeJanaiPlan> plan;
};

struct PlaybackTimestampResult {
    std::atomic_bool finished{false};
    uint64_t sourceEpoch = 0;
    std::optional<double> timestamp;
};

CompareWindow::CompareWindow(VideoSurface* surface, const QString& currentCandidateId,
                             QWidget* parent, native::Backend ajiBackend)
    : QDialog(parent), surface_(surface), currentCandidateId_(currentCandidateId),
      ajiBackend_(ajiBackend) {
    setWindowTitle(tr("Quality Comparison"));
    setModal(false);
    resize(kWindowWidth, kWindowHeight);
    if (surface_ != nullptr) {
        const native::AnimeJanaiPlan activePlan = surface_->animeJanaiPlan();
        if (activePlan.enabled && !activePlan.presetId.isEmpty()) {
            const QVector<native::AnimeJanaiPreset> publicPresets = native::animeJanaiPresets();
            const auto active = std::find_if(
                publicPresets.cbegin(), publicPresets.cend(),
                [&activePlan](const native::AnimeJanaiPreset& preset) {
                    return !preset.rife && preset.id == activePlan.presetId;
                });
            if (active != publicPresets.cend()) {
                currentCandidateId_ = active->id;
            }
        }
    }
    buildUi();
    populateCandidates();
    // Capture after mapping; position updates prevent accepting a late seek's old frame.
    if (surface_ != nullptr) {
        connect(surface_, &VideoSurface::playingChanged, this,
                &CompareWindow::onPlaybackStateChanged);
        connect(surface_, &VideoSurface::sourceEpochChanged, this, [this](uint64_t epoch) {
            QTimer::singleShot(0, this, [this, epoch] {
                if (surface_ == nullptr || surface_->sourceEpoch() != epoch) {
                    return;
                }
                if (playbackHoldActive_ && epoch != playbackHoldSourceEpoch_) {
                    invalidateFrozenPlayback(tr("The source changed during comparison."));
                } else if (playbackReleasePending_) {
                    finishDeferredPlaybackHoldRelease();
                }
            });
        });
        connect(surface_, &VideoSurface::positionChanged, this,
                [this](double) { ++positionEpoch_; });
        // Carries the queue depth and the end of a settling transition, so the
        // armed retry never freezes a stale epoch.
        connect(surface_, &VideoSurface::nativePlaybackStatusChanged, this,
                &CompareWindow::onNativeStatusSettled);
        connect(surface_, &QObject::destroyed, this, [this] {
            playbackHoldActive_ = false;
            playbackHoldOriginalPlaying_ = false;
            playbackReleasePending_ = true;
            deferredPlanLockRelease_ = true;
            if (!rendering_) {
                finishDeferredPlaybackHoldRelease();
            }
        });
    }
    onModeChanged(modeBox_->currentIndex());
}

CompareWindow::~CompareWindow() {
    if (preparationTask_) {
        preparationTask_->cancelled.store(true, std::memory_order_release);
    }
    if (!rendering_) {
        requestPlaybackHoldRelease();
    }
    releasePlanLock();
}

void CompareWindow::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    // Reusing the window always captures the current position again.
    if (surface_ != nullptr && surface_->hasVideo()) {
        captureNow();
    }
}

void CompareWindow::keyPressEvent(QKeyEvent* event) {
    // Both the title bar's own close and the keyboard go through the same
    // release path: closing is all either does, so nothing here answers with a
    // dialog result or acts on any other key.
    if (event->key() == Qt::Key_Escape) {
        close();
        event->accept();
        return;
    }
    QDialog::keyPressEvent(event);
}

void CompareWindow::closeEvent(QCloseEvent* event) {
    requestPlaybackHoldRelease();
    QDialog::closeEvent(event);
}

void CompareWindow::hideEvent(QHideEvent* event) {
    dismissCandidateMenus();
    ++selectionGeneration_;
    if (preparationTask_) {
        preparationTask_->cancelled.store(true, std::memory_order_release);
    }
    requestPlaybackHoldRelease();
    QDialog::hideEvent(event);
}

void CompareWindow::captureNow() {
    QPointer<CompareWindow> self(this);
    ++selectionGeneration_;
    playbackNeedsRecapture_ = false;
    playbackReleasePending_ = false;
    deferredPlanLockRelease_ = false;
    acquirePlaybackHold();
    maintainPlaybackHold();
    // Every capture path holds the plan lock, so no freeze is ever taken outside it.
    if (surface_ != nullptr && !ensurePlanLock()) {
        frame_ = QImage();
        captureError_.clear();
        render();
        return;
    }
    captureFrame();
    if (self.isNull()) {
        return;
    }
    render();
    if (self.isNull()) {
        return;
    }
    // The lock's lifetime is the visible window: a programmatic capture on a
    // hidden window must not leave the plan held.
    if (!isVisible()) {
        requestPlaybackHoldRelease();
    }
}

bool CompareWindow::ensurePlanLock() {
    if (nativeLock_.has_value() && *nativeLock_) {
        return true;
    }
    if (surface_ == nullptr) {
        return true;
    }
    QString reasonKey;
    QString reason;
    std::optional<VideoSurface::NativePlanLock> lock =
        surface_->acquireNativePlanLock(&reasonKey, &reason);
    if (lock.has_value()) {
        nativeLock_ = std::move(lock);
        lockError_.clear();
        lockRetryArmed_ = false;
        return true;
    }
    if (reasonKey == QLatin1String("native.playback.noSource")) {
        // No source epoch exists, so no native plan can move: there is nothing to
        // freeze, and the capture itself will say there is no video.
        lockError_.clear();
        return true;
    }
    lockError_ = reason.isEmpty()
                     ? tr("The native plan cannot be frozen right now.")
                     : reason;
    // One retry on the surface's settled signal: an epoch already mid-swap must
    // not be frozen stale, and the refusal is named meanwhile.
    lockRetryArmed_ = reasonKey == QLatin1String("native.playback.lockBusy");
    return false;
}

void CompareWindow::releasePlanLock() {
    // Reset runs the scoped token's destructor, submitting any queued transition.
    nativeLock_.reset();
    lockRetryArmed_ = false;
}

void CompareWindow::onNativeStatusSettled() {
    if (surface_ == nullptr) {
        return;
    }
    updateInfo();
    if (!lockRetryArmed_) {
        return;
    }
    const native::NativePlaybackController::Status& status = surface_->nativePlaybackStatus();
    const bool settled =
        status.pendingRequestId.isEmpty() &&
        status.state != native::NativePlaybackController::State::Admitting &&
        status.state != native::NativePlaybackController::State::Draining;
    if (!settled) {
        return;
    }
    // Exactly one retry. Deferred, because capture pumps the event loop and this
    // runs from the surface's own status publication.
    lockRetryArmed_ = false;
    QTimer::singleShot(0, this, [this] {
        if (ensurePlanLock()) {
            captureNow();
        } else {
            render();
        }
    });
}

void CompareWindow::buildUi() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(kGap, kGap, kGap, kGap);
    root->setSpacing(kGapSmall);

    // No candidate sidebar: both sides are picked from the row under the
    // picture, so the catalogue is not listed twice.
    auto* right = new QVBoxLayout();
    right->setSpacing(kGapSmall);

    // --- header: the mode picker -----------------------------------------
    auto* header = new QHBoxLayout();
    header->setSpacing(kGapSmall);
    header->addWidget(new QLabel(tr("Mode:"), this));
    modeBox_ = new QComboBox(this);
    modeBox_->setObjectName(QStringLiteral("compareMode"));
    for (const CompareMode mode : {CompareMode::SplitH, CompareMode::SplitV,
                                   CompareMode::SideBySide, CompareMode::Flicker,
                                   CompareMode::Difference}) {
        modeBox_->addItem(QString::fromUtf8(modeLabel(mode)),
                          QString::fromLatin1(modeKey(mode)));
    }
    const int sideBySide = modeBox_->findData(QString::fromLatin1(modeKey(CompareMode::SideBySide)));
    if (sideBySide >= 0) {
        modeBox_->setCurrentIndex(sideBySide);
    }
    connect(modeBox_, qOverload<int>(&QComboBox::currentIndexChanged), this,
            &CompareWindow::onModeChanged);
    header->addWidget(modeBox_);

    header->addWidget(new QLabel(tr("Filter:"), this));
    filterBox_ = new QComboBox(this);
    filterBox_->setObjectName(QStringLiteral("compareFilter"));
    filterBox_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    filterBox_->addItem(tr("Bilinear"), true);
    filterBox_->addItem(tr("Nearest Neighbor"), false);
    filterBox_->setToolTip(tr("How the picture is scaled"));
    filterBox_->setAccessibleName(tr("Filter"));
    header->addWidget(filterBox_);

    auto* fit = new QToolButton(this);
    fit->setObjectName(QStringLiteral("compareFit"));
    fit->setText(tr("Fit"));
    fit->setToolTip(tr("Fit the picture"));
    fit->setAccessibleName(tr("Fit the picture"));
    connect(fit, &QToolButton::clicked, this, [this] {
        if (mode_ == CompareMode::SideBySide) grid_->fitToContent();
        else canvas_->fitToContent();
    });
    header->addWidget(fit);

    zoom_ = new QLabel(tr("%1%").arg(100), this);
    zoom_->setObjectName(QStringLiteral("compareZoom"));
    zoom_->setFont(uiFont(10));
    zoom_->setToolTip(tr("View scale"));
    zoom_->setAccessibleName(tr("View scale"));
    zoom_->setStyleSheet(QStringLiteral("color: %1; background: transparent;").arg(w90().name()));
    zoom_->setMinimumWidth(QFontMetrics(zoom_->font()).horizontalAdvance(QStringLiteral("999%")));
    header->addWidget(zoom_);

    header->addStretch(1);
    hoverSwatch_ = new QLabel(this);
    hoverSwatch_->setObjectName(QStringLiteral("compareHoverSwatch"));
    hoverSwatch_->setFixedSize(14, 14);
    hoverSwatch_->hide();
    header->addWidget(hoverSwatch_);
    hover_ = new QLabel(this);
    hover_->setObjectName(QStringLiteral("compareHover"));
    hover_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    hover_->setFont(uiFont(10));
    header->addWidget(hover_);

    right->addLayout(header);

    // Mode knobs live on their own row so side-by-side, which uses none of
    // them, does not leave an empty strip under the header.
    modeControls_ = new QWidget(this);
    auto* view = new QHBoxLayout(modeControls_);
    view->setContentsMargins(0, 0, 0, 0);
    view->setSpacing(kGapSmall);

    splitLabel_ = new QLabel(tr("Split:"), modeControls_);
    view->addWidget(splitLabel_);
    split_ = new QDoubleSpinBox(modeControls_);
    split_->setObjectName(QStringLiteral("compareSplit"));
    split_->setRange(0.0, 1.0);
    split_->setSingleStep(0.01);
    split_->setValue(0.5);
    split_->setToolTip(tr("Divider position"));
    connect(split_, &QDoubleSpinBox::valueChanged, this, &CompareWindow::onSplitChanged);
    view->addWidget(split_);

    flickerLabel_ = new QLabel(tr("Flicker, Hz:"), modeControls_);
    view->addWidget(flickerLabel_);
    flicker_ = new QSpinBox(modeControls_);
    flicker_->setObjectName(QStringLiteral("compareFlicker"));
    flicker_->setRange(1, 30);
    flicker_->setValue(1);
    flicker_->setToolTip(tr("How fast the two pictures alternate"));
    connect(flicker_, &QSpinBox::valueChanged, this, &CompareWindow::onFlickerChanged);
    view->addWidget(flicker_);

    gainLabel_ = new QLabel(tr("Gain:"), modeControls_);
    view->addWidget(gainLabel_);
    gain_ = new QSpinBox(modeControls_);
    gain_->setObjectName(QStringLiteral("compareGain"));
    gain_->setRange(kDiffGainMin, kDiffGainMax);
    gain_->setValue(kDiffGainMin);
    gain_->setToolTip(tr("Amplify the difference"));
    connect(gain_, &QSpinBox::valueChanged, this, &CompareWindow::onGainChanged);
    view->addWidget(gain_);
    view->addStretch(1);
    modeControls_->hide();

    right->addWidget(modeControls_);

    canvas_ = new CompareCanvas(this);
    canvas_->setObjectName(QStringLiteral("compareCanvas"));
    canvas_->setMinimumSize(640, 400);
    connect(canvas_, &CompareCanvas::splitDragged, this, [this](double fraction) {
        const QSignalBlocker blocker(split_);
        split_->setValue(fraction);
    });
    connect(canvas_, &CompareCanvas::candidateChosen, this, &CompareWindow::onCandidateChosen);
    connect(canvas_, &CompareCanvas::hoverPixel, this, &CompareWindow::onHoverPixel);
    connect(canvas_, &CompareCanvas::zoomChanged, this, [this](int percent) {
        if (mode_ != CompareMode::SideBySide && zoom_ != nullptr) {
            zoom_->setText(tr("%1%").arg(percent));
        }
    });
    canvas_->setStyleSheet(
        QStringLiteral("QGraphicsView#compareCanvas { background: %1; border: 1px solid %2;"
                       " border-radius: %3px; }")
            .arg(bg0().name())
            .arg(white(20).name())
            .arg(kGap));
    stack_ = new QStackedWidget(this);
    stack_->addWidget(canvas_);
    // Side-by-side is the multi-pane surface: one pane per side, each with the
    // selector row under its picture. The other modes share one row under the
    // single canvas.
    grid_ = new CompareGrid(this);
    stack_->addWidget(grid_);
    connect(grid_, &CompareGrid::choiceChosen, this, &CompareWindow::onCandidateChosen);
    connect(grid_, &CompareGrid::zoomChanged, this, [this](int percent) {
        if (mode_ == CompareMode::SideBySide && zoom_ != nullptr) {
            zoom_->setText(tr("%1%").arg(percent));
        }
    });
    connect(filterBox_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
        const bool smooth = filterBox_->currentData().toBool();
        canvas_->setSmoothScaling(smooth);
        grid_->setSmoothScaling(smooth);
    });
    right->addWidget(stack_, 1);

    info_ = new QLabel(this);
    info_->setObjectName(QStringLiteral("compareInfo"));
    info_->setWordWrap(true);
    info_->setFixedHeight(kCompareInfoHeight);
    info_->setFont(uiFont(9));
    info_->setStyleSheet(QStringLiteral("color: %1; background: transparent;").arg(w55().name()));
    right->addWidget(info_);

    root->addLayout(right, 1);

    processingOverlay_ = new ProcessingOverlay(this);
    connect(processingOverlay_, &ProcessingOverlay::cancelRequested, this, [this] {
        if (!buildOperationActive_) return;
        ++selectionGeneration_;
        if (preparationTask_) preparationTask_->cancelled.store(true, std::memory_order_release);
        if (!pendingAjiId_.isEmpty())
            ajiErrors_.insert(pendingAjiId_, tr("Comparison engine build cancelled."));
        renderQueued_ = true;
    });

    flickerTimer_ = new QTimer(this);
    connect(flickerTimer_, &QTimer::timeout, this, &CompareWindow::onFlickerTimeout);
}

void CompareWindow::populateCandidates() {
    const QVector<anime4k::ModeInfo> presets = anime4k::availableModes();
    const QVector<native::AnimeJanaiPreset> ajiPresets = native::animeJanaiPresets();
    const native::Backend ajiBackend = ajiBackend_;
    candidates_.clear();
    candidates_.reserve(presets.size() + ajiPresets.size());
    choices_.clear();
    choices_.reserve(presets.size() + ajiPresets.size());
    ajiBackends_.clear();
    ajiErrors_.clear();
    for (const anime4k::ModeInfo& preset : presets) {
        candidates_.append(preset.id);
        ChoiceEntry entry;
        entry.id = preset.id;
        entry.label = preset.label;
        entry.path = categoryPath(preset.category);
        choices_.append(entry);
    }
    for (const native::AnimeJanaiPreset& preset : ajiPresets) {
        if (preset.rife || !preset.id.startsWith(QLatin1String("spatial:"))) {
            continue;
        }
        candidates_.append(preset.id);
        ajiBackends_.insert(preset.id, ajiBackend);
        choices_.append({preset.id, preset.label, {tr("AnimeJaNai"), tr("Spatial models")}});
    }
    selection_.clear();
    if (candidates_.isEmpty()) {
        return;
    }
    // Two sides, always: the chain the player is running opens as A and another
    // candidate as B, so the window opens on a comparison rather than on an
    // empty pane. A catalogue of one compares a chain with itself.
    const qsizetype current =
        std::max<qsizetype>(0, candidates_.indexOf(currentCandidateId_));
    selection_.append(candidates_.value(current));
    const qsizetype other = current == 0 ? std::min<qsizetype>(1, candidates_.size() - 1) : 0;
    if (other != current) {
        selection_.append(candidates_.at(other));
    }
}

void CompareWindow::acquirePlaybackHold() {
    if (surface_ == nullptr) {
        return;
    }
    if (playbackHoldActive_ && playbackHoldOwned()) {
        playbackReleasePending_ = false;
        return;
    }
    playbackHoldActive_ = false;
    playbackHoldOriginalPlaying_ = surface_->isPlaying();
    playbackHoldSourceEpoch_ = surface_->sourceEpoch();
    const quint64 generationBeforePause = surface_->pauseRequestGeneration();
    playbackHoldMutation_ = true;
    if (playbackHoldOriginalPlaying_) {
        surface_->setPaused(true);
    }
    playbackHoldMutation_ = false;
    playbackHoldGeneration_ = surface_->pauseRequestGeneration();
    if (playbackHoldOriginalPlaying_ &&
        playbackHoldGeneration_ != generationBeforePause + 1) {
        playbackHoldOriginalPlaying_ = false;
    }
    playbackHoldActive_ = surface_->sourceEpoch() == playbackHoldSourceEpoch_ &&
                          !surface_->isPlaying();
    playbackReleasePending_ = false;
    playbackHoldFailed_ = false;
    playbackNeedsRecapture_ = false;
    playbackHoldRetryScheduled_ = false;
    playbackHoldRetryCount_ = 0;
}

bool CompareWindow::playbackHoldOwned() const {
    return playbackHoldActive_ && surface_ != nullptr &&
           surface_->sourceEpoch() == playbackHoldSourceEpoch_ &&
           surface_->pauseRequestGeneration() == playbackHoldGeneration_ &&
           !surface_->isPlaying() && surface_->pauseObserved();
}

void CompareWindow::requestPlaybackHoldRelease() {
    playbackReleasePending_ = true;
    deferredPlanLockRelease_ = true;
    if (rendering_) {
        return;
    }
    if (isolatedRenderer_) isolatedRenderer_->stop();
    processingOverlay_->hide();
    finishDeferredPlaybackHoldRelease();
}

void CompareWindow::finishDeferredPlaybackHoldRelease() {
    if (!playbackReleasePending_) {
        return;
    }
    if (rendering_) {
        deferredPlanLockRelease_ = true;
        return;
    }
    if (playbackReleasePending_ && playbackHoldOwned() &&
        playbackHoldOriginalPlaying_ && !playbackHoldFailed_) {
        playbackHoldMutation_ = true;
        surface_->setPaused(false);
        playbackHoldMutation_ = false;
    }
    playbackHoldActive_ = false;
    playbackHoldOriginalPlaying_ = false;
    playbackReleasePending_ = false;
    if (deferredPlanLockRelease_) {
        deferredPlanLockRelease_ = false;
        releasePlanLock();
    }
    if (!isVisible()) {
        frame_ = QImage();
        isolatedOriginal_ = QImage();
        frozenSourceEpoch_ = 0;
        frozenPlaybackTimestamp_.reset();
        renderedA_ = QImage();
        renderedB_ = QImage();
        pendingAjiId_.clear();
        pendingAjiPreparation_ = false;
        canvas_->clear(QString());
        canvas_->setSideImages(QImage(), QImage(), QImage());
        clearGridPanes();
    }
}

void CompareWindow::onPlaybackStateChanged(bool playing) {
    if (!playbackHoldActive_ || playbackHoldMutation_ || surface_ == nullptr) {
        return;
    }
    if (surface_->sourceEpoch() != playbackHoldSourceEpoch_) {
        invalidateFrozenPlayback(tr("The source changed during comparison."));
        return;
    }
    if (!playing) {
        if (surface_->pauseRequestGeneration() != playbackHoldGeneration_) {
            playbackHoldOriginalPlaying_ = false;
            playbackHoldGeneration_ = surface_->pauseRequestGeneration();
        }
        if (playbackNeedsRecapture_) {
            QTimer::singleShot(0, this, [this] { tryRecaptureAfterPause(); });
        }
        return;
    }
    if (surface_->pauseRequestGeneration() != playbackHoldGeneration_) {
        invalidateFrozenPlayback(tr("Playback resumed during comparison."));
        return;
    }
    const bool hadFrozenPair = !frame_.isNull() && frozenPlaybackTimestamp_.has_value();
    playbackNeedsRecapture_ = isVisible() && !hadFrozenPair;
    ++selectionGeneration_;
    if (preparationTask_) {
        preparationTask_->cancelled.store(true, std::memory_order_release);
    }
    pendingAjiId_.clear();
    pendingAjiPreparation_ = false;
    frame_ = QImage();
    frozenPlaybackTimestamp_.reset();
    renderedA_ = QImage();
    renderedB_ = QImage();
    isolatedOriginal_ = QImage();
    captureError_ = hadFrozenPair
                        ? tr("Playback advanced after the frozen comparison frame; capture again.")
                        : tr("Playback resumed while the comparison was holding it; waiting for pause.");
    canvas_->clear(captureError_);
    canvas_->setSideImages(QImage(), QImage(), QImage());
    clearGridPanes();
    updateInfo();
    surface_->reassertPaused();
    maintainPlaybackHold();
    render();
}

void CompareWindow::maintainPlaybackHold() {
    if (!playbackHoldActive_ || surface_ == nullptr) {
        return;
    }
    if (surface_->sourceEpoch() != playbackHoldSourceEpoch_) {
        invalidateFrozenPlayback(tr("The source changed during comparison."));
        return;
    }
    const quint64 requestGeneration = surface_->pauseRequestGeneration();
    if (requestGeneration != playbackHoldGeneration_) {
        if (surface_->pauseObserved() && !surface_->isPlaying()) {
            playbackHoldOriginalPlaying_ = false;
            playbackHoldGeneration_ = requestGeneration;
        } else {
            invalidateFrozenPlayback(tr("Playback resumed during comparison."));
            return;
        }
    }
    if (!surface_->pauseObserved() || surface_->isPlaying()) {
        if (playbackHoldRetryCount_ >= kMaxPauseReassertions) {
            surface_->reassertPaused();
            playbackHoldActive_ = false;
            playbackHoldOriginalPlaying_ = false;
            playbackNeedsRecapture_ = false;
            playbackHoldFailed_ = true;
            captureError_ = tr("Playback could not be held paused for comparison.");
            updateInfo();
            render();
            return;
        }
        ++playbackHoldRetryCount_;
        surface_->reassertPaused();
        if (!playbackHoldRetryScheduled_) {
            playbackHoldRetryScheduled_ = true;
            QTimer::singleShot(kPollIntervalMs, this, [this] {
                playbackHoldRetryScheduled_ = false;
                maintainPlaybackHold();
            });
        }
    } else {
        playbackHoldRetryCount_ = 0;
    }
}

void CompareWindow::tryRecaptureAfterPause() {
    if (!playbackNeedsRecapture_) {
        return;
    }
    if (captureFrameActive_ || rendering_) {
        QTimer::singleShot(kPollIntervalMs, this,
                           [this] { tryRecaptureAfterPause(); });
        return;
    }
    if (isVisible() && playbackHoldActive_ && playbackHoldOwned()) {
        playbackNeedsRecapture_ = false;
        captureNow();
    } else if (!isVisible()) {
        playbackNeedsRecapture_ = false;
    }
}

void CompareWindow::invalidateFrozenPlayback(const QString& reason) {
    playbackHoldActive_ = false;
    playbackHoldOriginalPlaying_ = false;
    playbackNeedsRecapture_ = false;
    playbackHoldRetryScheduled_ = false;
    playbackReleasePending_ = true;
    deferredPlanLockRelease_ = true;
    ++selectionGeneration_;
    if (preparationTask_) {
        preparationTask_->cancelled.store(true, std::memory_order_release);
    }
    pendingAjiId_.clear();
    pendingAjiPreparation_ = false;
    frame_ = QImage();
    frozenPlaybackTimestamp_.reset();
    renderedA_ = QImage();
    renderedB_ = QImage();
    isolatedOriginal_ = QImage();
    captureError_ = reason;
    canvas_->clear(reason);
    canvas_->setSideImages(QImage(), QImage(), QImage());
    clearGridPanes();
    updateInfo();
    render();
    if (!rendering_ && deferredPlanLockRelease_) {
        deferredPlanLockRelease_ = false;
        releasePlanLock();
    }
}

void CompareWindow::setAjiBackend(native::Backend backend) {
    if (ajiBackend_ != backend) ++selectionGeneration_;
    ajiBackend_ = backend;
    for (auto it = ajiBackends_.begin(); it != ajiBackends_.end(); ++it) {
        it.value() = backend;
    }
    ajiErrors_.clear();
}
QString CompareWindow::sideA() const { return selection_.value(0); }

QString CompareWindow::sideB() const { return selection_.value(1); }

QImage CompareWindow::grabSettledFrame() {
    QPointer<CompareWindow> self(this);
    if (surface_ == nullptr) {
        return QImage();
    }
    // Require both a quiet position pump and identical pixels after any seek.
    QImage previous;
    double previousPosition = 0.0;
    int stableRounds = 0;
    quint64 epoch = positionEpoch_;
    int quietEpochRounds = 0;
    for (int waited = 0; waited < kSettleTimeoutMs; waited += kPollIntervalMs) {
        QCoreApplication::processEvents();
        if (self.isNull()) {
            return QImage();
        }
        QThread::msleep(kPollIntervalMs);
        // The event loop above can deliver a close that destroys the borrowed
        // surface; the QPointer turns that into a refusal here instead of a
        // read of freed memory.
        if (surface_ == nullptr) {
            return QImage();
        }
        const QImage probe = surface_->grabFramebuffer();
        const double now = surface_->position();
        if (positionEpoch_ != epoch) {
            epoch = positionEpoch_;
            quietEpochRounds = 0;
            stableRounds = 0;
        } else {
            ++quietEpochRounds;
        }
        if (probe.isNull()) {
            // No framebuffer to read at all: nothing to wait for, and nothing
            // to fall back on either.
            previous = QImage();
            stableRounds = 0;
            continue;
        }
        if (previous.isNull() || probe != previous ||
            std::abs(now - previousPosition) >= kSettleQuietSeconds) {
            stableRounds = 0;
        } else {
            ++stableRounds;
        }
        previous = probe;
        previousPosition = now;
        if (quietEpochRounds >= kMinObserveRounds && stableRounds >= kSettleRounds) {
            return previous;
        }
    }
    return QImage();
}

std::optional<double> CompareWindow::readPlaybackTimestamp() {
    QPointer<CompareWindow> self(this);
    QPointer<VideoSurface> source = surface_;
    if (source == nullptr) {
        return std::nullopt;
    }
    const uint64_t expectedEpoch = source->sourceEpoch();
    auto result = std::make_shared<PlaybackTimestampResult>();
    const AsyncRequestId request = source->requestPlaybackTimestamp(
        [result](uint64_t epoch, std::optional<double> timestamp) {
            result->sourceEpoch = epoch;
            result->timestamp = timestamp;
            result->finished.store(true, std::memory_order_release);
        });
    if (!request.valid()) {
        return std::nullopt;
    }
    for (int waited = 0; waited < kSettleTimeoutMs; waited += 10) {
        QCoreApplication::processEvents();
        if (self.isNull()) {
            return std::nullopt;
        }
        if (source == nullptr || source->sourceEpoch() != expectedEpoch) {
            return std::nullopt;
        }
        if (result->finished.load(std::memory_order_acquire)) {
            break;
        }
        QThread::msleep(10);
    }
    if (!result->finished.load(std::memory_order_acquire) ||
        result->sourceEpoch != expectedEpoch ||
        result->sourceEpoch != source->sourceEpoch() || !result->timestamp.has_value() ||
        !std::isfinite(*result->timestamp)) {
        return std::nullopt;
    }
    return result->timestamp;
}

void CompareWindow::captureFrame() {
    struct ActiveGuard {
        bool& active;
        ~ActiveGuard() { active = false; }
    };
    captureFrameActive_ = true;
    ActiveGuard activeGuard{captureFrameActive_};
    QPointer<CompareWindow> self(this);
    // Capture the current settled frame without seeking; failure is visible.
    frame_ = QImage();
    isolatedOriginal_ = QImage();
    renderedAId_.clear();
    renderedBId_.clear();
    captureError_.clear();
    if (surface_ == nullptr) {
        captureError_ = tr("No playback surface");
        return;
    }
    if (!surface_->hasVideo()) {
        captureError_ = tr("No video to compare");
        return;
    }
    const uint64_t sourceEpoch = surface_->sourceEpoch();
    const quint64 captureGeneration = selectionGeneration_;
    for (int waited = 0; waited < kSettleTimeoutMs &&
                         (!surface_->pauseObserved() || surface_->isPlaying());
         waited += kPollIntervalMs) {
        QCoreApplication::processEvents();
        if (self.isNull()) {
            return;
        }
        if (surface_ == nullptr || surface_->sourceEpoch() != sourceEpoch ||
            selectionGeneration_ != captureGeneration) {
            if (!self.isNull() && captureError_.isEmpty()) {
                captureError_ = tr("Playback changed during frame capture");
            }
            return;
        }
        const quint64 requestGeneration = surface_->pauseRequestGeneration();
        if (requestGeneration != playbackHoldGeneration_ &&
            surface_->pauseObserved() && !surface_->isPlaying()) {
            playbackHoldOriginalPlaying_ = false;
            playbackHoldGeneration_ = requestGeneration;
        }
        QThread::msleep(kPollIntervalMs);
    }
    if (!playbackHoldOwned()) {
        if (captureError_.isEmpty()) {
            captureError_ = tr("Playback did not settle while pausing for comparison");
        }
        return;
    }
    const std::optional<double> timestampBefore = readPlaybackTimestamp();
    if (self.isNull()) {
        return;
    }
    if (!timestampBefore.has_value()) {
        captureError_ = tr("Could not capture the playback timestamp");
        return;
    }
    const QImage grabbed = grabSettledFrame();
    if (self.isNull()) {
        return;
    }
    if (grabbed.isNull() || surface_ == nullptr ||
        surface_->sourceEpoch() != sourceEpoch) {
        captureError_ = surface_ == nullptr || surface_->sourceEpoch() != sourceEpoch
                            ? tr("The source changed during frame capture")
                            : tr("Could not capture a new frame");
        return;
    }
    const std::optional<double> playbackTimestamp = readPlaybackTimestamp();
    if (self.isNull()) {
        return;
    }
    if (!playbackTimestamp.has_value() || !playbackHoldOwned() ||
        std::abs(*playbackTimestamp - *timestampBefore) > 1e-6 || surface_ == nullptr ||
        surface_->sourceEpoch() != sourceEpoch) {
        captureError_ = surface_ == nullptr || surface_->sourceEpoch() != sourceEpoch
                            ? tr("The source changed during frame capture")
                            : (!playbackTimestamp.has_value()
                                   ? tr("Could not capture the playback timestamp")
                                   : tr("The playback timestamp changed while capturing the frame"));
        return;
    }
    frame_ = grabbed;
    isolatedRenderer_.reset();
    isolatedOriginal_ = QImage();
    frozenSourceEpoch_ = sourceEpoch;
    frozenPlaybackTimestamp_ = playbackTimestamp;
}

QImage CompareWindow::renderIsolated(const QString& id) {
    QPointer<CompareWindow> self(this);
    const quint64 generation = activeRenderGeneration_;
    const auto cancelled = [self, generation] {
        return self == nullptr || !self->isVisible() ||
               generation != self->selectionGeneration_ ||
               !self->playbackHoldOwned() || self->surface_ == nullptr ||
               self->surface_->sourceEpoch() != self->frozenSourceEpoch_;
    };
    const auto progress = [self](const QString& detail) {
        if (self == nullptr || self->buildOperationActive_) return;
        self->buildOperationActive_ = true;
        self->processingOverlay_->anchorTo(self);
        self->processingOverlay_->begin(CompareWindow::tr("Build comparison TensorRT engine"));
        self->processingOverlay_->setStage(-1, 0, detail, QString());
    };
    if (!isolatedRenderer_) {
        isolatedRenderer_ = std::make_shared<CompareRenderer>();
        const auto renderer = isolatedRenderer_;
        pendingAjiId_ = id;
        updateInfo();
        const bool captured = renderer->capture(surface_->sourceUrl(), *frozenPlaybackTimestamp_,
                                                frame_.size(), cancelled, progress);
        if (self == nullptr) return {};
        if (!captured) {
            pendingAjiId_.clear();
            if (!cancelled()) ajiErrors_.insert(id, renderer->error());
            abandonIsolatedRenderer();
            return {};
        }
        isolatedOriginal_ = renderer->render(QStringLiteral("off"), {}, cancelled, progress);
        if (self == nullptr) return {};
        if (cancelled()) {
            finishIsolatedRetry();
            return {};
        }
        if (isolatedOriginal_.isNull()) {
            pendingAjiId_.clear();
            ajiErrors_.insert(id, renderer->error());
            abandonIsolatedRenderer();
            return {};
        }
    }
    native::AnimeJanaiPlan plan;
    if (ajiBackends_.contains(id)) {
        const native::Backend backend = ajiBackends_.value(id);
        auto task = std::make_shared<AjiPreparationTask>();
        preparationTask_ = task;
        pendingAjiId_ = id;
        pendingAjiPreparation_ = true;
        updateInfo();
        QThreadPool::globalInstance()->start(QRunnable::create([task, id, backend] {
            task->plan = native::prepareAnimeJanaiPlan(id, backend, [task] {
                return task->cancelled.load(std::memory_order_acquire);
            });
            task->finished.store(true, std::memory_order_release);
        }));
        while (!task->finished.load(std::memory_order_acquire)) {
            if (cancelled()) task->cancelled.store(true, std::memory_order_release);
            if (self != nullptr) QCoreApplication::processEvents();
            QThread::msleep(kPollIntervalMs);
        }
        if (self != nullptr) preparationTask_.reset();
        if (cancelled()) {
            task->cancelled.store(true, std::memory_order_release);
            finishIsolatedRetry();
            return {};
        }
        pendingAjiPreparation_ = false;
        plan = std::move(*task->plan);
        if (!plan.available) {
            pendingAjiId_.clear();
            ajiErrors_.insert(id, plan.reason);
            return {};
        }
    }
    const auto renderer = isolatedRenderer_;
    if (id == QLatin1String("off")) {
        pendingAjiId_.clear();
        return isolatedOriginal_;
    }
    pendingAjiId_ = id;
    updateInfo();
    const QImage image = renderer->render(id, plan, cancelled, progress);
    const bool stopped = cancelled();
    if (stopped || image.isNull()) abandonIsolatedRenderer();
    if (self == nullptr) return {};
    if (!stopped && image.isNull()) {
        ajiErrors_.insert(id, renderer->error());
        settleIsolatedBuild(false, tr("Comparison engine build failed: %1").arg(renderer->error()));
    } else if (image.isNull()) {
        settleIsolatedBuild(false, tr("Comparison engine build cancelled."));
    } else {
        settleIsolatedBuild(true, tr("Comparison engine ready."));
    }
    pendingAjiId_.clear();
    return stopped ? QImage() : image;
}

void CompareWindow::abandonIsolatedRenderer() {
    if (const auto renderer = isolatedRenderer_) {
        // Keeping the shared copy alive across stop() leaves the GL release on
        // this call's stack, so the context is still current for it.
        renderer->stop();
        isolatedRenderer_.reset();
    }
}

void CompareWindow::finishIsolatedRetry() {
    pendingAjiPreparation_ = false;
    pendingAjiId_.clear();
    settleIsolatedBuild(false, tr("Comparison engine build cancelled."));
}

void CompareWindow::settleIsolatedBuild(bool ok, const QString& summary) {
    if (buildOperationActive_) {
        buildOperationActive_ = false;
        processingOverlay_->finish(ok, summary);
    }
    if (!isVisible()) processingOverlay_->hide();
}

void CompareWindow::render() {
    QPointer<CompareWindow> self(this);
    if (rendering_) {
        renderQueued_ = true;
        return;
    }
    rendering_ = true;
    do {
        renderQueued_ = false;
        activeRenderGeneration_ = selectionGeneration_;
        renderOnce();
        if (self.isNull()) {
            return;
        }
    } while (renderQueued_);
    rendering_ = false;
    if (!isVisible() || !playbackHoldOwned()) {
        if (isolatedRenderer_) isolatedRenderer_->stop();
    }
    if (playbackReleasePending_ && (!isVisible() || !playbackHoldActive_)) {
        finishDeferredPlaybackHoldRelease();
    } else if (deferredPlanLockRelease_ && !playbackHoldActive_) {
        deferredPlanLockRelease_ = false;
        releasePlanLock();
    }
}

void CompareWindow::renderOnce() {
    QPointer<CompareWindow> self(this);
    if (!frame_.isNull() && surface_ == nullptr) {
        frame_ = QImage();
        captureError_ = tr("The playback surface was destroyed");
    } else if (!frame_.isNull() && surface_->sourceEpoch() != frozenSourceEpoch_) {
        frame_ = QImage();
        captureError_ = tr("The source changed after the frame was captured");
    }
    if (frame_.isNull()) {
        // A failed capture, an unfrozen plan and a window with no frame are
        // different faults; the named one is shown and no stale picture remains.
        renderedA_ = QImage();
        renderedB_ = QImage();
        canvas_->clear(!lockError_.isEmpty()
                           ? lockError_
                           : (captureError_.isEmpty() ? tr("No frame to compare")
                                                      : captureError_));
        canvas_->setSideImages(QImage(), QImage(), QImage());
        clearGridPanes();
        updateInfo();
        return;
    }
    const QString renderAId = sideA();
    const QString renderBId = sideB();
    const auto candidateOutput = [this](const QString& id) {
        if (ajiErrors_.contains(id)) return QImage();
        const bool backendMatches = !ajiBackends_.contains(id) || renderedBackend_ == ajiBackend_;
        if (backendMatches && id == renderedAId_ && !renderedA_.isNull()) return renderedA_;
        if (backendMatches && id == renderedBId_ && !renderedB_.isNull()) return renderedB_;
        return renderIsolated(id);
    };
    const QImage a = candidateOutput(renderAId);
    if (self.isNull()) {
        return;
    }
    const QImage b = activeRenderGeneration_ == selectionGeneration_
                         ? (renderBId == renderAId ? a : candidateOutput(renderBId))
                         : QImage();
    if (self.isNull()) {
        return;
    }
    if (surface_ == nullptr || surface_->sourceEpoch() != frozenSourceEpoch_) {
        frame_ = QImage();
        renderedA_ = QImage();
        renderedB_ = QImage();
        captureError_ = surface_ == nullptr ? tr("The playback surface was destroyed")
                                            : tr("The source changed during comparison rendering");
        canvas_->clear(captureError_);
        canvas_->setSideImages(QImage(), QImage(), QImage());
        clearGridPanes();
        updateInfo();
        return;
    }
    if (!playbackHoldOwned()) {
        frame_ = QImage();
        frozenPlaybackTimestamp_.reset();
        playbackNeedsRecapture_ = false;
        if (captureError_.isEmpty()) {
            captureError_ = tr("Playback changed during comparison.");
        }
    }
    if (frame_.isNull()) {
        renderedA_ = QImage();
        renderedB_ = QImage();
        clearGridPanes();
        canvas_->clear(captureError_.isEmpty()
                           ? tr("Playback changed during comparison.")
                           : captureError_);
        canvas_->setSideImages(QImage(), QImage(), QImage());
        updateInfo();
        return;
    }
    if (activeRenderGeneration_ != selectionGeneration_) return;
    renderedA_ = a;
    renderedB_ = b;
    renderedAId_ = renderAId;
    renderedBId_ = renderBId;
    renderedBackend_ = ajiBackend_;
    QImage difference;
    if (!a.isNull() && !b.isNull() && a.size() == b.size()) {
        // Gain 1 is the honest picture, so it is the colour-mapped one;
        // anything above it is an explicitly amplified grey.
        difference = gain_->value() > 1 ? differenceGrayscale(a, b, gain_->value())
                                        : differenceJet(a, b);
    }
    canvas_->setSideNames(labelFor(sideA()), labelFor(sideB()));
    canvas_->setChoices(choices_, sideA(), sideB());
    canvas_->setMode(mode_);
    canvas_->setSideImages(a, b, difference);
    stack_->setCurrentIndex(mode_ == CompareMode::SideBySide ? 1 : 0);
    if (mode_ == CompareMode::SideBySide) {
        // A deferred teardown never outlives a fresh pair: the panes it would
        // delete are still alive, so republishing them cancels the deletion.
        gridTeardownDeferred_ = false;
        grid_->setChoices(choices_);
        grid_->setItems({sideA(), sideB()}, {a, b},
                        {labelFor(sideA()), labelFor(sideB())});
    }
    updateInfo();
}

void CompareWindow::updateFlickerTimer() {
    if (mode_ != CompareMode::Flicker) {
        flickerTimer_->stop();
        return;
    }
    const int interval = static_cast<int>(1000.0 / std::max(1, flicker_->value()));
    flickerTimer_->setInterval(interval);
    if (!flickerTimer_->isActive()) {
        flickerTimer_->start();
    }
}

void CompareWindow::onFlickerTimeout() {
    flickerShowA_ = !flickerShowA_;
    canvas_->setFlickerShowA(flickerShowA_);
}

void CompareWindow::onModeChanged(int index) {
    dismissCandidateMenus();
    CompareMode mode;
    if (index < 0 || !modeFromKey(modeBox_->itemData(index).toString().toLatin1().constData(), mode)) {
        return;
    }
    mode_ = mode;
    // A control is only present, not merely disabled, for the modes that read
    // it: a disabled knob is a knob that invites a click before admitting it
    // does nothing, and a hidden one cannot be asked.
    const bool isSplit = mode == CompareMode::SplitH || mode == CompareMode::SplitV;
    const bool isFlicker = mode == CompareMode::Flicker;
    const bool isDifference = mode == CompareMode::Difference;
    splitLabel_->setVisible(isSplit);
    split_->setVisible(isSplit);
    flickerLabel_->setVisible(isFlicker);
    flicker_->setVisible(isFlicker);
    gainLabel_->setVisible(isDifference);
    gain_->setVisible(isDifference);
    modeControls_->setVisible(isSplit || isFlicker || isDifference);
    canvas_->setSplit(split_->value());
    canvas_->setFlickerRate(flicker_->value());
    canvas_->setDiffGain(gain_->value());
    updateFlickerTimer();
    if (mode == CompareMode::Flicker) {
        canvas_->setFlickerShowA(flickerShowA_);
    }
    render();
}

void CompareWindow::onSplitChanged(double value) {
    canvas_->setSplit(value);
}

void CompareWindow::onFlickerChanged(int value) {
    canvas_->setFlickerRate(value);
    updateFlickerTimer();
}

void CompareWindow::onGainChanged(int value) {
    canvas_->setDiffGain(value);
    render();
}

void CompareWindow::onCandidateChosen(int side, const QString& id) {
    // Menu activation guard: this slot runs inside QAction::activate, and the
    // render below pumps the event loop, so a source change can invalidate and
    // clear the grid while the activation is still on the stack. Pane deletion
    // is deferred until the activation unwinds (see clearGridPanes).
    struct ActivationGuard {
        CompareWindow* owner;
        explicit ActivationGuard(CompareWindow* window) : owner(window) {
            ++owner->menuActivationDepth_;
        }
        ~ActivationGuard() { --owner->menuActivationDepth_; }
    };
    ActivationGuard activation(this);
    dismissCandidateMenus();
    if (!candidates_.contains(id) || side < 0 || side > 1) {
        return;
    }
    ajiErrors_.remove(id);
    // The badge menus are the only chooser, so this is the whole selection: A
    // first, B second. Picking the same chain on both sides is allowed - two
    // frames of one chain is a legitimate way to look at it.
    ++selectionGeneration_;
    if (side == 0) {
        if (selection_.isEmpty()) {
            selection_.append(id);
        } else {
            selection_[0] = id;
        }
    } else if (side == 1) {
        if (selection_.size() < 2) {
            selection_.append(id);
        } else {
            selection_[1] = id;
        }
    } else {
        return;
    }
    if (side == 0 && renderedAId_ != id) {
        renderedA_ = {};
        renderedAId_.clear();
    }
    if (side == 1 && renderedBId_ != id) {
        renderedB_ = {};
        renderedBId_.clear();
    }
    canvas_->setSideNames(labelFor(sideA()), labelFor(sideB()));
    canvas_->setChoices(choices_, sideA(), sideB());
    canvas_->setSideImages(renderedA_, renderedB_, {});
    if (mode_ == CompareMode::SideBySide)
        grid_->setItems({sideA(), sideB()}, {renderedA_, renderedB_},
                        {labelFor(sideA()), labelFor(sideB())});
    render();
}

void CompareWindow::dismissCandidateMenus() {
    for (QToolButton* badge : findChildren<QToolButton*>()) {
        if (badge->menu() != nullptr) badge->menu()->close();
    }
}

void CompareWindow::clearGridPanes() {
    if (grid_ == nullptr) {
        return;
    }
    if (menuActivationDepth_ > 0) {
        gridTeardownDeferred_ = true;
        QTimer::singleShot(0, this, [this] { flushDeferredGridClear(); });
        return;
    }
    grid_->setItems({}, {}, {});
}

void CompareWindow::flushDeferredGridClear() {
    if (!gridTeardownDeferred_ || grid_ == nullptr) {
        return;
    }
    if (menuActivationDepth_ > 0) {
        QTimer::singleShot(0, this, [this] { flushDeferredGridClear(); });
        return;
    }
    gridTeardownDeferred_ = false;
    grid_->setItems({}, {}, {});
}

void CompareWindow::onHoverPixel(int x, int y, bool valid, QRgb rgb) {
    if (!valid) {
        hover_->clear();
        hoverSwatch_->hide();
        return;
    }
    const QColor color(rgb);
    hover_->setText(tr("%1, %2 · %3").arg(x).arg(y).arg(color.name()));
    hoverSwatch_->setStyleSheet(
        QStringLiteral("background: %1; border: 1px solid %2; border-radius: 2px;")
            .arg(color.name(), white(40).name()));
    hoverSwatch_->show();
}

void CompareWindow::updateInfo() {
    QStringList parts;
    if (!lockError_.isEmpty()) parts.append(tr("Native plan not frozen: %1").arg(lockError_));
    if (!captureError_.isEmpty()) parts.append(tr("Capture error: %1").arg(captureError_));
    if (!pendingAjiId_.isEmpty()) {
        parts.append(pendingAjiPreparation_ ? tr("Preparing AnimeJaNai %1…").arg(labelFor(pendingAjiId_))
                                           : tr("Rendering %1…").arg(labelFor(pendingAjiId_)));
    }
    for (const QString& id : {sideA(), sideB()}) {
        if (ajiErrors_.contains(id))
            parts.append(tr("%1 unavailable: %2").arg(labelFor(id), ajiErrors_.value(id)));
    }
    if (queuedNativeChanges() > 0)
        parts.append(tr("%n native change(s) queued until Compare closes", nullptr, queuedNativeChanges()));
    if (mode_ == CompareMode::Difference && !renderedA_.isNull() && !renderedB_.isNull() &&
        renderedA_.size() == renderedB_.size())
        parts.append(tr("mean difference %1/255")
                         .arg(QString::number(meanAbsoluteDifference(renderedA_, renderedB_), 'f', 2)));
    info_->setText(parts.join(QStringLiteral("  ·  ")));
    info_->setVisible(!parts.isEmpty());
}

QString CompareWindow::labelFor(const QString& id) const {
    if (ajiBackends_.contains(id)) {
        const QVector<native::AnimeJanaiPreset> presets = native::animeJanaiPresets();
        for (const native::AnimeJanaiPreset& preset : presets) {
            if (preset.id == id) {
                return preset.label;
            }
        }
        return QStringLiteral("—");
    }
    const anime4k::Candidate candidate = anime4k::candidateForId(id);
    return candidate.label.isEmpty() ? QStringLiteral("—") : candidate.label;
}

}  // namespace shell
}  // namespace mplayer
