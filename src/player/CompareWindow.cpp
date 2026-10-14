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
#include <QColor>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QResizeEvent>
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

QFrame* makeToolbarDivider(QWidget* parent) {
    auto* line = new QFrame(parent);
    line->setObjectName(QStringLiteral("compareToolbarSep"));
    line->setFixedSize(1, 18);
    return line;
}

QString compareWindowStyleSheet() {
    return QStringLiteral(
        "QDialog {"
        "  background-color: #0d0e11;"
        "  color: #e8eaed;"
        "}"
        "QFrame#compareHeaderBar {"
        "  background-color: #16181d;"
        "  border: 1px solid #282c34;"
        "  border-radius: 6px;"
        "}"
        "QFrame#compareToolbarSep {"
        "  background-color: #282c34;"
        "  border: none;"
        "}"
        "QLabel#compareFieldLabel {"
        "  color: #8b929e;"
        "  font-size: 11px;"
        "  font-weight: 600;"
        "  background: transparent;"
        "}"
        "QComboBox {"
        "  background-color: #1e2128;"
        "  color: #e8eaed;"
        "  border: 1px solid #2e333d;"
        "  border-radius: 4px;"
        "  padding: 3px 22px 3px 8px;"
        "  min-height: 22px;"
        "  font-size: 12px;"
        "  font-weight: 500;"
        "}"
        "QComboBox:hover {"
        "  background-color: #252932;"
        "  border-color: #3e4552;"
        "}"
        "QComboBox::drop-down {"
        "  border: none;"
        "  width: 18px;"
        "}"
        "QComboBox QAbstractItemView {"
        "  background-color: #16181d;"
        "  color: #e8eaed;"
        "  border: 1px solid #2e333d;"
        "  border-radius: 4px;"
        "  selection-background-color: #2563eb;"
        "  selection-color: #ffffff;"
        "  padding: 2px;"
        "}"
        "QDoubleSpinBox, QSpinBox {"
        "  background-color: #1e2128;"
        "  color: #e8eaed;"
        "  border: 1px solid #2e333d;"
        "  border-radius: 4px;"
        "  padding: 2px 6px;"
        "  min-height: 22px;"
        "  font-size: 12px;"
        "  font-weight: 500;"
        "}"
        "QDoubleSpinBox:hover, QSpinBox:hover {"
        "  border-color: #3e4552;"
        "}"
        "QToolButton#compareFit, QPushButton#compareActionBtn, QPushButton#comparePinBtn {"
        "  background-color: #1e2128;"
        "  color: #e8eaed;"
        "  border: 1px solid #2e333d;"
        "  border-radius: 4px;"
        "  padding: 3px 10px;"
        "  min-height: 22px;"
        "  font-size: 12px;"
        "  font-weight: 500;"
        "}"
        "QToolButton#compareFit:hover, QPushButton#compareActionBtn:hover, QPushButton#comparePinBtn:hover {"
        "  background-color: #262a34;"
        "  border-color: #3e4552;"
        "  color: #ffffff;"
        "}"
        "QToolButton#compareFit:pressed, QPushButton#compareActionBtn:pressed {"
        "  background-color: #2f3440;"
        "}"
        "QPushButton#comparePinBtn:checked {"
        "  background-color: rgba(37, 99, 235, 0.26);"
        "  border-color: #3b82f6;"
        "  color: #ffffff;"
        "  font-weight: 600;"
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
        "  width: 12px;"
        "  height: 12px;"
        "  margin: -4px 0;"
        "  background: #e8eaed;"
        "  border: 1px solid #16181d;"
        "  border-radius: 6px;"
        "}"
        "QSlider::handle:horizontal:hover {"
        "  background: #ffffff;"
        "  border-color: #3b82f6;"
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
    setStyleSheet(compareWindowStyleSheet());
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

void CompareWindow::setPinned(bool pinned) {
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
        pinButton_->setChecked(pinned);
        pinButton_->setText(pinned ? tr("Pinned") : tr("Pin"));
    }
    const bool wasVisible = isVisible();
    togglingPin_ = true;
    setWindowFlag(Qt::WindowStaysOnTopHint, pinned);
    if (wasVisible) {
        show();
        raise();
        activateWindow();
    }
#ifdef Q_OS_WIN
    if (wasVisible && winId() != 0) {
        const HWND hwnd = reinterpret_cast<HWND>(winId());
        if (hwnd != nullptr && IsWindow(hwnd)) {
            SetWindowPos(hwnd, pinned ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
    }
#endif
    togglingPin_ = false;
}

void CompareWindow::resizeEvent(QResizeEvent* event) {
    QDialog::resizeEvent(event);
    if (stack_ != nullptr && canvas_ != nullptr && !stack_->rect().isEmpty() &&
        canvas_->size() != stack_->size()) {
        canvas_->resize(stack_->size());
    }
}

void CompareWindow::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    if (togglingPin_) {
        return;
    }
    if (stack_ != nullptr && canvas_ != nullptr && !stack_->rect().isEmpty() &&
        canvas_->size() != stack_->size()) {
        canvas_->resize(stack_->size());
    }
    if (surface_ != nullptr && surface_->hasVideo()) {
        captureNow();
    }
}

void CompareWindow::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        close();
        event->accept();
        return;
    }
    if (!event->isAutoRepeat() && event->modifiers() == Qt::NoModifier) {
        if (event->key() == Qt::Key_Space && mode_ != CompareMode::SideBySide && canvas_ != nullptr) {
            canvas_->setHeldSide(0);
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_S) {
            onSwapSides();
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_F) {
            if (mode_ == CompareMode::SideBySide) grid_->fitToContent();
            else canvas_->fitToContent();
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_R) {
            captureNow();
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_P) {
            setPinned(!pinned_);
            event->accept();
            return;
        }
    }
    QDialog::keyPressEvent(event);
}

void CompareWindow::keyReleaseEvent(QKeyEvent* event) {
    if (!event->isAutoRepeat() && event->key() == Qt::Key_Space && canvas_ != nullptr &&
        canvas_->heldSide() >= 0) {
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

void CompareWindow::captureNow() {
    QPointer<CompareWindow> self(this);
    ++selectionGeneration_;
    playbackNeedsRecapture_ = false;
    playbackReleasePending_ = false;
    deferredPlanLockRelease_ = false;
    acquirePlaybackHold();
    maintainPlaybackHold();
    if (surface_ != nullptr && !ensurePlanLock()) {
        frame_ = QImage();
        captureError_.clear();
        render();
        if (!self.isNull() && (!isVisible() || !lockRetryArmed_)) {
            requestPlaybackHoldRelease();
        }
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
    if (!isVisible() || (frame_.isNull() && !playbackNeedsRecapture_ && !lockRetryArmed_)) {
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
        lockError_.clear();
        return true;
    }
    lockError_ = reason.isEmpty()
                     ? tr("The native plan cannot be frozen right now.")
                     : reason;
    lockRetryArmed_ = reasonKey == QLatin1String("native.playback.lockBusy");
    return false;
}

void CompareWindow::releasePlanLock() {
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

    // --- Single Integrated Top Toolbar ------------------------------------
    auto* headerFrame = new QFrame(this);
    headerFrame->setObjectName(QStringLiteral("compareHeaderBar"));
    auto* header = new QHBoxLayout(headerFrame);
    header->setContentsMargins(10, 6, 10, 6);
    header->setSpacing(kGapSmall);

    auto* modeTitle = new QLabel(tr("Mode"), headerFrame);
    modeTitle->setObjectName(QStringLiteral("compareFieldLabel"));
    header->addWidget(modeTitle);

    modeBox_ = new QComboBox(headerFrame);
    modeBox_->setObjectName(QStringLiteral("compareMode"));
    modeBox_->setCursor(Qt::PointingHandCursor);
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

    // Inline contextual mode controls (Split / Flicker / Gain) so the viewport
    // never shifts vertically when switching modes.
    modeControls_ = new QWidget(headerFrame);
    modeControls_->setObjectName(QStringLiteral("compareModeBar"));
    auto* view = new QHBoxLayout(modeControls_);
    view->setContentsMargins(4, 0, 4, 0);
    view->setSpacing(kGapSmall);

    splitLabel_ = new QLabel(tr("Split"), modeControls_);
    splitLabel_->setObjectName(QStringLiteral("compareFieldLabel"));
    view->addWidget(splitLabel_);
    split_ = new QDoubleSpinBox(modeControls_);
    split_->setObjectName(QStringLiteral("compareSplit"));
    split_->setRange(0.0, 1.0);
    split_->setSingleStep(0.01);
    split_->setValue(0.5);
    split_->setToolTip(tr("Divider position (double-click divider to center)"));
    connect(split_, &QDoubleSpinBox::valueChanged, this, &CompareWindow::onSplitChanged);
    view->addWidget(split_);

    splitSlider_ = new QSlider(Qt::Horizontal, modeControls_);
    splitSlider_->setRange(0, 1000);
    splitSlider_->setValue(500);
    splitSlider_->setFixedWidth(110);
    splitSlider_->setCursor(Qt::PointingHandCursor);
    splitSlider_->setToolTip(tr("Drag to move split divider"));
    connect(splitSlider_, &QSlider::valueChanged, this, [this](int sliderVal) {
        split_->setValue(sliderVal / 1000.0);
    });
    view->addWidget(splitSlider_);

    flickerLabel_ = new QLabel(tr("Hz"), modeControls_);
    flickerLabel_->setObjectName(QStringLiteral("compareFieldLabel"));
    view->addWidget(flickerLabel_);
    flicker_ = new QSpinBox(modeControls_);
    flicker_->setObjectName(QStringLiteral("compareFlicker"));
    flicker_->setRange(1, 30);
    flicker_->setValue(1);
    flicker_->setToolTip(tr("Alternation frequency (Hz)"));
    connect(flicker_, &QSpinBox::valueChanged, this, &CompareWindow::onFlickerChanged);
    view->addWidget(flicker_);

    flickerSlider_ = new QSlider(Qt::Horizontal, modeControls_);
    flickerSlider_->setRange(1, 30);
    flickerSlider_->setValue(1);
    flickerSlider_->setFixedWidth(100);
    flickerSlider_->setCursor(Qt::PointingHandCursor);
    connect(flickerSlider_, &QSlider::valueChanged, flicker_, &QSpinBox::setValue);
    view->addWidget(flickerSlider_);

    gainLabel_ = new QLabel(tr("Gain"), modeControls_);
    gainLabel_->setObjectName(QStringLiteral("compareFieldLabel"));
    view->addWidget(gainLabel_);
    gain_ = new QSpinBox(modeControls_);
    gain_->setObjectName(QStringLiteral("compareGain"));
    gain_->setRange(kDiffGainMin, kDiffGainMax);
    gain_->setValue(kDiffGainMin);
    gain_->setToolTip(tr("Amplify pixel difference"));
    connect(gain_, &QSpinBox::valueChanged, this, &CompareWindow::onGainChanged);
    view->addWidget(gain_);

    gainSlider_ = new QSlider(Qt::Horizontal, modeControls_);
    gainSlider_->setRange(kDiffGainMin, kDiffGainMax);
    gainSlider_->setValue(kDiffGainMin);
    gainSlider_->setFixedWidth(100);
    gainSlider_->setCursor(Qt::PointingHandCursor);
    connect(gainSlider_, &QSlider::valueChanged, gain_, &QSpinBox::setValue);
    view->addWidget(gainSlider_);

    modeControls_->hide();
    header->addWidget(modeControls_);

    header->addWidget(makeToolbarDivider(headerFrame));

    auto* filterTitle = new QLabel(tr("Scaling"), headerFrame);
    filterTitle->setObjectName(QStringLiteral("compareFieldLabel"));
    header->addWidget(filterTitle);

    filterBox_ = new QComboBox(headerFrame);
    filterBox_->setObjectName(QStringLiteral("compareFilter"));
    filterBox_->setCursor(Qt::PointingHandCursor);
    filterBox_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    filterBox_->addItem(tr("Bilinear"), true);
    filterBox_->addItem(tr("Nearest Neighbor"), false);
    filterBox_->setToolTip(tr("Texture scaling filter"));
    filterBox_->setAccessibleName(tr("Filter"));
    header->addWidget(filterBox_);

    auto* fit = new QToolButton(headerFrame);
    fit->setObjectName(QStringLiteral("compareFit"));
    fit->setCursor(Qt::PointingHandCursor);
    fit->setText(tr("Fit"));
    fit->setToolTip(tr("Reset zoom and fit to window (F)"));
    fit->setAccessibleName(tr("Fit the picture"));
    connect(fit, &QToolButton::clicked, this, [this] {
        if (mode_ == CompareMode::SideBySide) grid_->fitToContent();
        else canvas_->fitToContent();
    });
    header->addWidget(fit);

    zoom_ = new QLabel(tr("%1%").arg(100), headerFrame);
    zoom_->setObjectName(QStringLiteral("compareZoom"));
    zoom_->setFont(uiFont(10));
    zoom_->setAlignment(Qt::AlignCenter);
    zoom_->setToolTip(tr("Current zoom level"));
    zoom_->setAccessibleName(tr("View scale"));
    zoom_->setStyleSheet(
        QStringLiteral("color: #dcdfe4; background: #121419;"
                       " border: 1px solid #282c34;"
                       " border-radius: 4px; padding: 2px 7px;"
                       " font-family: 'Consolas', 'JetBrains Mono', monospace; font-weight: 600;"));
    zoom_->setMinimumWidth(QFontMetrics(zoom_->font()).horizontalAdvance(QStringLiteral("9999%")) + 14);
    header->addWidget(zoom_);

    header->addWidget(makeToolbarDivider(headerFrame));

    swapButton_ = new QPushButton(tr("Swap A ⇄ B"), headerFrame);
    swapButton_->setObjectName(QStringLiteral("compareActionBtn"));
    swapButton_->setCursor(Qt::PointingHandCursor);
    swapButton_->setFocusPolicy(Qt::NoFocus);
    swapButton_->setToolTip(tr("Swap Side A and Side B models (S)"));
    connect(swapButton_, &QPushButton::clicked, this, &CompareWindow::onSwapSides);
    header->addWidget(swapButton_);

    recaptureButton_ = new QPushButton(tr("Recapture"), headerFrame);
    recaptureButton_->setObjectName(QStringLiteral("compareActionBtn"));
    recaptureButton_->setCursor(Qt::PointingHandCursor);
    recaptureButton_->setFocusPolicy(Qt::NoFocus);
    recaptureButton_->setToolTip(tr("Capture the current playback frame again (R)"));
    connect(recaptureButton_, &QPushButton::clicked, this, &CompareWindow::captureNow);
    header->addWidget(recaptureButton_);

    pinButton_ = new QPushButton(tr("Pin"), headerFrame);
    pinButton_->setObjectName(QStringLiteral("comparePinBtn"));
    pinButton_->setCheckable(true);
    pinButton_->setCursor(Qt::PointingHandCursor);
    pinButton_->setFocusPolicy(Qt::NoFocus);
    pinButton_->setToolTip(tr("Keep Compare window always on top of other windows (P)"));
    pinButton_->setAccessibleName(tr("Always on top"));
    connect(pinButton_, &QPushButton::toggled, this, &CompareWindow::setPinned);
    header->addWidget(pinButton_);

    header->addStretch(1);

    hoverSwatch_ = new QLabel(headerFrame);
    hoverSwatch_->setObjectName(QStringLiteral("compareHoverSwatch"));
    hoverSwatch_->setFixedSize(14, 14);
    hoverSwatch_->hide();
    header->addWidget(hoverSwatch_);

    hover_ = new QLabel(headerFrame);
    hover_->setObjectName(QStringLiteral("compareHover"));
    hover_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    hover_->setFont(uiFont(9));
    hover_->setStyleSheet(
        QStringLiteral("color: #c9cdd4; background: transparent;"
                       " font-family: 'Consolas', 'JetBrains Mono', monospace;"));
    header->addWidget(hover_);

    right->addWidget(headerFrame);

    // --- Main Viewport Stack ----------------------------------------------
    canvas_ = new CompareCanvas(this);
    canvas_->setObjectName(QStringLiteral("compareCanvas"));
    canvas_->setMinimumSize(640, 400);
    connect(canvas_, &CompareCanvas::splitDragged, this, [this](double fraction) {
        const QSignalBlocker blocker(split_);
        split_->setValue(fraction);
        if (splitSlider_ != nullptr) {
            const QSignalBlocker sliderBlocker(splitSlider_);
            splitSlider_->setValue(static_cast<int>(std::lround(fraction * 1000.0)));
        }
    });
    connect(canvas_, &CompareCanvas::candidateChosen, this, &CompareWindow::onCandidateChosen);
    connect(canvas_, &CompareCanvas::hoverPixel, this, &CompareWindow::onHoverPixel);
    connect(canvas_, &CompareCanvas::zoomChanged, this, [this](int percent) {
        if (mode_ != CompareMode::SideBySide && zoom_ != nullptr) {
            zoom_->setText(tr("%1%").arg(percent));
        }
    });
    canvas_->setStyleSheet(
        QStringLiteral("QGraphicsView#compareCanvas { background: #111317; border: 1px solid #282c34;"
                       " border-radius: 6px; }"));
    stack_ = new QStackedWidget(this);
    stack_->addWidget(canvas_);

    grid_ = new CompareGrid(this);
    stack_->addWidget(grid_);
    connect(grid_, &CompareGrid::choiceChosen, this, &CompareWindow::onCandidateChosen);
    connect(grid_, &CompareGrid::hoverPixel, this, &CompareWindow::onHoverPixel);
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
    info_->setStyleSheet(
        QStringLiteral("color: #8b929e; background: transparent; padding: 0 4px;"));
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
    const QString shadersDir = anime4k::findShadersDir();
    const native::Backend ajiBackend = ajiBackend_;
    candidates_.clear();
    candidates_.reserve(presets.size() + ajiPresets.size());
    choices_.clear();
    choices_.reserve(presets.size() + ajiPresets.size());
    ajiBackends_.clear();
    ajiErrors_.clear();
    for (const anime4k::ModeInfo& preset : presets) {
        const anime4k::Candidate candidate = anime4k::candidateForId(preset.id, shadersDir);
        const bool complete = shadersDir.isEmpty() || anime4k::chainIsComplete(candidate, shadersDir);
        ChoiceEntry entry;
        entry.id = preset.id;
        entry.label = preset.label;
        entry.path = categoryPath(preset.category);
        if (complete) {
            candidates_.append(preset.id);
        } else {
            entry.disabled = true;
            QStringList missingFiles;
            for (const QString& file : candidate.files) {
                if (!QFileInfo(QDir(shadersDir).filePath(file)).exists()) {
                    missingFiles.append(QFileInfo(file).fileName());
                }
            }
            entry.tooltip = missingFiles.isEmpty()
                                ? tr("Missing shader file")
                                : tr("Missing shader file(s): %1")
                                      .arg(missingFiles.join(QStringLiteral(", ")));
        }
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
    const qsizetype current =
        std::max<qsizetype>(0, candidates_.indexOf(currentCandidateId_));
    selection_.append(candidates_.value(current));
    const qsizetype other = current == 0 ? std::min<qsizetype>(1, candidates_.size() - 1) : 0;
    if (other != current) {
        selection_.append(candidates_.at(other));
    }
    if (canvas_ != nullptr) {
        canvas_->setSideNames(labelFor(sideA()), labelFor(sideB()));
        canvas_->setChoices(choices_, sideA(), sideB());
    }
    if (grid_ != nullptr) {
        grid_->setChoices(choices_);
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
    if (!playbackHoldActive_ || surface_ == nullptr ||
        surface_->sourceEpoch() != playbackHoldSourceEpoch_) {
        return false;
    }
    const quint64 requestGeneration = surface_->pauseRequestGeneration();
    if (requestGeneration != playbackHoldGeneration_ &&
        !surface_->isPlaying() && surface_->pauseObserved()) {
        playbackHoldOriginalPlaying_ = false;
        playbackHoldGeneration_ = requestGeneration;
    }
    return surface_->pauseRequestGeneration() == playbackHoldGeneration_ &&
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
    const bool canRestorePlaying =
        playbackHoldOwned() ||
        (playbackHoldActive_ && surface_ != nullptr &&
         surface_->sourceEpoch() == playbackHoldSourceEpoch_ &&
         surface_->pauseRequestGeneration() == playbackHoldGeneration_);
    if (playbackReleasePending_ && canRestorePlaying &&
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
    if (captureFrameActive_) {
        surface_->reassertPaused();
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
    std::optional<double> timestampBefore = readPlaybackTimestamp();
    if (self.isNull()) {
        return;
    }
    if (!timestampBefore.has_value()) {
        captureError_ = tr("Could not capture the playback timestamp");
        return;
    }
    QImage grabbed = grabSettledFrame();
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
    std::optional<double> playbackTimestamp = readPlaybackTimestamp();
    if (self.isNull()) {
        return;
    }
    // When pausing active playback, the final in-flight decoder frame may finish
    // presenting at the start of grabSettledFrame(). If the timestamp moved once
    // while settling, re-verify on the now-settled frame.
    if (playbackTimestamp.has_value() && playbackHoldOwned() &&
        std::abs(*playbackTimestamp - *timestampBefore) > 1e-6 &&
        surface_ != nullptr && surface_->sourceEpoch() == sourceEpoch) {
        timestampBefore = playbackTimestamp;
        grabbed = grabSettledFrame();
        if (self.isNull()) {
            return;
        }
        if (!grabbed.isNull() && surface_ != nullptr &&
            surface_->sourceEpoch() == sourceEpoch) {
            playbackTimestamp = readPlaybackTimestamp();
            if (self.isNull()) {
                return;
            }
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
    if (id == QLatin1String("off") && !isolatedOriginal_.isNull()) {
        pendingAjiId_.clear();
        return isolatedOriginal_;
    }
    const quint64 generation = activeRenderGeneration_;
    const bool startedVisible = isVisible();
    const auto cancelled = [self, generation, startedVisible] {
        return self == nullptr || (startedVisible && !self->isVisible()) ||
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
        const QImage rawOff = renderer->render(QStringLiteral("off"), {}, cancelled, progress);
        if (self == nullptr) return {};
        if (cancelled()) {
            finishIsolatedRetry();
            return {};
        }
        if (rawOff.isNull()) {
            pendingAjiId_.clear();
            ajiErrors_.insert(id, renderer->error());
            abandonIsolatedRenderer();
            return {};
        }
        if (isolatedOriginal_.isNull()) {
            isolatedOriginal_ = rawOff;
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
    canvas_->setSideNames(labelFor(sideA()), labelFor(sideB()));
    canvas_->setChoices(choices_, sideA(), sideB());
    if (!frame_.isNull() && surface_ == nullptr) {
        frame_ = QImage();
        captureError_ = tr("The playback surface was destroyed");
    } else if (!frame_.isNull() && surface_->sourceEpoch() != frozenSourceEpoch_) {
        frame_ = QImage();
        captureError_ = tr("The source changed after the frame was captured");
    }
    if (frame_.isNull()) {
        renderedA_ = QImage();
        renderedB_ = QImage();
        stack_->setCurrentIndex(0);
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
        stack_->setCurrentIndex(0);
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
        stack_->setCurrentIndex(0);
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
        difference = gain_->value() > 1 ? differenceGrayscale(a, b, gain_->value())
                                        : differenceJet(a, b);
    }
    stack_->setCurrentIndex(mode_ == CompareMode::SideBySide ? 1 : 0);
    if (stack_ != nullptr && !stack_->rect().isEmpty() && canvas_->size() != stack_->size()) {
        canvas_->resize(stack_->size());
    }
    canvas_->setSideNames(labelFor(sideA()), labelFor(sideB()));
    canvas_->setChoices(choices_, sideA(), sideB());
    canvas_->setMode(mode_);
    canvas_->setSideImages(a, b, difference);
    if (mode_ == CompareMode::SideBySide) {
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
    const bool isSplit = mode == CompareMode::SplitH || mode == CompareMode::SplitV;
    const bool isFlicker = mode == CompareMode::Flicker;
    const bool isDifference = mode == CompareMode::Difference;
    splitLabel_->setVisible(isSplit);
    split_->setVisible(isSplit);
    if (splitSlider_ != nullptr) splitSlider_->setVisible(isSplit);
    flickerLabel_->setVisible(isFlicker);
    flicker_->setVisible(isFlicker);
    if (flickerSlider_ != nullptr) flickerSlider_->setVisible(isFlicker);
    gainLabel_->setVisible(isDifference);
    gain_->setVisible(isDifference);
    if (gainSlider_ != nullptr) gainSlider_->setVisible(isDifference);
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
    if (splitSlider_ != nullptr) {
        const QSignalBlocker blocker(splitSlider_);
        splitSlider_->setValue(static_cast<int>(std::lround(value * 1000.0)));
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

void CompareWindow::onSwapSides() {
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

void CompareWindow::onCandidateChosen(int side, const QString& id) {
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
    if (side == 0 && (renderedAId_ != id ||
                      (ajiBackends_.contains(id) && renderedBackend_ != ajiBackend_))) {
        renderedA_ = {};
        renderedAId_.clear();
    }
    if (side == 1 && (renderedBId_ != id ||
                      (ajiBackends_.contains(id) && renderedBackend_ != ajiBackend_))) {
        renderedB_ = {};
        renderedBId_.clear();
    }
    canvas_->setSideNames(labelFor(sideA()), labelFor(sideB()));
    canvas_->setChoices(choices_, sideA(), sideB());
    canvas_->setSideImages(renderedA_, renderedB_, {});
    if (mode_ == CompareMode::SideBySide) {
        grid_->setChoices(choices_);
        grid_->setItems({sideA(), sideB()}, {renderedA_, renderedB_},
                        {labelFor(sideA()), labelFor(sideB())});
    }
    render();
}

void CompareWindow::dismissCandidateMenus() {
    for (QMenu* menu : findChildren<QMenu*>()) {
        menu->hide();
        menu->close();
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
    hover_->setText(tr("%1, %2  %3 (%4,%5,%6)")
                        .arg(x)
                        .arg(y)
                        .arg(color.name().toUpper())
                        .arg(color.red())
                        .arg(color.green())
                        .arg(color.blue()));
    hoverSwatch_->setStyleSheet(
        QStringLiteral("background: %1; border: 1px solid #484f5e; border-radius: 3px;")
            .arg(color.name()));
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
