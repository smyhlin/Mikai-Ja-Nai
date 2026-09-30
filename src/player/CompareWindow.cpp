#include "CompareWindow.hpp"

#include "Anime4K.hpp"
#include "CompareCanvas.hpp"
#include "CompareGrid.hpp"
#include "ShellTheme.hpp"
#include "VideoSurface.hpp"

#include <QComboBox>
#include <QCoreApplication>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QShowEvent>
#include <QSlider>
#include <QSpinBox>
#include <QStackedWidget>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace mplayer {
namespace shell {

using mplayer::VideoSurface;

namespace {

constexpr int kGap = 12;
constexpr int kGapSmall = 8;
constexpr int kWindowWidth = 1280;
constexpr int kWindowHeight = 800;
constexpr int kInfoHeight = 44;
constexpr int kTransportWidth = 160;
/// One frame at a typical anime rate, in seconds. The surface exposes no frame
/// index, so stepping by time is the honest equivalent.
constexpr double kFrameStep = 1.0 / 24.0;
/// How close the surface's reported position must land to the requested one
/// before the grabbed frame counts as the right one.
constexpr double kSeekTolerance = 0.12;
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
/// How long a seek is given to land before the capture is called a failure.
constexpr int kSeekTimeoutMs = 3000;
/// Poll spacing while waiting. Fine enough that kSettleRounds consecutive grabs
/// span a real change, coarse enough not to starve the event loop.
constexpr int kPollIntervalMs = 40;

QString caption(const QString& text) {
    return QStringLiteral(" %1 ").arg(text);
}

}  // namespace

CompareWindow::CompareWindow(VideoSurface* surface, const QString& currentCandidateId,
                             QWidget* parent)
    : QDialog(parent), surface_(surface), currentCandidateId_(currentCandidateId) {
    setWindowTitle(QStringLiteral("Порівняння якості"));
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
    }
    render();
}

CompareWindow::~CompareWindow() = default;

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

void CompareWindow::captureNow() {
    captureFrame();
    render();
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
    header->addWidget(new QLabel(QStringLiteral("Режим:"), this));
    modeBox_ = new QComboBox(this);
    modeBox_->setObjectName(QStringLiteral("compareMode"));
    for (const CompareMode mode : {CompareMode::Original, CompareMode::Processed,
                                   CompareMode::SplitH, CompareMode::SplitV,
                                   CompareMode::SideBySide, CompareMode::Overlay,
                                   CompareMode::Flicker, CompareMode::Difference}) {
        modeBox_->addItem(QString::fromLatin1(modeLabel(mode)),
                          QString::fromLatin1(modeKey(mode)));
    }
    const int sideBySide = modeBox_->findData(QString::fromLatin1(modeKey(CompareMode::SideBySide)));
    if (sideBySide >= 0) {
        modeBox_->setCurrentIndex(sideBySide);
    }
    connect(modeBox_, qOverload<int>(&QComboBox::currentIndexChanged), this,
            &CompareWindow::onModeChanged);
    header->addWidget(modeBox_);
    header->addStretch(1);
    right->addLayout(header);

    // --- view controls: one per mode that has a value --------------------
    auto* view = new QHBoxLayout();
    view->setSpacing(kGapSmall);

    splitLabel_ = new QLabel(QStringLiteral("Розділ:"), this);
    view->addWidget(splitLabel_);
    split_ = new QDoubleSpinBox(this);
    split_->setObjectName(QStringLiteral("compareSplit"));
    split_->setRange(0.0, 1.0);
    split_->setSingleStep(0.01);
    split_->setValue(0.5);
    connect(split_, &QDoubleSpinBox::valueChanged, this, &CompareWindow::onSplitChanged);
    view->addWidget(split_);

    opacityLabel_ = new QLabel(QStringLiteral("Накладання:"), this);
    view->addWidget(opacityLabel_);
    opacity_ = new QSlider(Qt::Horizontal, this);
    opacity_->setObjectName(QStringLiteral("compareOpacity"));
    opacity_->setRange(0, 100);
    opacity_->setValue(50);
    opacity_->setFixedWidth(110);
    connect(opacity_, &QSlider::valueChanged, this, &CompareWindow::onOpacityChanged);
    view->addWidget(opacity_);

    flickerLabel_ = new QLabel(QStringLiteral("Мигання, Гц:"), this);
    view->addWidget(flickerLabel_);
    flicker_ = new QSpinBox(this);
    flicker_->setObjectName(QStringLiteral("compareFlicker"));
    flicker_->setRange(1, 30);
    flicker_->setValue(1);
    connect(flicker_, &QSpinBox::valueChanged, this, &CompareWindow::onFlickerChanged);
    view->addWidget(flicker_);

    gainLabel_ = new QLabel(QStringLiteral("Підсилення:"), this);
    view->addWidget(gainLabel_);
    gain_ = new QSpinBox(this);
    gain_->setObjectName(QStringLiteral("compareGain"));
    gain_->setRange(kDiffGainMin, kDiffGainMax);
    gain_->setValue(kDiffGainMin);
    connect(gain_, &QSpinBox::valueChanged, this, &CompareWindow::onGainChanged);
    view->addWidget(gain_);

    // The wheel does not zoom, so this is the only way back to the whole frame
    // after a drag has walked off it.
    auto* fit = new QToolButton(this);
    fit->setObjectName(QStringLiteral("compareFit"));
    fit->setText(QStringLiteral("Вписати"));
    connect(fit, &QToolButton::clicked, this, [this] { canvas_->fitToContent(); });
    view->addWidget(fit);

    right->addLayout(view);

    // --- transport --------------------------------------------------------
    auto* transport = new QHBoxLayout();
    transport->setSpacing(kGapSmall);
    play_ = new QToolButton(this);
    play_->setObjectName(QStringLiteral("comparePlay"));
    play_->setText(QStringLiteral("Грати"));
    connect(play_, &QToolButton::clicked, this, &CompareWindow::onPlayPause);
    transport->addWidget(play_);

    auto* back = new QToolButton(this);
    back->setObjectName(QStringLiteral("compareStepBack"));
    back->setText(QStringLiteral("< кадр"));
    connect(back, &QToolButton::clicked, this, &CompareWindow::onStepBack);
    transport->addWidget(back);

    auto* forward = new QToolButton(this);
    forward->setObjectName(QStringLiteral("compareStepForward"));
    forward->setText(QStringLiteral("кадр >"));
    connect(forward, &QToolButton::clicked, this, &CompareWindow::onStepForward);
    transport->addWidget(forward);

    // Re-grab the frame where playback stands right now, without stepping: the
    // two arrow buttons move one frame, and a paused picture that is not the
    // picture being looked at any more needs a way back to the current one.
    auto* recapture = new QToolButton(this);
    recapture->setObjectName(QStringLiteral("compareRecapture"));
    recapture->setText(QStringLiteral("Оновити кадр"));
    connect(recapture, &QToolButton::clicked, this, &CompareWindow::captureNow);
    transport->addWidget(recapture);

    transport_ = new QLabel(this);
    transport_->setObjectName(QStringLiteral("compareTransport"));
    transport_->setMinimumWidth(kTransportWidth);
    transport->addWidget(transport_);

    hover_ = new QLabel(this);
    hover_->setObjectName(QStringLiteral("compareHover"));
    transport->addWidget(hover_, 1);

    auto* close = new QPushButton(QStringLiteral("Закрити"), this);
    close->setObjectName(QStringLiteral("compareClose"));
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    transport->addWidget(close);
    right->addLayout(transport);

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
    info_->setFixedHeight(kInfoHeight);
    info_->setFont(uiFont(9));
    info_->setStyleSheet(QStringLiteral("color: %1; background: transparent;").arg(w55().name()));
    right->addWidget(info_);

    root->addLayout(right, 1);

    flickerTimer_ = new QTimer(this);
    connect(flickerTimer_, &QTimer::timeout, this, &CompareWindow::onFlickerTimeout);
}

void CompareWindow::populateCandidates() {
    const QString shadersDir = anime4k::findShadersDir();
    const QVector<anime4k::Candidate> all = anime4k::allCandidates(shadersDir);
    candidates_.clear();
    candidates_.reserve(all.size());
    for (const anime4k::Candidate& candidate : all) {
        candidates_.append(candidate.id);
    }
    // No shaders on disk is a real state, not an error: the window opens with
    // both sides empty and says so in the info line.
    selection_.clear();
    if (candidates_.isEmpty()) {
        return;
    }
    // Two sides, always: the chain the player is running opens as A, and the
    // next preset as B, so the window opens on a comparison rather than on an
    // empty pane. A catalogue of one compares a chain with itself, which is
    // still better than nothing.
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
        captureError_ = QStringLiteral("Немає поверхні відтворення");
        return;
    }
    if (!surface_->hasVideo()) {
        captureError_ = QStringLiteral("Немає відео для порівняння");
        return;
    }
    if (surface_->isPlaying()) {
        surface_->setPaused(true);
    }
    double settled = 0.0;
    const QImage grabbed = grabSettledFrame(&settled);
    if (grabbed.isNull()) {
        captureError_ = QStringLiteral("Не вдалося зняти новий кадр");
        return;
    }
    // Both sides render from this one image, so the difference between them is
    // the chain and nothing else.
    frame_ = grabbed;
    position_ = settled;
}

void CompareWindow::captureAt(double seconds) {
    // The frame-step buttons are the one place that moves the picture, so this
    // is the one place that seeks. It then waits for the seek to land and for
    // the framebuffer to catch up with it before keeping anything, and it keeps
    // nothing at all when that does not happen - the same no-stale-fallback
    // rule as captureFrame().
    if (surface_ == nullptr) {
        return;
    }
    frame_ = QImage();
    captureError_.clear();
    if (surface_->isPlaying()) {
        surface_->setPaused(true);
    }
    const double duration = surface_->duration();
    const double target = duration > 0.0 ? std::clamp(seconds, 0.0, duration) : seconds;
    surface_->seek(target);
    bool landed = false;
    for (int waited = 0; waited < kSeekTimeoutMs; waited += kPollIntervalMs) {
        QCoreApplication::processEvents();
        QThread::msleep(kPollIntervalMs);
        if (std::abs(surface_->position() - target) < kSeekTolerance) {
            landed = true;
            break;
        }
    }
    if (!landed) {
        captureError_ = QStringLiteral("Не вдалося перейти на кадр %1 с")
                            .arg(QString::number(target, 'f', 2));
        return;
    }
    double settled = 0.0;
    const QImage grabbed = grabSettledFrame(&settled);
    if (grabbed.isNull()) {
        captureError_ = QStringLiteral("Не вдалося зняти кадр %1 с")
                            .arg(QString::number(target, 'f', 2));
        return;
    }
    frame_ = grabbed;
    position_ = settled;
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
        // The reason, not a generic "no frame": a capture that failed and a
        // window that was never given a frame are different faults, and saying
        // which is the difference between a diagnosable window and a shrug.
        // Nothing of the previous picture is left on screen either.
        renderedA_ = QImage();
        renderedB_ = QImage();
        canvas_->clear(captureError_.isEmpty()
                           ? QStringLiteral("Немає кадру для порівняння")
                           : captureError_);
        canvas_->setSideImages(QImage(), QImage(), QImage());
        grid_->setItems({}, {}, {});
        updateInfo();
        return;
    }
    const QImage a = renderWithChain(sideA());
    const QImage b = renderWithChain(sideB());
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
    canvas_->setCandidates(candidates_, [&] {
        QStringList labels;
        labels.reserve(candidates_.size());
        for (const QString& id : candidates_) {
            labels.append(labelFor(id));
        }
        return labels;
    }(), sideA(), sideB());
    canvas_->setMode(mode_);
    canvas_->setSideImages(a.isNull() ? frame_ : a, b.isNull() ? frame_ : b, difference);
    stack_->setCurrentIndex(mode_ == CompareMode::SideBySide ? 1 : 0);
    if (mode_ == CompareMode::SideBySide) {
        QStringList labels;
        labels.reserve(candidates_.size());
        for (const QString& id : candidates_) {
            labels.append(labelFor(id));
        }
        grid_->setChoices(candidates_, labels);
        grid_->setItems({sideA(), sideB()},
                        {a.isNull() ? frame_ : a, b.isNull() ? frame_ : b},
                        {labelFor(sideA()), labelFor(sideB())});
    }
    updateInfo();
    updateTransport();
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
    const bool isOverlay = mode == CompareMode::Overlay;
    const bool isFlicker = mode == CompareMode::Flicker;
    const bool isDifference = mode == CompareMode::Difference;
    splitLabel_->setVisible(isSplit);
    split_->setVisible(isSplit);
    opacityLabel_->setVisible(isOverlay);
    opacity_->setVisible(isOverlay);
    flickerLabel_->setVisible(isFlicker);
    flicker_->setVisible(isFlicker);
    gainLabel_->setVisible(isDifference);
    gain_->setVisible(isDifference);
    canvas_->setSplit(split_->value());
    canvas_->setOpacity(opacity_->value() / 100.0);
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

void CompareWindow::onOpacityChanged(int value) {
    canvas_->setOpacity(value / 100.0);
}

void CompareWindow::onFlickerChanged(int value) {
    canvas_->setFlickerRate(value);
    updateFlickerTimer();
}

void CompareWindow::onGainChanged(int value) {
    canvas_->setDiffGain(value);
    render();
}

void CompareWindow::onPlayPause() {
    if (surface_ == nullptr) {
        return;
    }
    surface_->setPaused(!surface_->isPlaying());
    updateTransport();
}

void CompareWindow::onStepForward() {
    if (surface_ == nullptr) {
        return;
    }
    captureAt(std::max(0.0, position_ + kFrameStep));
    render();
}

void CompareWindow::onStepBack() {
    if (surface_ == nullptr) {
        return;
    }
    captureAt(std::max(0.0, position_ - kFrameStep));
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
        line += QStringLiteral(" · %1 шейдер").arg(candidate.files.size());
        if (candidate.scale > 0) {
            line += QStringLiteral(" · ×%1").arg(candidate.scale);
        }
        if (!anime4k::chainIsComplete(candidate, shadersDir)) {
            line += QStringLiteral(" · ФАЙЛ ВІДСУТНІЙ");
        }
        parts.append(line);
    };
    describe(QStringLiteral("A"), a, infoA);
    describe(QStringLiteral("B"), b, infoB);
    // A failed capture leads the line. Whatever the two sides are, the
    // comparison itself did not happen, and that outranks naming them.
    if (!captureError_.isEmpty()) {
        info_->setText(QStringLiteral("Помилка захоплення: %1").arg(captureError_));
        return;
    }
    if (parts.isEmpty()) {
        info_->setText(QStringLiteral("Оберіть щонайменше один варіант для порівняння."));
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
            parts.append(QStringLiteral("вікно вужче за джерелом — апскейл не запуститься"));
        }
    }
    if (mode_ == CompareMode::Difference && !renderedA_.isNull() && !renderedB_.isNull() &&
        renderedA_.size() == renderedB_.size()) {
        parts.append(QStringLiteral("сер. різниця %1/255")
                         .arg(QString::number(meanAbsoluteDifference(renderedA_, renderedB_),
                                              'f', 2)));
    }
    info_->setText(parts.join(QStringLiteral("  ·  ")));
}

void CompareWindow::updateTransport() {
    if (surface_ == nullptr) {
        transport_->setText(QString());
        return;
    }
    // The window pauses playback to freeze a frame, so the label is derived from
    // the surface rather than set where the button is pressed: on open the
    // player is already paused and the button has to say so.
    play_->setText(surface_->isPlaying() ? QStringLiteral("Пауза") : QStringLiteral("Грати"));
    const double duration = surface_->duration();
    // The position the two sides were actually taken from, not the surface's
    // current one. They differ whenever a capture failed, and then this label
    // is the only place that says so.
    const double shown = frame_.isNull() ? surface_->position() : position_;
    transport_->setText(QStringLiteral("%1 / %2 с")
                            .arg(QString::number(shown, 'f', 2),
                                 QString::number(duration, 'f', 2)));
}

QString CompareWindow::labelFor(const QString& id) const {
    const anime4k::Candidate candidate = anime4k::candidateForId(id);
    return candidate.label.isEmpty() ? QStringLiteral("—") : candidate.label;
}

}  // namespace shell
}  // namespace mplayer
