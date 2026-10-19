#include "CompareWindow.hpp"

#include "Anime4K.hpp"
#include "CompareCanvas.hpp"
#include "CompareGrid.hpp"
#include "CompareRenderer.hpp"
#include "ShellTheme.hpp"
#include "VideoSurface.hpp"
#include "native/components/AnimeJanaiCatalogWatcher.hpp"
#include "native/components/AnimeJanaiRuntime.hpp"
#include "native/status/NativePlaybackController.hpp"

#include <QApplication>
#include <QCloseEvent>
#include <QColor>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFileInfo>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QRunnable>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QStackedWidget>
#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <optional>
#include <utility>

namespace mplayer {
namespace shell {

using mplayer::VideoSurface;

namespace {

constexpr int kGap = 10;
constexpr int kGapSmall = 6;
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

// A side can keep showing a model the catalogue no longer offers. The caption
// reuses the catalogue's existing words: no new translatable string is
// introduced, and a language switch restates the same entry in place.
QString unavailableChoiceCaption(const QString& label) {
    return QCoreApplication::translate("SettingsDialog", "%1 (not available)").arg(label);
}

QString missingCatalogueReason() {
    return QCoreApplication::translate("AnimeJanaiRuntime",
                                       "The selected AnimeJaNai preset is not recognised.");
}

QFrame* createVerticalDivider(QWidget* parent) {
    auto* line = new QFrame(parent);
    line->setFrameShape(QFrame::VLine);
    line->setFixedWidth(1);
    line->setFixedHeight(20);
    line->setStyleSheet(QStringLiteral("background-color: #282c34; border: none;"));
    return line;
}

QString windowStyleSheet() {
    return QStringLiteral(
        "QDialog {"
        "  background-color: #0d0e11;"
        "  color: #e5e7eb;"
        "}"
        "QFrame#compareToolbarCard {"
        "  background-color: #16181d;"
        "  border: 1px solid #282c34;"
        "  border-radius: 6px;"
        "}"
        "QLabel {"
        "  color: #9ca3af;"
        "  background: transparent;"
        "}"
        "QComboBox {"
        "  background-color: #1e2128;"
        "  color: #e5e7eb;"
        "  border: 1px solid #282c34;"
        "  border-radius: 4px;"
        "  padding: 3px 10px;"
        "  min-height: 22px;"
        "}"
        "QComboBox:hover {"
        "  border-color: #3b82f6;"
        "}"
        "QComboBox QAbstractItemView {"
        "  background-color: #16181d;"
        "  color: #e5e7eb;"
        "  border: 1px solid #282c34;"
        "  selection-background-color: #2563eb;"
        "  selection-color: #ffffff;"
        "}"
        "QToolButton, QPushButton {"
        "  background-color: #1e2128;"
        "  color: #e5e7eb;"
        "  border: 1px solid #282c34;"
        "  border-radius: 4px;"
        "  padding: 3px 10px;"
        "  min-height: 22px;"
        "  font-weight: 500;"
        "}"
        "QToolButton:hover, QPushButton:hover {"
        "  background-color: #262a34;"
        "  border-color: #3b82f6;"
        "  color: #ffffff;"
        "}"
        "QPushButton#comparePinBtn:checked {"
        "  background-color: rgba(59, 130, 246, 0.22);"
        "  border-color: #3b82f6;"
        "  color: #60a5fa;"
        "}"
        "QSpinBox, QDoubleSpinBox {"
        "  background-color: #1e2128;"
        "  color: #e5e7eb;"
        "  border: 1px solid #282c34;"
        "  border-radius: 4px;"
        "  padding: 2px 6px;"
        "  min-height: 22px;"
        "}"
        "QSlider::groove:horizontal {"
        "  height: 4px;"
        "  background: #282c34;"
        "  border-radius: 2px;"
        "}"
        "QSlider::sub-page:horizontal {"
        "  background: #3b82f6;"
        "  border-radius: 2px;"
        "}"
        "QSlider::handle:horizontal {"
        "  background: #e5e7eb;"
        "  border: 1px solid #3b82f6;"
        "  width: 12px;"
        "  margin: -4px 0;"
        "  border-radius: 6px;"
        "}");
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
    setStyleSheet(windowStyleSheet());
    if (surface_ != nullptr) {
        const native::AnimeJanaiPlan activePlan = surface_->animeJanaiPlan();
        if (activePlan.enabled && !activePlan.presetId.isEmpty()) {
            const QVector<native::AnimeJanaiPreset> publicPresets = native::animeJanaiPresets();
            const auto active = std::find_if(
                publicPresets.cbegin(), publicPresets.cend(),
                [&activePlan](const native::AnimeJanaiPreset& preset) {
                    if (preset.rife || !preset.id.startsWith(QLatin1String("spatial:"))) {
                        return false;
                    }
                    if (preset.id == activePlan.presetId) {
                        return true;
                    }
                    if ((activePlan.presetId == QLatin1String("builtin1001") ||
                         activePlan.presetId == QLatin1String("builtin1002")) &&
                        preset.label.contains(QLatin1String("HD_V3.1_Balanced"),
                                              Qt::CaseInsensitive)) {
                        return true;
                    }
                    if (activePlan.presetId == QLatin1String("builtin1003") &&
                        preset.label.contains(QLatin1String("HD_V3.1_Performance"),
                                              Qt::CaseInsensitive)) {
                        return true;
                    }
                    return false;
                });
            if (active != publicPresets.cend()) {
                currentCandidateId_ = active->id;
            }
        }
    }
    buildUi();
    populateCandidates();
    // One observer for this window's whole life. It follows the saved manager
    // configuration by default; the window never edits that configuration.
    catalogWatcher_ = new native::AnimeJanaiCatalogWatcher(this);
    connect(catalogWatcher_, &native::AnimeJanaiCatalogWatcher::catalogChanged, this,
            &CompareWindow::onCatalogChanged);
    catalogWatcher_->refresh();
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
    if (togglingPin_) {
        return;
    }
    // Reusing the window always captures the current position again.
    if (surface_ != nullptr && surface_->hasVideo()) {
        captureNow();
    }
}

void CompareWindow::togglePin(bool pinned) {
    if (pinned_ == pinned) {
        if (pinButton_ != nullptr && pinButton_->isChecked() != pinned) {
            const QSignalBlocker blocker(pinButton_);
            pinButton_->setChecked(pinned);
        }
        return;
    }
    pinned_ = pinned;
    if (pinButton_ != nullptr) {
        const QSignalBlocker blocker(pinButton_);
        pinButton_->setChecked(pinned_);
        pinButton_->setText(pinned_ ? tr("Pinned") : tr("Pin"));
        pinButton_->setAccessibleName(pinned_ ? tr("Pinned") : tr("Pin"));
    }
    const bool wasVisible = isVisible();
    togglingPin_ = true;
    setWindowFlag(Qt::WindowStaysOnTopHint, pinned_);
    if (wasVisible) {
        show();
        raise();
        activateWindow();
    }
#ifdef Q_OS_WIN
    if (const HWND hwnd = reinterpret_cast<HWND>(winId()); hwnd != nullptr && IsWindow(hwnd)) {
        SetWindowPos(hwnd, pinned_ ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
#endif
    togglingPin_ = false;
}

void CompareWindow::swapSides() {
    if (selection_.size() < 2) {
        return;
    }
    dismissCandidateMenus();
    ++selectionGeneration_;
    std::swap(selection_[0], selection_[1]);
    std::swap(renderedA_, renderedB_);
    std::swap(renderedAId_, renderedBId_);
    render();
}

void CompareWindow::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        close();
        event->accept();
        return;
    }
    if (event->modifiers() == Qt::NoModifier) {
        switch (event->key()) {
            case Qt::Key_P:
                togglePin(!pinned_);
                event->accept();
                return;
            case Qt::Key_S:
                swapSides();
                event->accept();
                return;
            case Qt::Key_R:
                captureNow();
                event->accept();
                return;
            case Qt::Key_F:
                if (mode_ == CompareMode::SideBySide) {
                    grid_->fitToContent();
                } else {
                    canvas_->fitToContent();
                }
                event->accept();
                return;
            case Qt::Key_Space:
                if (!event->isAutoRepeat() && canvas_ != nullptr) {
                    canvas_->setHeldSide(0);
                }
                event->accept();
                return;
            default:
                break;
        }
    }
    QDialog::keyPressEvent(event);
}

void CompareWindow::keyReleaseEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Space && !event->isAutoRepeat() && canvas_ != nullptr) {
        canvas_->setHeldSide(-1);
        event->accept();
        return;
    }
    QDialog::keyReleaseEvent(event);
}

void CompareWindow::closeEvent(QCloseEvent* event) {
    requestPlaybackHoldRelease();
    QDialog::closeEvent(event);
}

void CompareWindow::hideEvent(QHideEvent* event) {
    if (togglingPin_) {
        QDialog::hideEvent(event);
        return;
    }
    dismissCandidateMenus();
    ++selectionGeneration_;
    if (preparationTask_) {
        preparationTask_->cancelled.store(true, std::memory_order_release);
    }
    requestPlaybackHoldRelease();
    QDialog::hideEvent(event);
}

void CompareWindow::changeEvent(QEvent* event) {
    QDialog::changeEvent(event);
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
}

void CompareWindow::retranslateUi() {
    // Only words are restated. The captured frame, both rendered pictures, the
    // A/B ids, the opaque errors, the camera, the selection and the playback
    // hold are read from their own state and never touched.
    setWindowTitle(tr("Quality Comparison"));
    if (modeCaption_ != nullptr) {
        modeCaption_->setText(tr("Mode:"));
    }
    if (modeBox_ != nullptr) {
        // Restated through each item's stable mode key, never its index.
        for (int index = 0; index < modeBox_->count(); ++index) {
            CompareMode mode;
            if (modeFromKey(modeBox_->itemData(index).toString().toLatin1().constData(), mode)) {
                modeBox_->setItemText(index, QString::fromUtf8(modeLabel(mode)));
            }
        }
    }
    if (filterCaption_ != nullptr) {
        filterCaption_->setText(tr("Filter:"));
    }
    if (filterBox_ != nullptr) {
        const int bilinear = filterBox_->findData(true);
        if (bilinear >= 0) filterBox_->setItemText(bilinear, tr("Bilinear"));
        const int nearest = filterBox_->findData(false);
        if (nearest >= 0) filterBox_->setItemText(nearest, tr("Nearest Neighbor"));
        filterBox_->setToolTip(tr("How the picture is scaled"));
        filterBox_->setAccessibleName(tr("Filter"));
    }
    if (fitButton_ != nullptr) {
        fitButton_->setText(tr("Fit"));
        fitButton_->setToolTip(tr("Fit the picture (F)"));
        fitButton_->setAccessibleName(tr("Fit the picture"));
    }
    if (zoom_ != nullptr) {
        zoom_->setText(tr("%1%").arg(zoomPercent_));
        zoom_->setToolTip(tr("View scale"));
        zoom_->setAccessibleName(tr("View scale"));
    }
    if (swapButton_ != nullptr) {
        swapButton_->setText(tr("Swap A ⇄ B"));
        swapButton_->setToolTip(tr("Swap Side A and Side B (S)"));
        swapButton_->setAccessibleName(tr("Swap A ⇄ B"));
    }
    if (recaptureButton_ != nullptr) {
        recaptureButton_->setText(tr("Recapture"));
        recaptureButton_->setToolTip(tr("Capture the current player frame (R)"));
        recaptureButton_->setAccessibleName(tr("Recapture"));
    }
    if (pinButton_ != nullptr) {
        pinButton_->setText(pinned_ ? tr("Pinned") : tr("Pin"));
        pinButton_->setToolTip(tr("Keep Compare window always on top (P)"));
        pinButton_->setAccessibleName(pinned_ ? tr("Pinned") : tr("Pin"));
    }
    if (splitLabel_ != nullptr) splitLabel_->setText(tr("Split:"));
    if (flickerLabel_ != nullptr) flickerLabel_->setText(tr("Flicker, Hz:"));
    if (gainLabel_ != nullptr) gainLabel_->setText(tr("Gain:"));
    if (split_ != nullptr) split_->setToolTip(tr("Divider position"));
    if (flicker_ != nullptr) flicker_->setToolTip(tr("How fast the two pictures alternate"));
    if (gain_ != nullptr) gain_->setToolTip(tr("Amplify the difference"));
    refreshHoverText();

    // Selector captions are translated catalogue text, so they are re-read
    // from the same semantic catalogue and republished through the existing
    // choice setters. This neither rebuilds a surface nor renders a picture.
    if (rendering_) {
        // The render pumps the event loop and reads the choice lists; defer
        // only the catalogue snapshot while the app-owned chrome is already
        // restated above.
        catalogueRefreshPending_ = true;
        scheduleCatalogueRefresh();
        updateInfo();
    } else {
        refreshCandidates();
    }

    if (processingOverlay_ != nullptr) {
        if (buildOperationActive_) {
            // Running: retitle without resetting the run, its timer or its
            // progress. The stage words are the caller's and stay verbatim.
            processingOverlay_->setOperationTitle(tr("Build comparison TensorRT engine"));
        } else if (buildSummaryKind_ != BuildSummaryKind::None) {
            // Terminal: only the app-owned wrapper around an opaque failure
            // detail is rebuilt; the detail itself is preserved verbatim.
            processingOverlay_->setSummary(buildSummaryText());
        }
    }
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
        requestPlaybackHoldRelease();
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
    // The lock's lifetime is the visible window with a valid captured frame:
    // a failed capture or a programmatic capture on a hidden window must hand
    // playback and the plan lock back immediately.
    if (!isVisible() || frame_.isNull()) {
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

    auto* right = new QVBoxLayout();
    right->setSpacing(kGapSmall);

    // --- Single workstation top toolbar card ------------------------------
    auto* toolbarCard = new QFrame(this);
    toolbarCard->setObjectName(QStringLiteral("compareToolbarCard"));
    auto* header = new QHBoxLayout(toolbarCard);
    header->setContentsMargins(10, 6, 10, 6);
    header->setSpacing(kGapSmall);

    modeCaption_ = new QLabel(tr("Mode:"), toolbarCard);
    header->addWidget(modeCaption_);
    modeBox_ = new QComboBox(toolbarCard);
    modeBox_->setObjectName(QStringLiteral("compareMode"));
    modeBox_->setFocusPolicy(Qt::NoFocus);
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

    header->addWidget(createVerticalDivider(toolbarCard));

    filterCaption_ = new QLabel(tr("Filter:"), toolbarCard);
    header->addWidget(filterCaption_);
    filterBox_ = new QComboBox(toolbarCard);
    filterBox_->setObjectName(QStringLiteral("compareFilter"));
    filterBox_->setFocusPolicy(Qt::NoFocus);
    filterBox_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    filterBox_->addItem(tr("Bilinear"), true);
    filterBox_->addItem(tr("Nearest Neighbor"), false);
    filterBox_->setToolTip(tr("How the picture is scaled"));
    filterBox_->setAccessibleName(tr("Filter"));
    header->addWidget(filterBox_);

    fitButton_ = new QToolButton(toolbarCard);
    fitButton_->setObjectName(QStringLiteral("compareFit"));
    fitButton_->setFocusPolicy(Qt::NoFocus);
    fitButton_->setCursor(Qt::PointingHandCursor);
    fitButton_->setText(tr("Fit"));
    fitButton_->setToolTip(tr("Fit the picture (F)"));
    fitButton_->setAccessibleName(tr("Fit the picture"));
    connect(fitButton_, &QToolButton::clicked, this, [this] {
        if (mode_ == CompareMode::SideBySide) grid_->fitToContent();
        else canvas_->fitToContent();
    });
    header->addWidget(fitButton_);

    zoom_ = new QLabel(tr("%1%").arg(100), toolbarCard);
    zoom_->setObjectName(QStringLiteral("compareZoom"));
    zoom_->setFont(monoFont(9));
    zoom_->setAlignment(Qt::AlignCenter);
    zoom_->setToolTip(tr("View scale"));
    zoom_->setAccessibleName(tr("View scale"));
    zoom_->setStyleSheet(QStringLiteral(
        "color: #e5e7eb; background: #1e2128; border: 1px solid #282c34;"
        " border-radius: 4px; padding: 2px 6px;"));
    zoom_->setMinimumWidth(
        QFontMetrics(zoom_->font()).horizontalAdvance(QStringLiteral("9999%")) + 12);
    header->addWidget(zoom_);

    header->addWidget(createVerticalDivider(toolbarCard));

    swapButton_ = new QPushButton(tr("Swap A ⇄ B"), toolbarCard);
    swapButton_->setObjectName(QStringLiteral("compareSwap"));
    swapButton_->setFocusPolicy(Qt::NoFocus);
    swapButton_->setCursor(Qt::PointingHandCursor);
    swapButton_->setToolTip(tr("Swap Side A and Side B (S)"));
    swapButton_->setAccessibleName(tr("Swap A ⇄ B"));
    connect(swapButton_, &QPushButton::clicked, this, &CompareWindow::swapSides);
    header->addWidget(swapButton_);

    recaptureButton_ = new QPushButton(tr("Recapture"), toolbarCard);
    recaptureButton_->setObjectName(QStringLiteral("compareRecapture"));
    recaptureButton_->setFocusPolicy(Qt::NoFocus);
    recaptureButton_->setCursor(Qt::PointingHandCursor);
    recaptureButton_->setToolTip(tr("Capture the current player frame (R)"));
    recaptureButton_->setAccessibleName(tr("Recapture"));
    connect(recaptureButton_, &QPushButton::clicked, this, &CompareWindow::captureNow);
    header->addWidget(recaptureButton_);

    pinButton_ = new QPushButton(tr("Pin"), toolbarCard);
    pinButton_->setObjectName(QStringLiteral("comparePinBtn"));
    pinButton_->setCheckable(true);
    pinButton_->setFocusPolicy(Qt::NoFocus);
    pinButton_->setCursor(Qt::PointingHandCursor);
    pinButton_->setToolTip(tr("Keep Compare window always on top (P)"));
    pinButton_->setAccessibleName(tr("Pin"));
    connect(pinButton_, &QPushButton::toggled, this, &CompareWindow::togglePin);
    header->addWidget(pinButton_);

    // Inline contextual mode controls inside the single top toolbar so switching
    // modes never shifts the video viewport up or down.
    modeDivider_ = createVerticalDivider(toolbarCard);
    header->addWidget(modeDivider_);
    modeDivider_->hide();

    modeControls_ = new QWidget(toolbarCard);
    auto* view = new QHBoxLayout(modeControls_);
    view->setContentsMargins(0, 0, 0, 0);
    view->setSpacing(kGapSmall);

    splitLabel_ = new QLabel(tr("Split:"), modeControls_);
    view->addWidget(splitLabel_);
    splitSlider_ = new QSlider(Qt::Horizontal, modeControls_);
    splitSlider_->setRange(0, 100);
    splitSlider_->setValue(50);
    splitSlider_->setFixedWidth(90);
    splitSlider_->setFocusPolicy(Qt::NoFocus);
    view->addWidget(splitSlider_);
    split_ = new QDoubleSpinBox(modeControls_);
    split_->setObjectName(QStringLiteral("compareSplit"));
    split_->setRange(0.0, 1.0);
    split_->setSingleStep(0.01);
    split_->setValue(0.5);
    split_->setToolTip(tr("Divider position"));
    connect(split_, &QDoubleSpinBox::valueChanged, this, &CompareWindow::onSplitChanged);
    connect(splitSlider_, &QSlider::valueChanged, this, [this](int sliderVal) {
        split_->setValue(sliderVal / 100.0);
    });
    view->addWidget(split_);

    flickerLabel_ = new QLabel(tr("Flicker, Hz:"), modeControls_);
    view->addWidget(flickerLabel_);
    flickerSlider_ = new QSlider(Qt::Horizontal, modeControls_);
    flickerSlider_->setRange(1, 30);
    flickerSlider_->setValue(1);
    flickerSlider_->setFixedWidth(80);
    flickerSlider_->setFocusPolicy(Qt::NoFocus);
    view->addWidget(flickerSlider_);
    flicker_ = new QSpinBox(modeControls_);
    flicker_->setObjectName(QStringLiteral("compareFlicker"));
    flicker_->setRange(1, 30);
    flicker_->setValue(1);
    flicker_->setToolTip(tr("How fast the two pictures alternate"));
    connect(flicker_, &QSpinBox::valueChanged, this, &CompareWindow::onFlickerChanged);
    connect(flickerSlider_, &QSlider::valueChanged, flicker_, &QSpinBox::setValue);
    view->addWidget(flicker_);

    gainLabel_ = new QLabel(tr("Gain:"), modeControls_);
    view->addWidget(gainLabel_);
    gainSlider_ = new QSlider(Qt::Horizontal, modeControls_);
    gainSlider_->setRange(kDiffGainMin, kDiffGainMax);
    gainSlider_->setValue(kDiffGainMin);
    gainSlider_->setFixedWidth(80);
    gainSlider_->setFocusPolicy(Qt::NoFocus);
    view->addWidget(gainSlider_);
    gain_ = new QSpinBox(modeControls_);
    gain_->setObjectName(QStringLiteral("compareGain"));
    gain_->setRange(kDiffGainMin, kDiffGainMax);
    gain_->setValue(kDiffGainMin);
    gain_->setToolTip(tr("Amplify the difference"));
    connect(gain_, &QSpinBox::valueChanged, this, &CompareWindow::onGainChanged);
    connect(gainSlider_, &QSlider::valueChanged, gain_, &QSpinBox::setValue);
    view->addWidget(gain_);
    modeControls_->hide();

    header->addWidget(modeControls_);

    header->addStretch(1);
    hoverSwatch_ = new QLabel(toolbarCard);
    hoverSwatch_->setObjectName(QStringLiteral("compareHoverSwatch"));
    hoverSwatch_->setFixedSize(14, 14);
    hoverSwatch_->hide();
    header->addWidget(hoverSwatch_);
    hover_ = new QLabel(toolbarCard);
    hover_->setObjectName(QStringLiteral("compareHover"));
    hover_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    hover_->setFont(monoFont(9));
    hover_->setStyleSheet(QStringLiteral("color: #d1d5db;"));
    header->addWidget(hover_);

    right->addWidget(toolbarCard);

    canvas_ = new CompareCanvas(this);
    canvas_->setObjectName(QStringLiteral("compareCanvas"));
    canvas_->setMinimumSize(640, 400);
    connect(canvas_, &CompareCanvas::splitDragged, this, [this](double fraction) {
        const QSignalBlocker blocker(split_);
        split_->setValue(fraction);
        if (splitSlider_ != nullptr) {
            const QSignalBlocker sliderBlocker(splitSlider_);
            splitSlider_->setValue(static_cast<int>(std::lround(fraction * 100.0)));
        }
    });
    connect(canvas_, &CompareCanvas::candidateChosen, this, &CompareWindow::onCandidateChosen);
    connect(canvas_, &CompareCanvas::hoverPixel, this, &CompareWindow::onHoverPixel);
    connect(canvas_, &CompareCanvas::zoomChanged, this, [this](int percent) {
        zoomPercent_ = percent;
        if (mode_ != CompareMode::SideBySide && zoom_ != nullptr) {
            zoom_->setText(tr("%1%").arg(percent));
        }
    });
    canvas_->setStyleSheet(
        QStringLiteral("QGraphicsView#compareCanvas { background: #0b0c0e; border: 1px solid #282c34;"
                       " border-radius: 6px; }"));
    stack_ = new QStackedWidget(this);
    stack_->addWidget(canvas_);
    // Side-by-side is the multi-pane surface: one pane per side, each with the
    // selector row under its picture. The other modes share one row under the
    // single canvas.
    grid_ = new CompareGrid(this);
    grid_->setStyleSheet(
        QStringLiteral("CompareGrid { background: #282c34; border: 1px solid #282c34; border-radius: 6px; }"));
    stack_->addWidget(grid_);
    connect(grid_, &CompareGrid::choiceChosen, this, &CompareWindow::onCandidateChosen);
    connect(grid_, &CompareGrid::hoverPixel, this, &CompareWindow::onHoverPixel);
    connect(grid_, &CompareGrid::zoomChanged, this, [this](int percent) {
        zoomPercent_ = percent;
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
    info_->setStyleSheet(QStringLiteral("color: #9ca3af; background: transparent;"));
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

void CompareWindow::rebuildCandidates() {
    const QVector<anime4k::ModeInfo> presets = anime4k::availableModes();
    const QVector<native::AnimeJanaiPreset> ajiPresets = native::animeJanaiPresets();
    const QString shadersDir = anime4k::findShadersDir();
    const native::Backend ajiBackend = ajiBackend_;

    QStringList candidates;
    QVector<ChoiceEntry> choices;
    QHash<QString, QString> baseLabels;
    QHash<QString, native::Backend> backends;
    candidates.reserve(presets.size() + ajiPresets.size());
    choices.reserve(presets.size() + ajiPresets.size());
    for (const anime4k::ModeInfo& preset : presets) {
        const anime4k::Candidate candidate = anime4k::candidateForId(preset.id, shadersDir);
        const bool complete = anime4k::chainIsComplete(candidate, shadersDir);
        ChoiceEntry entry;
        entry.id = preset.id;
        entry.label = preset.label;
        entry.path = categoryPath(preset.category);
        baseLabels.insert(preset.id, preset.label);
        if (!complete) {
            entry.disabled = true;
            QStringList missingFiles;
            for (const QString& file : candidate.files) {
                if (shadersDir.isEmpty() || !QFileInfo::exists(QDir(shadersDir).filePath(file))) {
                    missingFiles.append(QFileInfo(file).fileName());
                }
            }
            if (missingFiles.isEmpty()) {
                for (const QString& file : preset.files) {
                    missingFiles.append(QFileInfo(file).fileName());
                }
            }
            entry.tooltip =
                tr("Missing shader file(s): %1").arg(missingFiles.join(QStringLiteral(", ")));
        } else {
            candidates.append(preset.id);
        }
        choices.append(entry);
    }
    for (const native::AnimeJanaiPreset& preset : ajiPresets) {
        if (preset.rife || !preset.id.startsWith(QLatin1String("spatial:"))) {
            continue;
        }
        // The registered catalogue is offered exactly as enumerated: an entry
        // that turns out to be unbuildable names its refusal when it is
        // rendered, rather than being second-guessed here.
        baseLabels.insert(preset.id, preset.label);
        choices.append({preset.id, preset.label, {tr("AnimeJaNai"), tr("Spatial models")},
                        false, {}});
        candidates.append(preset.id);
        backends.insert(preset.id, ajiBackend);
    }
    // A side keeps the id it was showing. When the catalogue no longer offers
    // it, the window adds one disabled entry so the refusal is visible instead
    // of silently substituting another model.
    for (const QString& selected : selection_) {
        if (selected.isEmpty()) {
            continue;
        }
        const bool offered = std::any_of(
            choices.cbegin(), choices.cend(),
            [&selected](const ChoiceEntry& entry) { return entry.id == selected; });
        if (offered) {
            continue;
        }
        // The base name comes from the previous pass, never from a caption
        // that already carries the suffix, so refreshes cannot stack
        // "(not available)" on itself.
        QString baseLabel = choiceBaseLabels_.value(selected);
        if (baseLabel.isEmpty()) {
            const anime4k::Candidate candidate = anime4k::candidateForId(selected);
            baseLabel = candidate.label;
        }
        if (baseLabel.isEmpty()) {
            baseLabel = selected;
        }
        ChoiceEntry missingEntry;
        missingEntry.id = selected;
        missingEntry.label = unavailableChoiceCaption(baseLabel);
        missingEntry.disabled = true;
        // The id left the catalogue, so its old reason no longer applies; the
        // refusal is the catalogue's own "not recognised" wording.
        missingEntry.tooltip = missingCatalogueReason();
        baseLabels.insert(selected, baseLabel);
        choices.append(missingEntry);
    }
    candidates_ = std::move(candidates);
    choices_ = std::move(choices);
    choiceBaseLabels_ = std::move(baseLabels);
    ajiBackends_ = std::move(backends);
}

void CompareWindow::chooseDefaultSelection() {
    selection_.clear();
    if (candidates_.isEmpty() && currentCandidateId_.isEmpty()) {
        return;
    }
    // The requested chain is A even when the catalogue cannot offer it: that
    // missing id is then shown, disabled, rather than replaced. B is the first
    // different renderable candidate, so a requested model that is unavailable
    // still opens beside something real.
    const QString primary =
        currentCandidateId_.isEmpty() ? candidates_.value(0) : currentCandidateId_;
    if (primary.isEmpty()) {
        return;
    }
    selection_.append(primary);
    for (const QString& id : candidates_) {
        if (id != primary) {
            selection_.append(id);
            break;
        }
    }
}

void CompareWindow::populateCandidates() {
    ajiErrors_.clear();
    rebuildCandidates();
    chooseDefaultSelection();
    // The requested chain may be outside what the catalogue offers. The second
    // pass adds that id's explicit disabled entry; it never replaces the id.
    rebuildCandidates();
    publishCandidateChoices();
    updateInfo();
}

void CompareWindow::refreshCandidates() {
    rebuildCandidates();
    publishCandidateChoices();
    updateInfo();
}

void CompareWindow::publishCandidateChoices() {
    if (canvas_ == nullptr) {
        return;
    }
    dismissCandidateMenus();
    // Labels and choices only: the pictures, the frozen frame, the camera and
    // the pane content are untouched, so a refresh never refits or rebuilds.
    canvas_->setSideNames(labelFor(sideA()), labelFor(sideB()));
    canvas_->setChoices(choices_, sideA(), sideB());
    if (grid_ != nullptr) {
        grid_->setChoices(choices_, {sideA(), sideB()});
    }
}

void CompareWindow::onCatalogChanged() {
    invalidateAjiCatalogueCache();
    catalogueRefreshPending_ = true;
    if (rendering_) {
        // The render pumps the event loop, so rebuilding its inputs here would
        // race the run reading them. The invalidation above has already
        // cancelled it; the lists are refreshed once it unwinds.
        scheduleCatalogueRefresh();
        return;
    }
    applyCatalogueRefresh();
}

void CompareWindow::applyCatalogueRefresh() {
    catalogueRefreshPending_ = false;
    refreshCandidates();
}

void CompareWindow::scheduleCatalogueRefresh() {
    if (catalogueRefreshScheduled_) {
        return;
    }
    catalogueRefreshScheduled_ = true;
    QTimer::singleShot(kPollIntervalMs, this, [this] {
        catalogueRefreshScheduled_ = false;
        if (rendering_) {
            scheduleCatalogueRefresh();
            return;
        }
        applyCatalogueRefresh();
    });
}

void CompareWindow::invalidateAjiCatalogueCache() {
    // Only AnimeJaNai outputs depend on the observed catalogue. Anime4K images,
    // the frozen frame and the frozen timestamp survive a model paste or edit.
    if (!renderedAId_.isEmpty() && ajiBackends_.contains(renderedAId_)) {
        renderedA_ = QImage();
        renderedAId_.clear();
    }
    if (!renderedBId_.isEmpty() && ajiBackends_.contains(renderedBId_)) {
        renderedB_ = QImage();
        renderedBId_.clear();
    }
    const bool ajiInFlight =
        !pendingAjiId_.isEmpty() && ajiBackends_.contains(pendingAjiId_);
    if (!rendering_ || !(pendingAjiPreparation_ || ajiInFlight)) {
        return;
    }
    // The build or render in flight was started against the old graph. Bumping
    // the generation makes its own cancellation checks refuse to publish, and
    // cancelling the task stops the worker without waiting for it.
    ++selectionGeneration_;
    if (preparationTask_) {
        preparationTask_->cancelled.store(true, std::memory_order_release);
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
    playbackHoldActive_ = surface_->sourceEpoch() == playbackHoldSourceEpoch_;
    playbackReleasePending_ = false;
    playbackHoldFailed_ = false;
    playbackNeedsRecapture_ = false;
    playbackHoldRetryScheduled_ = false;
    playbackHoldRetryCount_ = 0;
}

bool CompareWindow::playbackHoldOwned() const {
    return playbackHoldActive_ && surface_ != nullptr &&
           surface_->sourceEpoch() == playbackHoldSourceEpoch_ &&
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
    if (surface_ != nullptr && surface_->pauseRequestGeneration() != playbackHoldGeneration_) {
        // An explicit pause request arrived after Compare took its hold, so
        // closing Compare must preserve the newer pause rather than resuming.
        playbackHoldOriginalPlaying_ = false;
        playbackHoldGeneration_ = surface_->pauseRequestGeneration();
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

void CompareWindow::setCurrentCandidateId(const QString& candidateId) {
    QString resolved = candidateId;
    if (surface_ != nullptr) {
        const native::AnimeJanaiPlan activePlan = surface_->animeJanaiPlan();
        if (activePlan.enabled && !activePlan.presetId.isEmpty()) {
            const QVector<native::AnimeJanaiPreset> publicPresets = native::animeJanaiPresets();
            const auto active = std::find_if(
                publicPresets.cbegin(), publicPresets.cend(),
                [&activePlan](const native::AnimeJanaiPreset& preset) {
                    if (preset.rife || !preset.id.startsWith(QLatin1String("spatial:"))) {
                        return false;
                    }
                    if (preset.id == activePlan.presetId) {
                        return true;
                    }
                    if ((activePlan.presetId == QLatin1String("builtin1001") ||
                         activePlan.presetId == QLatin1String("builtin1002")) &&
                        preset.label.contains(QLatin1String("HD_V3.1_Balanced"),
                                              Qt::CaseInsensitive)) {
                        return true;
                    }
                    if (activePlan.presetId == QLatin1String("builtin1003") &&
                        preset.label.contains(QLatin1String("HD_V3.1_Performance"),
                                              Qt::CaseInsensitive)) {
                        return true;
                    }
                    return false;
                });
            if (active != publicPresets.cend()) {
                resolved = active->id;
            }
        }
    }
    if (resolved == currentCandidateId_) {
        return;
    }
    currentCandidateId_ = resolved;
    chooseDefaultSelection();
    refreshCandidates();
}
QString CompareWindow::sideA() const { return selection_.value(0); }

QString CompareWindow::sideB() const { return selection_.value(1); }

QString CompareWindow::selectedCandidateId(int side) const {
    return side == 0 ? sideA() : (side == 1 ? sideB() : QString());
}

QString CompareWindow::candidateError(const QString& id) const {
    const QString engineError = ajiErrors_.value(id);
    return engineError.isEmpty() ? unavailableReason(id) : engineError;
}

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
        const QImage probe = surface_->grabVideoFramebuffer();
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
    std::optional<double> timestampBefore;
    std::optional<double> playbackTimestamp;
    QImage grabbed;
    for (int attempt = 0; attempt < 3; ++attempt) {
        timestampBefore = readPlaybackTimestamp();
        if (self.isNull()) {
            return;
        }
        if (!timestampBefore.has_value()) {
            captureError_ = tr("Could not capture the playback timestamp");
            return;
        }
        grabbed = grabSettledFrame();
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
        playbackTimestamp = readPlaybackTimestamp();
        if (self.isNull()) {
            return;
        }
        if (playbackTimestamp.has_value() &&
            std::abs(*playbackTimestamp - *timestampBefore) <= 1e-6) {
            break;
        }
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
        self->buildSummaryKind_ = BuildSummaryKind::None;
        self->buildFailureDetail_.clear();
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
        const native::AnimeJanaiSourceContext source = surface_->animeJanaiSourceContext();
        QThreadPool::globalInstance()->start(QRunnable::create([task, id, backend, source] {
            const native::Anime4KShaderProgramResolver shaderResolver =
                [](const QString& presetId, QString* reason) {
                    return anime4k::filterShaderProgramForCandidate(
                        presetId, anime4k::findShadersDir(), reason);
                };
            task->plan = native::prepareAnimeJanaiPlan(
                id, backend, [task] {
                    return task->cancelled.load(std::memory_order_acquire);
                }, source, shaderResolver);
            task->finished.store(true, std::memory_order_release);
        }));
        while (!task->finished.load(std::memory_order_acquire)) {
            if (cancelled()) task->cancelled.store(true, std::memory_order_release);
            if (self != nullptr) QCoreApplication::processEvents();
            QThread::msleep(kPollIntervalMs);
        }
        if (self == nullptr) {
            task->cancelled.store(true, std::memory_order_release);
            return {};
        }
        preparationTask_.reset();
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
    if (stopped || image.isNull()) renderer->stop();
    if (self == nullptr) return {};
    if (!stopped && image.isNull()) {
        ajiErrors_.insert(id, renderer->error());
        finishIsolatedBuild(BuildSummaryKind::Failed, renderer->error());
    } else if (image.isNull()) {
        finishIsolatedBuild(BuildSummaryKind::Cancelled);
    } else {
        finishIsolatedBuild(BuildSummaryKind::Ready);
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
    finishIsolatedBuild(BuildSummaryKind::Cancelled);
}

void CompareWindow::finishIsolatedBuild(BuildSummaryKind kind, const QString& detail) {
    if (!buildOperationActive_) {
        if (!isVisible()) processingOverlay_->hide();
        return;
    }
    buildSummaryKind_ = kind;
    buildFailureDetail_ = kind == BuildSummaryKind::Failed ? detail : QString();
    buildOperationActive_ = false;
    processingOverlay_->finish(kind == BuildSummaryKind::Ready, buildSummaryText());
    if (!isVisible()) processingOverlay_->hide();
}

QString CompareWindow::buildSummaryText() const {
    switch (buildSummaryKind_) {
        case BuildSummaryKind::Ready:
            return tr("Comparison engine ready.");
        case BuildSummaryKind::Failed:
            return tr("Comparison engine build failed: %1").arg(buildFailureDetail_);
        case BuildSummaryKind::Cancelled:
            return tr("Comparison engine build cancelled.");
        case BuildSummaryKind::None:
            break;
    }
    return QString();
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
    } while (renderQueued_ ||
             (isVisible() && playbackHoldOwned() && !frame_.isNull() &&
              activeRenderGeneration_ != selectionGeneration_));
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
        // An id the catalogue no longer offers has no renderer and no backend;
        // asking the isolated core for it would compare against a model that is
        // not there. The side stays empty and updateInfo() names the refusal.
        if (id.isEmpty() || !candidates_.contains(id)) return QImage();
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
        grid_->setChoices(choices_, {sideA(), sideB()});
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
    if (splitSlider_ != nullptr) splitSlider_->setVisible(isSplit);
    split_->setVisible(isSplit);
    flickerLabel_->setVisible(isFlicker);
    if (flickerSlider_ != nullptr) flickerSlider_->setVisible(isFlicker);
    flicker_->setVisible(isFlicker);
    gainLabel_->setVisible(isDifference);
    if (gainSlider_ != nullptr) gainSlider_->setVisible(isDifference);
    gain_->setVisible(isDifference);
    const bool hasModeControls = isSplit || isFlicker || isDifference;
    if (modeDivider_ != nullptr) modeDivider_->setVisible(hasModeControls);
    modeControls_->setVisible(hasModeControls);
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
    if (splitSlider_ != nullptr) {
        const QSignalBlocker blocker(splitSlider_);
        splitSlider_->setValue(static_cast<int>(std::lround(value * 100.0)));
    }
    canvas_->setSplit(value);
}

void CompareWindow::onFlickerChanged(int value) {
    if (flickerSlider_ != nullptr) {
        const QSignalBlocker blocker(flickerSlider_);
        flickerSlider_->setValue(value);
    }
    canvas_->setFlickerRate(value);
    updateFlickerTimer();
}

void CompareWindow::onGainChanged(int value) {
    if (gainSlider_ != nullptr) {
        const QSignalBlocker blocker(gainSlider_);
        gainSlider_->setValue(value);
    }
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
    // Ignore duplicate re-entrant activation for the exact same side and model
    // while that side is already actively rendering.
    const QString currentSideId = side == 0 ? sideA() : sideB();
    if (rendering_ && currentSideId == id && !ajiErrors_.contains(id)) {
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
    if (side == 0 && (renderedAId_ != id || ajiBackends_.contains(id))) {
        renderedA_ = {};
        renderedAId_.clear();
    }
    if (side == 1 && (renderedBId_ != id || ajiBackends_.contains(id))) {
        renderedB_ = {};
        renderedBId_.clear();
    }
    canvas_->setSideNames(labelFor(sideA()), labelFor(sideB()));
    canvas_->setChoices(choices_, sideA(), sideB());
    grid_->setChoices(choices_, {sideA(), sideB()});
    render();
}

void CompareWindow::dismissCandidateMenus() {
    for (QMenu* menu : findChildren<QMenu*>()) {
        menu->close();
        menu->hide();
    }
    while (QWidget* popup = QApplication::activePopupWidget()) {
        popup->close();
        popup->hide();
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
    hoverValid_ = valid;
    hoverX_ = x;
    hoverY_ = y;
    hoverRgb_ = rgb;
    refreshHoverText();
}

void CompareWindow::refreshHoverText() {
    if (hover_ == nullptr) {
        return;
    }
    if (!hoverValid_) {
        hover_->clear();
        if (hoverSwatch_ != nullptr) hoverSwatch_->hide();
        return;
    }
    const QColor color(hoverRgb_);
    hover_->setText(
        tr("%1, %2 · %3 (%4, %5, %6)")
            .arg(hoverX_)
            .arg(hoverY_)
            .arg(color.name().toUpper())
            .arg(color.red())
            .arg(color.green())
            .arg(color.blue()));
    if (hoverSwatch_ != nullptr) {
        hoverSwatch_->setStyleSheet(
            QStringLiteral("background: %1; border: 1px solid #4b5563; border-radius: 3px;")
                .arg(color.name()));
        hoverSwatch_->show();
    }
}

QString CompareWindow::unavailableReason(const QString& id) const {
    if (id.isEmpty()) {
        return QString();
    }
    for (const ChoiceEntry& entry : choices_) {
        if (entry.id != id) {
            continue;
        }
        if (entry.disabled && !entry.tooltip.isEmpty()) {
            // The catalogue's own refusal, verbatim: a missing graph or shader
            // is opaque to this window and is never re-worded.
            return entry.tooltip;
        }
        break;
    }
    if (!candidates_.contains(id)) {
        return missingCatalogueReason();
    }
    return QString();
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
        if (id.isEmpty()) {
            continue;
        }
        if (!candidates_.contains(id)) {
            parts.append(tr("%1 unavailable: %2").arg(labelFor(id), unavailableReason(id)));
        } else if (ajiErrors_.contains(id)) {
            parts.append(tr("%1 unavailable: %2").arg(labelFor(id), ajiErrors_.value(id)));
        }
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
    // The choice list is the snapshot every label on screen was built from, so
    // it stays the freshest name even for an id the catalogue has dropped.
    for (const ChoiceEntry& entry : choices_) {
        if (entry.id == id) {
            return entry.label;
        }
    }
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
