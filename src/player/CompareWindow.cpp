#include "CompareWindow.hpp"

#include "Anime4K.hpp"
#include "ShellTheme.hpp"
#include "VideoSurface.hpp"

#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QHBoxLayout>
#include <QLabel>
#include <QItemSelectionModel>
#include <QListWidget>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QSlider>
#include <QThread>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace mplayer {
namespace shell {

using mplayer::VideoSurface;

namespace {

constexpr int kGap = 12;
constexpr int kGapSmall = 8;
constexpr int kWindowWidth = 1180;
constexpr int kWindowHeight = 720;
constexpr int kListWidth = 300;
constexpr int kRowHeight = 30;
/// How far along the video the frozen frame is taken, as a fraction. Mid-clip:
/// the opening of an episode is a title card, and the end is a credit roll,
/// neither of which has the detail a restore shader exists to recover.
constexpr double kFrameFraction = 0.35;
constexpr int kInfoHeight = 44;
/// The gap between the two panels, and the height of the label strip over
/// each. Both are the window's own, not the player dock's.
constexpr int kGutter = 8;
constexpr int kLabelHeight = 22;
constexpr int kDivider = 2;

QString caption(const QString& text) {
    return QStringLiteral(" %1 ").arg(text);
}

}  // namespace

CompareWindow::CompareWindow(VideoSurface* surface, const QString& currentCandidateId,
                             QWidget* parent)
    : QDialog(parent), surface_(surface), appliedId_(currentCandidateId) {
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

    auto* controls = new QHBoxLayout();
    controls->setSpacing(kGapSmall);
    controls->addWidget(new QLabel(QStringLiteral("Режим:"), this));
    layoutBox_ = new QComboBox(this);
    layoutBox_->setObjectName(QStringLiteral("compareLayout"));
    layoutBox_->addItem(QStringLiteral("Поруч"));
    layoutBox_->addItem(QStringLiteral("Розділене"));
    layoutBox_->addItem(QStringLiteral("Різниця"));
    connect(layoutBox_, qOverload<int>(&QComboBox::currentIndexChanged), this,
            &CompareWindow::onLayoutChanged);
    controls->addWidget(layoutBox_);
    controls->addStretch(1);
    right->addLayout(controls);

    canvas_ = new QLabel(this);
    canvas_->setObjectName(QStringLiteral("compareCanvas"));
    canvas_->setAlignment(Qt::AlignCenter);
    canvas_->setMinimumSize(640, 400);
    canvas_->setStyleSheet(
        QStringLiteral("QLabel#compareCanvas { background: %1; border: 1px solid %2;"
                       " border-radius: %3px; }")
            .arg(bg0().name())
            .arg(white(20).name())
            .arg(kGap));
    right->addWidget(canvas_, 1);

    auto* timeline = new QHBoxLayout();
    timeline->setSpacing(kGapSmall);
    timeline->addWidget(new QLabel(QStringLiteral("Кадр:"), this));
    scrub_ = new QSlider(Qt::Horizontal, this);
    scrub_->setObjectName(QStringLiteral("compareScrub"));
    scrub_->setRange(0, 1000);
    connect(scrub_, &QSlider::valueChanged, this, &CompareWindow::onScrubMoved);
    timeline->addWidget(scrub_, 1);
    right->addLayout(timeline);

    split_ = new QSlider(Qt::Horizontal, this);
    split_->setObjectName(QStringLiteral("compareSplit"));
    split_->setRange(0, 1000);
    split_->setValue(500);
    split_->setToolTip(QStringLiteral("Де проходить межа між A та B"));
    connect(split_, &QSlider::valueChanged, this, &CompareWindow::onSplitMoved);
    split_->hide();
    right->addWidget(split_);

    info_ = new QLabel(this);
    info_->setObjectName(QStringLiteral("compareInfo"));
    info_->setWordWrap(true);
    info_->setFixedHeight(kInfoHeight);
    info_->setFont(uiFont(9));
    info_->setStyleSheet(QStringLiteral("color: %1; background: transparent;").arg(w55().name()));
    right->addWidget(info_);

    auto* actions = new QHBoxLayout();
    actions->setSpacing(kGapSmall);
    applyA_ = new QPushButton(QStringLiteral("Застосувати A"), this);
    applyA_->setObjectName(QStringLiteral("compareApplyA"));
    connect(applyA_, &QPushButton::clicked, this, &CompareWindow::onApplyA);
    actions->addWidget(applyA_);
    applyB_ = new QPushButton(QStringLiteral("Застосувати B"), this);
    applyB_->setObjectName(QStringLiteral("compareApplyB"));
    connect(applyB_, &QPushButton::clicked, this, &CompareWindow::onApplyB);
    actions->addWidget(applyB_);
    actions->addStretch(1);
    auto* close = new QPushButton(QStringLiteral("Закрити"), this);
    close->setObjectName(QStringLiteral("compareClose"));
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    actions->addWidget(close);
    right->addLayout(actions);

    body->addLayout(right, 1);
    root->addLayout(body, 1);
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
    // empty pane. A viewer replaces either by clicking a row.
    int current = 0;
    for (int row = 0; row < list_->count(); ++row) {
        if (list_->item(row)->data(Qt::UserRole).toString() == appliedId_) {
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
    // In press order, not list order: the row picked first is A. QItemSelection
    // is a set, so the order the viewer chose is read from the current row
    // plus everything else selected.
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

QString CompareWindow::chainFor(const QString& id) const {
    return anime4k::shaderStringForCandidate(id);
}

void CompareWindow::captureFrame() {
    if (surface_ == nullptr) {
        return;
    }
    const double duration = surface_->duration();
    // One seek, then wait for it to land. Re-seeking to the same timestamp does
    // not reproduce the same pixels, so the frame is taken once and both sides
    // render from that single image - a live preview would differ between two
    // grabs for reasons that have nothing to do with the shader.
    position_ = duration > 0.0 ? std::clamp(duration * kFrameFraction, 0.0, duration) : 0.0;
    if (surface_->isPlaying()) {
        surface_->setPaused(true);
    }
    surface_->seek(position_);
    for (int attempt = 0; attempt < 40; ++attempt) {
        QCoreApplication::processEvents();
        QThread::msleep(40);
        const QImage probe = surface_->grabFramebuffer();
        if (!probe.isNull() && std::abs(surface_->position() - position_) < 1.0) {
            frame_ = probe;
            break;
        }
    }
    if (frame_.isNull()) {
        frame_ = surface_->grabFramebuffer();
    }
}

void CompareWindow::onSelectionChanged() {
    render();
}

void CompareWindow::onLayoutChanged(int) {
    split_->setVisible(layoutBox_->currentIndex() == static_cast<int>(Layout::Split));
    render();
}

void CompareWindow::onScrubMoved(int value) {
    if (surface_ == nullptr) {
        return;
    }
    const double duration = surface_->duration();
    if (duration <= 0.0) {
        return;
    }
    position_ = duration * (static_cast<double>(value) / 1000.0);
    surface_->seek(position_);
    for (int attempt = 0; attempt < 40; ++attempt) {
        QCoreApplication::processEvents();
        QThread::msleep(40);
        if (std::abs(surface_->position() - position_) < 1.0) {
            const QImage probe = surface_->grabFramebuffer();
            if (!probe.isNull()) {
                frame_ = probe;
                break;
            }
        }
    }
    render();
}

void CompareWindow::onSplitMoved(int value) {
    splitAt_ = value;
    render();
}

void CompareWindow::onApplyA() {
    const QString id = sideA();
    if (id.isEmpty()) {
        return;
    }
    appliedId_ = id;
    emit applyRequested(id);
}

void CompareWindow::onApplyB() {
    const QString id = sideB();
    if (id.isEmpty()) {
        return;
    }
    appliedId_ = id;
    emit applyRequested(id);
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
        const double outputWidth =
            static_cast<double>(canvas_->pixmap().width()) * surface_->devicePixelRatioF();
        if (outputWidth / 1.200 < sourceWidth) {
            parts.append(
                QStringLiteral("вікно вужче за джерелом — апскейл не запуститься"));
        }
    }
    info_->setText(parts.join(QStringLiteral("  ·  ")));
}
void CompareWindow::render() {
    if (frame_.isNull()) {
        canvas_->setText(QStringLiteral("Немає кадру для порівняння"));
        canvas_->setPixmap(QPixmap());
        updateInfo();
        return;
    }
    switch (static_cast<Layout>(layoutBox_->currentIndex())) {
        case Layout::SideBySide:
            renderSideBySide();
            break;
        case Layout::Split:
            renderSplit();
            break;
        case Layout::Difference:
            renderDifference();
            break;
    }
    updateInfo();
}

// A side is produced by applying its chain to the live core and taking the
// frame that comes back. The frame is the same pixels every time because it
// is frozen: what changes between the two captures is the filter chain and
// nothing else, which is the only way the two can be compared at all.
QImage CompareWindow::renderWithChain(const QString& id) {
    if (surface_ == nullptr || frame_.isNull()) {
        return QImage();
    }
    const QString previous = surface_->anime4KChainId();
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

void CompareWindow::renderSideBySide() {
    const QImage a = renderWithChain(sideA());
    const QImage b = renderWithChain(sideB());
    const QImage& first = a.isNull() ? frame_ : a;
    const QImage& second = b.isNull() ? frame_ : b;
    QImage canvas(first.size() * 2 + QSize(kGutter, 0), QImage::Format_RGB888);
    canvas.fill(QColor(0, 0, 0).rgb());
    QPainter painter(&canvas);
    painter.drawImage(0, 0, first);
    painter.drawImage(first.width() + kGutter, 0, second);
    // The labels name the chains, not the letters: "A" tells the viewer
    // nothing they did not already know from the list.
    painter.setPen(w90());
    painter.fillRect(QRect(0, 0, first.width(), kLabelHeight), QColor(0, 0, 0, 150));
    painter.fillRect(QRect(first.width() + kGutter, 0, second.width(), kLabelHeight),
                     QColor(0, 0, 0, 150));
    painter.drawText(QRect(0, 0, first.width(), kLabelHeight), Qt::AlignCenter,
                     caption(QStringLiteral("A · %1").arg(labelFor(sideA()))));
    painter.drawText(QRect(first.width() + kGutter, 0, second.width(), kLabelHeight),
                     Qt::AlignCenter,
                     caption(QStringLiteral("B · %1").arg(labelFor(sideB()))));
    painter.end();
    canvas_->setPixmap(QPixmap::fromImage(canvas));
}

void CompareWindow::renderSplit() {
    // One image, one divide: A on the left of the line, B on the right, from
    // the same frozen frame. This is the layout that actually shows a
    // difference, because the eye compares across the seam rather than
    // between two panels.
    const QImage a = renderWithChain(sideA());
    const QImage b = renderWithChain(sideB());
    QImage canvas = a.isNull() ? frame_.convertToFormat(QImage::Format_RGB888)
                               : a.convertToFormat(QImage::Format_RGB888);
    const int cut = std::clamp(canvas.width() * splitAt_ / 1000, 0, canvas.width());
    if (!b.isNull()) {
        const QImage right = b.convertToFormat(QImage::Format_RGB888);
        QPainter blit(&canvas);
        blit.drawImage(QRect(cut, 0, canvas.width() - cut, canvas.height()), right,
                       QRect(cut, 0, canvas.width() - cut, canvas.height()));
    }
    QPainter painter(&canvas);
    painter.fillRect(QRect(cut - 1, 0, kDivider, canvas.height()), accent());
    painter.setPen(w90());
    painter.fillRect(QRect(0, 0, canvas.width(), kLabelHeight), QColor(0, 0, 0, 150));
    painter.drawText(QRect(0, 0, canvas.width(), kLabelHeight), Qt::AlignCenter,
                     caption(QStringLiteral("A · %1   |   B · %2")
                                  .arg(labelFor(sideA()), labelFor(sideB()))));
    painter.end();
    canvas_->setPixmap(QPixmap::fromImage(canvas));
}

void CompareWindow::renderDifference() {
    const QImage a = renderWithChain(sideA());
    const QImage b = renderWithChain(sideB());
    if (a.isNull() || b.isNull() || a.size() != b.size()) {
        canvas_->setText(QStringLiteral("Не вдалося отримати обидва кадри"));
        canvas_->setPixmap(QPixmap());
        return;
    }
    // The absolute per-pixel difference, amplified: what is left once the two
    // chains agree is the measurement noise, and anything above it is a real
    // difference in the picture.
    const QImage left = a.convertToFormat(QImage::Format_RGB888);
    const QImage right = b.convertToFormat(QImage::Format_RGB888);
    QImage out(left.size(), QImage::Format_RGB888);
    double total = 0.0;
    qint64 counted = 0;
    for (int y = 0; y < out.height(); ++y) {
        for (int x = 0; x < out.width(); ++x) {
            const QRgb pa = left.pixel(x, y);
            const QRgb pb = right.pixel(x, y);
            const int dr = std::abs(qRed(pa) - qRed(pb));
            const int dg = std::abs(qGreen(pa) - qGreen(pb));
            const int db = std::abs(qBlue(pa) - qBlue(pb));
            total += dr + dg + db;
            counted += 3;
            out.setPixel(x, y, qRgb(std::min(255, dr * 8), std::min(255, dg * 8),
                                     std::min(255, db * 8)));
        }
    }
    const double mean = counted > 0 ? total / static_cast<double>(counted) : 0.0;
    QPainter painter(&out);
    painter.fillRect(QRect(0, 0, out.width(), kLabelHeight), QColor(0, 0, 0, 150));
    painter.setPen(w90());
    painter.drawText(QRect(0, 0, out.width(), kLabelHeight), Qt::AlignCenter,
                     caption(QStringLiteral("Різниця · середнє %1/255")
                                  .arg(QString::number(mean, 'f', 2))));
    painter.end();
    canvas_->setPixmap(QPixmap::fromImage(out));
}

QString CompareWindow::labelFor(const QString& id) const {
    const anime4k::Candidate candidate = anime4k::candidateForId(id);
    return candidate.label.isEmpty() ? QStringLiteral("—") : candidate.label;
}
}  // namespace shell
}  // namespace mplayer
