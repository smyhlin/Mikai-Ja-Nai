#include "CompareWindow.hpp"

#include "Anime4K.hpp"
#include "CompareCanvas.hpp"
#include "CompareGrid.hpp"
#include "ShellTheme.hpp"
#include "VideoSurface.hpp"

#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSignalBlocker>
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
constexpr int kListWidth = 300;
constexpr int kRowHeight = 30;
constexpr int kInfoHeight = 44;
constexpr int kTransportWidth = 160;
/// One frame at a typical anime rate, in seconds. The surface exposes no frame
/// index, so stepping by time is the honest equivalent.
constexpr double kFrameStep = 1.0 / 24.0;
/// How close the surface's reported position must land to the requested one
/// before the grabbed frame counts as the right one.
constexpr double kSeekTolerance = 0.12;

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
    populateList();
    if (surface_ != nullptr && surface_->hasVideo()) {
        captureFrame();
    }
    render();
}

CompareWindow::~CompareWindow() = default;

void CompareWindow::buildUi() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(kGap, kGap, kGap, kGap);
    root->setSpacing(kGapSmall);

    auto* body = new QHBoxLayout();
    body->setSpacing(kGap);

    list_ = new QListWidget(this);
    list_->setObjectName(QStringLiteral("compareCandidates"));
    list_->setFixedWidth(kListWidth);
    list_->setUniformItemSizes(true);
    list_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    list_->setFont(uiFont(9));
    connect(list_, &QListWidget::itemSelectionChanged, this, &CompareWindow::onSelectionChanged);
    body->addWidget(list_);

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

    // No zoom buttons: the wheel already zooms about the cursor, and a second
    // way to do the same thing is how a control row turns into a wall.
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

    body->addLayout(right, 1);
    root->addLayout(body, 1);

    flickerTimer_ = new QTimer(this);
    connect(flickerTimer_, &QTimer::timeout, this, &CompareWindow::onFlickerTimeout);
}

void CompareWindow::populateList() {
    const QString shadersDir = anime4k::findShadersDir();
    const QVector<anime4k::Candidate> all = anime4k::allCandidates(shadersDir);
    list_->clear();
    candidates_.clear();
    for (const anime4k::Candidate& candidate : all) {
        auto* item = new QListWidgetItem(candidate.label, list_);
        item->setData(Qt::UserRole, candidate.id);
        item->setToolTip(candidate.hint);
        item->setSizeHint(QSize(0, kRowHeight));
        candidates_.append(candidate.id);
    }
    if (list_->count() == 0) {
        list_->addItem(QStringLiteral("Шадерів не знайдено"));
        list_->setEnabled(false);
        return;
    }
    // Two rows, always: the chain the player is running opens as A, and the
    // next preset as B, so the window opens on a comparison rather than on an
    // empty pane.
    int current = 0;
    for (int row = 0; row < list_->count(); ++row) {
        if (list_->item(row)->data(Qt::UserRole).toString() == currentCandidateId_) {
            current = row;
            break;
        }
    }
    const int other = current == 0 ? (list_->count() > 1 ? 1 : 0) : 0;
    list_->setCurrentRow(current);
    list_->selectionModel()->select(list_->model()->index(current, 0),
                                    QItemSelectionModel::ClearAndSelect);
    if (other != current) {
        list_->selectionModel()->select(list_->model()->index(other, 0),
                                        QItemSelectionModel::Select);
    }
}

QStringList CompareWindow::selection() const {
    QStringList ids;
    if (list_->currentItem() != nullptr) {
        ids.append(list_->currentItem()->data(Qt::UserRole).toString());
    }
    for (QListWidgetItem* item : list_->selectedItems()) {
        const QString id = item->data(Qt::UserRole).toString();
        if (!ids.contains(id)) {
            ids.append(id);
        }
    }
    return ids;
}

QString CompareWindow::sideA() const {
    const QStringList picked = selection();
    return picked.isEmpty() ? QString() : picked.first();
}

QString CompareWindow::sideB() const {
    const QStringList picked = selection();
    return picked.size() > 1 ? picked.at(1) : QString();
}

void CompareWindow::captureFrame() {
    if (surface_ == nullptr) {
        return;
    }
    // Freeze where the viewer already is. A fixed fraction of the clip
    // compares a frame nobody picked - usually a title card or a credit roll,
    // which has none of the detail a restore shader exists to recover - and
    // silently disagreed with whatever was on screen when the window opened.
    if (surface_->isPlaying()) {
        surface_->setPaused(true);
    }
    const double duration = surface_->duration();
    const double at = surface_->position();
    // One seek, then wait for it to land. Re-seeking to the same timestamp does
    // not reproduce the same pixels, so the frame is taken once and both sides
    // render from that single image.
    captureAt(duration > 0.0 ? std::clamp(at, 0.0, duration) : at);
}

void CompareWindow::captureAt(double seconds) {
    // One seek, then wait for it to land. Re-seeking to the same timestamp does
    // not reproduce the same pixels, so the frame is taken once and both sides
    // render from that single image.
    //
    // A failed capture is not allowed to fall back on the previous frame: a
    // stale picture reads as a real comparison, which is worse than none at all.
    frame_ = QImage();
    position_ = seconds;
    surface_->seek(position_);
    for (int attempt = 0; attempt < 40; ++attempt) {
        QCoreApplication::processEvents();
        QThread::msleep(40);
        const QImage probe = surface_->grabFramebuffer();
        if (!probe.isNull() && std::abs(surface_->position() - position_) < kSeekTolerance) {
            frame_ = probe;
            break;
        }
    }
    if (frame_.isNull()) {
        frame_ = surface_->grabFramebuffer();
    }
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
    surface_->update();
    QImage last;
    for (int attempt = 0; attempt < 12; ++attempt) {
        QCoreApplication::processEvents();
        QThread::msleep(30);
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
    return last;
}

void CompareWindow::render() {
    if (frame_.isNull()) {
        canvas_->clear(QStringLiteral("Немає кадру для порівняння"));
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

void CompareWindow::onSelectionChanged() {
    render();
}

void CompareWindow::onCandidateChosen(int side, const QString& id) {
    QString first = sideA();
    QString second = sideB();
    if (side == 0) {
        first = id;
    } else {
        second = id;
        if (first.isEmpty()) {
            first = currentCandidateId_;
        }
    }
    QListWidgetItem* firstItem = nullptr;
    QListWidgetItem* secondItem = nullptr;
    for (int row = 0; row < list_->count(); ++row) {
        QListWidgetItem* item = list_->item(row);
        const QString candidate = item->data(Qt::UserRole).toString();
        if (candidate == first) {
            firstItem = item;
        }
        if (candidate == second) {
            secondItem = item;
        }
    }
    if (firstItem == nullptr) {
        return;
    }
    {
        const QSignalBlocker blocker(list_);
        list_->selectionModel()->clearSelection();
        list_->setCurrentItem(firstItem, QItemSelectionModel::ClearAndSelect);
        if (secondItem != nullptr && secondItem != firstItem) {
            list_->selectionModel()->select(list_->indexFromItem(secondItem),
                                            QItemSelectionModel::Select);
        }
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
    transport_->setText(QStringLiteral("%1 / %2 с")
                            .arg(QString::number(surface_->position(), 'f', 2),
                                 QString::number(duration, 'f', 2)));
}

QString CompareWindow::labelFor(const QString& id) const {
    const anime4k::Candidate candidate = anime4k::candidateForId(id);
    return candidate.label.isEmpty() ? QStringLiteral("—") : candidate.label;
}

}  // namespace shell
}  // namespace mplayer
