#include "CompareWindow.hpp"

#include "Anime4K.hpp"
#include "CompareCanvas.hpp"
#include "CompareGrid.hpp"
#include "ShellTheme.hpp"
#include "VideoSurface.hpp"
#include "native/NativePlaybackController.hpp"

#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QShowEvent>
#include <QSpinBox>
#include <QStackedWidget>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
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
/// How long a frame may sit still before the picture counts as settled. The
/// surface reports its position from a 50 ms pump, so anything tighter would
/// be measuring the pump rather than the video.
constexpr double kSettleQuietSeconds = 0.08;
/// Consecutive identical grabs before a frame is believed. One grab can be the
/// picture from before the change that was just asked for.
constexpr int kSettleRounds = 2;
/// Consecutive rounds with no position update from the surface before its
/// framebuffer is believed at all. The surface pumps its position every 50 ms,
/// so a capture that accepted a frame immediately could read the picture of a
/// seek that has not been reported yet. Five rounds is 200 ms, four pump
/// intervals, and it restarts whenever a late update does arrive.
constexpr int kMinObserveRounds = 5;
/// How long a capture waits for the picture to settle before giving up and
/// saying so.
constexpr int kSettleTimeoutMs = 3000;
/// Poll spacing while waiting. Fine enough that kSettleRounds consecutive grabs
/// span a real change, coarse enough not to starve the event loop.
constexpr int kPollIntervalMs = 40;

QString caption(const QString& text) {
    return QStringLiteral(" %1 ").arg(text);
}

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

CompareWindow::CompareWindow(VideoSurface* surface, const QString& currentCandidateId,
                             QWidget* parent)
    : QDialog(parent), surface_(surface), currentCandidateId_(currentCandidateId) {
    setWindowTitle(tr("Quality Comparison"));
    setModal(false);
    resize(kWindowWidth, kWindowHeight);
    buildUi();
    populateCandidates();
    // No capture here. The window is constructed before it is shown, and the
    // surface's framebuffer is not something to read before the widget stack
    // has been mapped. The first showEvent() takes the frame instead, so
    // construction and reopening follow exactly the same path.
    //
    // The position epoch counts the surface's own updates, which is the only
    // signal that a move is still being reported. A capture waits for it to go
    // quiet before trusting the framebuffer, because the surface reports its
    // position from a 50 ms pump: a rewind clicked just before a capture would
    // otherwise be read as the position the viewer started from.
    if (surface_ != nullptr) {
        connect(surface_, &VideoSurface::positionChanged, this,
                [this](double) { ++positionEpoch_; });
        // Carries the queue depth and the end of a settling transition, so the
        // armed retry never freezes a stale epoch.
        connect(surface_, &VideoSurface::nativePlaybackStatusChanged, this,
                &CompareWindow::onNativeStatusSettled);
    }
    render();
}

CompareWindow::~CompareWindow() { releasePlanLock(); }

void CompareWindow::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    // The player keeps one window and re-shows it, so showing it again is the
    // viewer asking to compare where the player is NOW. Without this the window
    // would raise itself onto the frame it froze the first time it was opened,
    // which is the whole of the "my rewind did nothing" complaint.
    if (surface_ != nullptr && surface_->hasVideo()) {
        captureNow();
    }
}

void CompareWindow::closeEvent(QCloseEvent* event) {
    // The window is reused, so the lock is released here and re-taken in
    // showEvent; release is also what applies a queued native request.
    releasePlanLock();
    QDialog::closeEvent(event);
}

void CompareWindow::hideEvent(QHideEvent* event) {
    releasePlanLock();
    QDialog::hideEvent(event);
}

void CompareWindow::captureNow() {
    // Every capture path holds the plan lock, so no freeze is ever taken outside it.
    if (surface_ != nullptr && !ensurePlanLock()) {
        frame_ = QImage();
        captureError_.clear();
        render();
        return;
    }
    captureFrame();
    render();
    // The lock's lifetime is the visible window: a programmatic capture on a
    // hidden window must not leave the plan held.
    if (!isVisible()) {
        releasePlanLock();
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

    // No candidate sidebar: both sides are picked from the badges on the
    // pictures themselves, so the list of every chain in the catalogue is a
    // second copy of a choice the picture already offers.
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
    hover_ = new QLabel(this);
    hover_->setObjectName(QStringLiteral("compareHover"));
    header->addWidget(hover_, 1);

    auto* close = new QPushButton(tr("Close"), this);
    close->setObjectName(QStringLiteral("compareClose"));
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    header->addWidget(close);
    right->addLayout(header);

    // --- view controls: one per mode that has a value --------------------
    auto* view = new QHBoxLayout();
    view->setSpacing(kGapSmall);

    splitLabel_ = new QLabel(tr("Split:"), this);
    view->addWidget(splitLabel_);
    split_ = new QDoubleSpinBox(this);
    split_->setObjectName(QStringLiteral("compareSplit"));
    split_->setRange(0.0, 1.0);
    split_->setSingleStep(0.01);
    split_->setValue(0.5);
    connect(split_, &QDoubleSpinBox::valueChanged, this, &CompareWindow::onSplitChanged);
    view->addWidget(split_);

    flickerLabel_ = new QLabel(tr("Flicker, Hz:"), this);
    view->addWidget(flickerLabel_);
    flicker_ = new QSpinBox(this);
    flicker_->setObjectName(QStringLiteral("compareFlicker"));
    flicker_->setRange(1, 30);
    flicker_->setValue(1);
    connect(flicker_, &QSpinBox::valueChanged, this, &CompareWindow::onFlickerChanged);
    view->addWidget(flicker_);

    gainLabel_ = new QLabel(tr("Gain:"), this);
    view->addWidget(gainLabel_);
    gain_ = new QSpinBox(this);
    gain_->setObjectName(QStringLiteral("compareGain"));
    gain_->setRange(kDiffGainMin, kDiffGainMax);
    gain_->setValue(kDiffGainMin);
    connect(gain_, &QSpinBox::valueChanged, this, &CompareWindow::onGainChanged);
    view->addWidget(gain_);

    auto* fit = new QToolButton(this);
    fit->setObjectName(QStringLiteral("compareFit"));
    fit->setText(tr("Fit"));
    connect(fit, &QToolButton::clicked, this, [this] { canvas_->fitToContent(); });
    view->addWidget(fit);

    right->addLayout(view);

    canvas_ = new CompareCanvas(this);
    canvas_->setObjectName(QStringLiteral("compareCanvas"));
    canvas_->setMinimumSize(640, 400);
    connect(canvas_, &CompareCanvas::splitDragged, this, [this](double fraction) {
        const QSignalBlocker blocker(split_);
        split_->setValue(fraction);
    });
    connect(canvas_, &CompareCanvas::candidateChosen, this, &CompareWindow::onCandidateChosen);
    connect(canvas_, &CompareCanvas::hoverPixel, this, &CompareWindow::onHoverPixel);
    canvas_->setStyleSheet(
        QStringLiteral("QGraphicsView#compareCanvas { background: %1; border: 1px solid %2;"
                       " border-radius: %3px; }")
            .arg(bg0().name())
            .arg(white(20).name())
            .arg(kGap));
    stack_ = new QStackedWidget(this);
    stack_->addWidget(canvas_);
    // Side-by-side is the multi-pane surface: one pane per side, each with its
    // own corner badge and candidate menu. The other modes render on the single
    // canvas, exactly like the reference's stacked surfaces.
    grid_ = new CompareGrid(this);
    stack_->addWidget(grid_);
    connect(grid_, &CompareGrid::choiceChosen, this, &CompareWindow::onCandidateChosen);
    right->addWidget(stack_, 1);

    info_ = new QLabel(this);
    info_->setObjectName(QStringLiteral("compareInfo"));
    info_->setWordWrap(true);
    info_->setFixedHeight(kCompareInfoHeight);
    info_->setFont(uiFont(9));
    info_->setStyleSheet(QStringLiteral("color: %1; background: transparent;").arg(w55().name()));
    right->addWidget(info_);

    root->addLayout(right, 1);

    flickerTimer_ = new QTimer(this);
    connect(flickerTimer_, &QTimer::timeout, this, &CompareWindow::onFlickerTimeout);
}

void CompareWindow::populateCandidates() {
    const QVector<anime4k::ModeInfo> presets = anime4k::availableModes();
    candidates_.clear();
    candidates_.reserve(presets.size());
    choices_.clear();
    choices_.reserve(presets.size());
    for (const anime4k::ModeInfo& preset : presets) {
        candidates_.append(preset.id);
        ChoiceEntry entry;
        entry.id = preset.id;
        entry.label = preset.label;
        entry.path = categoryPath(preset.category);
        choices_.append(entry);
    }
    selection_.clear();
    if (candidates_.isEmpty()) {
        return;
    }
    // Two sides, always: the chain the player is running opens as A and another
    // player-menu preset as B, so the window opens on a comparison rather than
    // on an empty pane. A catalogue of one compares a chain with itself.
    const qsizetype current =
        std::max<qsizetype>(0, candidates_.indexOf(currentCandidateId_));
    selection_.append(candidates_.value(current));
    const qsizetype other = current == 0 ? std::min<qsizetype>(1, candidates_.size() - 1) : 0;
    if (other != current) {
        selection_.append(candidates_.at(other));
    }
}

QString CompareWindow::sideA() const { return selection_.value(0); }

QString CompareWindow::sideB() const { return selection_.value(1); }

QImage CompareWindow::grabSettledFrame(double* settledPosition) {
    *settledPosition = 0.0;
    if (surface_ == nullptr) {
        return QImage();
    }
    // A frame is believed only when the framebuffer has stopped changing AND
    // the position has stopped changing, in the SAME consecutive rounds.
    //
    // Either signal alone lies, and this is the whole bug. The position comes
    // off a 50 ms pump, so a core that has just been rewound reports its new
    // position a frame or two BEFORE the framebuffer is repainted - and a
    // framebuffer that has not been repainted still holds the previous frame.
    // Requiring several consecutive rounds with both quiet, at a spacing wider
    // than the pump, is what makes this the frame at the current position
    // rather than the frame the viewer just left.
    QImage previous;
    double previousPosition = 0.0;
    int stableRounds = 0;
    quint64 epoch = positionEpoch_;
    int quietEpochRounds = 0;
    for (int waited = 0; waited < kSettleTimeoutMs; waited += kPollIntervalMs) {
        QCoreApplication::processEvents();
        QThread::msleep(kPollIntervalMs);
        // The event loop above can deliver a close that destroys the borrowed
        // surface; the QPointer turns that into a refusal here instead of a
        // read of freed memory.
        if (surface_ == nullptr) {
            return QImage();
        }
        const QImage probe = surface_->grabFramebuffer();
        const double now = surface_->position();
        // Phase one, observed first and on its own: has the surface stopped
        // reporting a move? A seek issued moments ago has not been pumped yet,
        // so the position on offer in the first rounds is still the one the
        // viewer is leaving. kMinObserveRounds is deliberately several pump
        // intervals, so a seek that was already in flight when the capture
        // started cannot slip through unobserved - and unlike a wall-clock
        // deadline it restarts when a late update does arrive.
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
        // Phase two, only after phase one has actually observed a settled
        // surface: the picture has to hold still for a run of rounds by now
        // that the position it belongs to has stopped moving. This is where a
        // repaint that landed after the seek gets read.
        if (quietEpochRounds >= kMinObserveRounds && stableRounds >= kSettleRounds) {
            *settledPosition = now;
            return previous;
        }
    }
    return QImage();
}

void CompareWindow::captureFrame() {
    // A capture is a READING of the player, not a command to it. The old
    // version seeked to the position it was already at, which re-seeked live
    // playback, and then accepted the framebuffer as soon as the reported
    // position came within tolerance - a tolerance the cached position reaches
    // before the framebuffer has been repainted, so the "fresh" frame it
    // stored was the one the viewer had just rewound away from. When the
    // tolerance was never reached it stored a bare grab instead, which is the
    // stale picture reading as a real comparison.
    //
    // So: read where the player actually is, freeze the picture it is actually
    // showing, and if that picture cannot be read, say so on screen.
    frame_ = QImage();
    captureError_.clear();
    if (surface_ == nullptr) {
        captureError_ = tr("No playback surface");
        return;
    }
    if (!surface_->hasVideo()) {
        captureError_ = tr("No video to compare");
        return;
    }
    // The pause below is a temporary override that buys a settled grab. Only a
    // picture earns it: a capture that cannot read one has frozen nothing, so
    // the play state it was found in is what it has to be left with.
    const bool wasPlaying = surface_->isPlaying();
    if (wasPlaying) {
        surface_->setPaused(true);
    }
    // The native plan is held by the real scoped lock (ensurePlanLock), not by
    // stopping the status reader: pausing polling is not locking, and the reader
    // stays live so a transition already in flight is still acknowledged. Only
    // the Anime4K shader chain is mutated here; native identity is untouched.
    double settled = 0.0;
    const QImage grabbed = grabSettledFrame(&settled);
    if (grabbed.isNull()) {
        captureError_ = tr("Could not capture a new frame");
        // Nothing was frozen, so the pause that bought the grab is put back.
        // The guard is the same QPointer one the grab loop uses: the event
        // loop above can have destroyed the borrowed surface mid-wait.
        if (wasPlaying && surface_ != nullptr) {
            surface_->setPaused(false);
        }
        return;
    }
    // Both sides render from this one image, so the difference between them is
    // the chain and nothing else.
    frame_ = grabbed;
}

QImage CompareWindow::renderWithChain(const QString& id) {
    if (surface_ == nullptr || frame_.isNull()) {
        return QImage();
    }
    surface_->setAnime4KChain(id);
    // A chain is applied when mpv builds its filter, and a paused video
    // produces no frames to build it from, so the surface is asked to render
    // and then given a few frames to settle. Two settled grabs, not one: the
    // first can still be the pre-chain image.
    //
    // The surface is parked on the frozen frame's position, so what comes back
    // here is that same source with the chain on - the only thing that differs
    // between the two sides.
    surface_->update();
    QImage last;
    for (int waited = 0; waited < kSettleTimeoutMs; waited += kPollIntervalMs) {
        QCoreApplication::processEvents();
        QThread::msleep(kPollIntervalMs);
        // Same guard as the capture loop: the pump can destroy the borrowed
        // surface mid-render.
        if (surface_ == nullptr) {
            return QImage();
        }
        surface_->update();
        QCoreApplication::processEvents();
        const QImage probe = surface_->grabFramebuffer();
        if (probe.isNull()) {
            continue;
        }
        if (!last.isNull() && probe == last) {
            return probe;
        }
        last = probe;
    }
    // Never the last unsettled grab. A side that has not settled is not a
    // picture of that chain, and handing one over would put a half-built
    // filter into a difference image and call it a result.
    return QImage();
}

void CompareWindow::render() {
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
        grid_->setItems({}, {}, {});
        updateInfo();
        return;
    }
    // The live chain is read before both sides and put back after: Compare must
    // not leave the player running a preview chain.
    const QString liveChain = surface_ != nullptr ? surface_->anime4KChainId() : QString();
    const QImage a = renderWithChain(sideA());
    const QImage b = renderWithChain(sideB());
    if (surface_ != nullptr) {
        if (liveChain.isEmpty()) {
            // No id to put back: fall back to the mode, which clears anything
            // the comparison left on the surface.
            surface_->setAnime4KMode(surface_->anime4KMode());
        } else if (surface_->anime4KChainId() != liveChain) {
            surface_->setAnime4KChain(liveChain);
        }
    }
    renderedA_ = a;
    renderedB_ = b;
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
    canvas_->setSideImages(a.isNull() ? frame_ : a, b.isNull() ? frame_ : b, difference);
    stack_->setCurrentIndex(mode_ == CompareMode::SideBySide ? 1 : 0);
    if (mode_ == CompareMode::SideBySide) {
        grid_->setChoices(choices_);
        grid_->setItems({sideA(), sideB()},
                        {a.isNull() ? frame_ : a, b.isNull() ? frame_ : b},
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
    if (!candidates_.contains(id)) {
        return;
    }
    // The badge menus are the only chooser, so this is the whole selection: A
    // first, B second. Picking the same chain on both sides is allowed - two
    // frames of one chain is a legitimate way to look at it.
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
    render();
}

void CompareWindow::onHoverPixel(int x, int y, bool valid) {
    hover_->setText(valid ? QStringLiteral("%1, %2").arg(x).arg(y) : QString());
}

void CompareWindow::updateInfo() {
    const QString a = sideA();
    const QString b = sideB();
    const anime4k::Candidate infoA = anime4k::candidateForId(a);
    const anime4k::Candidate infoB = anime4k::candidateForId(b);
    const QString shadersDir = anime4k::findShadersDir();
    QStringList parts;
    const auto describe = [&parts, &shadersDir](const QString& tag, const QString& id,
                                                const anime4k::Candidate& candidate) {
        if (id.isEmpty()) {
            return;
        }
        QString line = caption(tag) + QStringLiteral(" %1").arg(candidate.label);
        line += QStringLiteral(" · ") +
                tr("%n shader", nullptr, static_cast<int>(candidate.files.size()));
        if (candidate.scale > 0) {
            line += QStringLiteral(" · ×%1").arg(candidate.scale);
        }
        if (!anime4k::chainIsComplete(candidate, shadersDir)) {
            line += QStringLiteral(" · ") + tr("FILE MISSING");
        }
        parts.append(line);
    };
    describe(QStringLiteral("A"), a, infoA);
    describe(QStringLiteral("B"), b, infoB);
    // A plan that could not be frozen outranks everything: with no held epoch no
    // comparison of a native frame is claimed at all.
    if (!lockError_.isEmpty()) {
        info_->setText(tr("Native plan not frozen: %1").arg(lockError_));
        return;
    }
    // A failed capture leads the line. Whatever the two sides are, the
    // comparison itself did not happen, and that outranks naming them.
    if (!captureError_.isEmpty()) {
        info_->setText(tr("Capture error: %1").arg(captureError_));
        return;
    }
    // The base is named from the filter's own effective plan, never the request;
    // a requested-but-not-effective chain is labelled as the original frame.
    if (surface_ != nullptr) {
        const native::NativePlaybackController::Status& native =
            surface_->nativePlaybackStatus();
        if (native.effective.has_value()) {
            const native::NativeEffectivePlan& plan = *native.effective;
            QString line = tr("Native output base");
            if (!plan.recipe.isEmpty()) {
                line += QStringLiteral(" · ") + tr("recipe %1").arg(plan.recipe);
            }
            QStringList identity;
            if (!plan.provider.isEmpty()) identity.append(plan.provider);
            if (!plan.model.isEmpty()) identity.append(plan.model);
            if (!identity.isEmpty()) {
                line += QStringLiteral(" · ") + identity.join(QLatin1Char(' '));
            }
            if (plan.interpolation.has_value() && plan.interpolation->factor.isValid()) {
                line += QStringLiteral(" · ") +
                        tr("RIFE %1").arg(plan.interpolation->factor.toString());
                if (!plan.order.isEmpty()) {
                    line += QLatin1Char(' ') + plan.order;
                }
            }
            parts.prepend(line);
        } else if (native.state != native::NativePlaybackController::State::Off &&
                   native.state != native::NativePlaybackController::State::Idle) {
            // The queued case is named by this translated line plus the
            // translated count below, never by the controller's untranslated C
            // sentence; a real refusal keeps the actual reason in the wrapper.
            const bool queueState =
                native.reasonKey == QLatin1String("native.playback.planLocked") ||
                native.reasonKey == QLatin1String("native.playback.planLockRelease") ||
                queuedNativeChanges() > 0;
            parts.prepend(queueState
                              ? tr("Native requested · base is the original frame")
                              : tr("Native requested · base is the original frame — %1")
                                    .arg(native.reason));
        }
    }
    if (queuedNativeChanges() > 0) {
        parts.append(tr("%n native change(s) queued until Compare closes", nullptr,
                        queuedNativeChanges()));
    }
    if (parts.isEmpty()) {
        info_->setText(tr("Select at least one variant to compare."));
        return;
    }
    // The gate the shaders carry. Every upscaler is prefixed
    // `//!WHEN OUTPUT.w MAIN.w / 1.200 > ...`, so a chain declines to run at
    // all when mpv is already displaying the video at or above source size. On
    // a 1080p source in a 1080p window an x4 chain can therefore change
    // nothing at all, and saying so is the difference between "this looks
    // different" and "this cannot do anything here".
    const int sourceWidth =
        surface_ != nullptr && surface_->videoWidth() > 0 ? surface_->videoWidth() : frame_.width();
    const int sourceHeight = surface_ != nullptr && surface_->videoHeight() > 0
                                 ? surface_->videoHeight()
                                 : frame_.height();
    if (sourceWidth > 0 && sourceHeight > 0) {
        const double outputWidth = static_cast<double>(canvas_->imageWidth());
        if (outputWidth / 1.200 < sourceWidth) {
            parts.append(tr("window is narrower than the source - upscaling will not run"));
        }
    }
    if (mode_ == CompareMode::Difference && !renderedA_.isNull() && !renderedB_.isNull() &&
        renderedA_.size() == renderedB_.size()) {
        parts.append(tr("mean difference %1/255")
                         .arg(QString::number(meanAbsoluteDifference(renderedA_, renderedB_),
                                              'f', 2)));
    }
    info_->setText(parts.join(QStringLiteral("  ·  ")));
}

QString CompareWindow::labelFor(const QString& id) const {
    const anime4k::Candidate candidate = anime4k::candidateForId(id);
    return candidate.label.isEmpty() ? QStringLiteral("—") : candidate.label;
}

}  // namespace shell
}  // namespace mplayer
