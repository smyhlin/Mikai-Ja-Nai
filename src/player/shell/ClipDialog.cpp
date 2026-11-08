#include "ClipDialog.hpp"

#include "ClipExport.hpp"
#include "ShellTheme.hpp"
#include "../core/MediaTracks.hpp"
#include "../core/VideoSurface.hpp"

#include <QCloseEvent>
#include <QClipboard>
#include <QCoreApplication>
#include <QComboBox>
#include <QDesktopServices>
#include <QEvent>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPalette>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QPointer>
#include <QStringList>
#include <QSlider>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

namespace mplayer::shell {
namespace {

class ClipTranslation {
    Q_DECLARE_TR_FUNCTIONS(mplayer::shell::ClipDialog)
};

QString clipDialogStyleSheet() {
    const double radius = cornerRadius() > 0.0 ? cornerRadius() : 8.0;
    const double cardRadius = radius + 4.0;
    const QString acc = accent().name(QColor::HexRgb);
    const QString ink = inkOn(accent()).name(QColor::HexRgb);
    const QString accRgb = QStringLiteral("%1, %2, %3").arg(accent().red()).arg(accent().green()).arg(accent().blue());
    const QString hoverHex = accent().lighter(115).name(QColor::HexRgb);
    return QStringLiteral(
        "QDialog#clipDialog {"
        "  background-color: #0e0f14;"
        "  color: #f4f4f5;"
        "}"
        "QFrame#clipCard {"
        "  background-color: #15171e;"
        "  border: 1px solid rgba(255, 255, 255, 0.10);"
        "  border-radius: %1px;"
        "}"
        "QLabel {"
        "  color: #a1a1aa;"
        "  background: transparent;"
        "  font-size: 13px;"
        "}"
        "QLabel#clipSectionTitle {"
        "  color: #f4f4f5;"
        "  font-size: 13px;"
        "  font-weight: 600;"
        "}"
        "QLabel#clipDurationBadge {"
        "  background-color: rgba(%4, 0.18);"
        "  color: %2;"
        "  border: 1px solid rgba(%4, 0.35);"
        "  border-radius: 4px;"
        "  padding: 2px 8px;"
        "  font-size: 12px;"
        "  font-weight: 600;"
        "}"
        "QLabel#clipPreviewTime {"
        "  color: #e4e4e7;"
        "  font-size: 12px;"
        "  font-weight: 500;"
        "}"
        "QLabel#clipStatus {"
        "  color: #a1a1aa;"
        "  font-size: 12px;"
        "}"
        "QComboBox#clipQuality {"
        "  background-color: #1d2029;"
        "  color: #f4f4f5;"
        "  border: 1px solid rgba(255, 255, 255, 0.15);"
        "  border-radius: %5px;"
        "  padding: 3px 10px;"
        "  min-height: 24px;"
        "  font-size: 13px;"
        "  font-weight: 500;"
        "}"
        "QComboBox#clipQuality:hover {"
        "  border-color: %2;"
        "}"
        "QComboBox#clipQuality QAbstractItemView {"
        "  background-color: #15171e;"
        "  color: #f4f4f5;"
        "  border: 1px solid rgba(255, 255, 255, 0.18);"
        "  selection-background-color: %2;"
        "  selection-color: %3;"
        "}"
        "QDoubleSpinBox {"
        "  background-color: #1d2029;"
        "  color: #f4f4f5;"
        "  border: 1px solid rgba(255, 255, 255, 0.15);"
        "  border-radius: %5px;"
        "  padding: 3px 8px;"
        "  min-height: 24px;"
        "  font-size: 13px;"
        "  font-weight: 500;"
        "}"
        "QDoubleSpinBox:focus {"
        "  border-color: %2;"
        "  background-color: #242834;"
        "}"
        "QDoubleSpinBox::up-button, QDoubleSpinBox::down-button {"
        "  width: 16px;"
        "  background: transparent;"
        "  border: none;"
        "}"
        "QPushButton {"
        "  background-color: #1d2029;"
        "  color: #f4f4f5;"
        "  border: 1px solid rgba(255, 255, 255, 0.15);"
        "  border-radius: %5px;"
        "  padding: 4px 14px;"
        "  min-height: 26px;"
        "  font-size: 13px;"
        "  font-weight: 500;"
        "}"
        "QPushButton:hover {"
        "  background-color: #272b38;"
        "  border-color: rgba(255, 255, 255, 0.25);"
        "  color: #ffffff;"
        "}"
        "QPushButton:checked {"
        "  background-color: rgba(%4, 0.22);"
        "  border-color: %2;"
        "  color: %2;"
        "}"
        "QPushButton#clipExport {"
        "  background-color: %2;"
        "  color: %3;"
        "  border: 1px solid %2;"
        "  border-radius: %5px;"
        "  font-weight: 600;"
        "  min-height: 30px;"
        "  padding: 4px 20px;"
        "}"
        "QPushButton#clipExport:hover {"
        "  background-color: %6;"
        "  border-color: %6;"
        "}"
        "QPushButton#clipExport:disabled {"
        "  background-color: rgba(255, 255, 255, 0.08);"
        "  color: rgba(255, 255, 255, 0.35);"
        "  border-color: transparent;"
        "}"
        "QProgressBar#clipProgress {"
        "  background-color: rgba(255, 255, 255, 0.10);"
        "  border-radius: 3px;"
        "  border: none;"
        "  min-height: 6px;"
        "  max-height: 6px;"
        "}"
        "QProgressBar#clipProgress::chunk {"
        "  background-color: %2;"
        "  border-radius: 3px;"
        "}"
        "QSlider#clipPreviewScrub::groove:horizontal {"
        "  height: 4px;"
        "  background: rgba(255, 255, 255, 0.15);"
        "  border-radius: 2px;"
        "}"
        "QSlider#clipPreviewScrub::sub-page:horizontal {"
        "  background: %2;"
        "  border-radius: 2px;"
        "}"
        "QSlider#clipPreviewScrub::handle:horizontal {"
        "  width: 14px;"
        "  height: 14px;"
        "  margin: -5px 0;"
        "  border-radius: 7px;"
        "  background: #ffffff;"
        "}"
        "QSlider#clipPreviewScrub::handle:horizontal:hover {"
        "  background: %2;"
        "}")
        .arg(QString::number(cardRadius, 'f', 1), acc, ink, accRgb,
             QString::number(radius, 'f', 1), hoverHex);
}

class ClipRange final : public QWidget {
public:
    explicit ClipRange(QWidget* parent = nullptr) : QWidget(parent) {
        setObjectName(QStringLiteral("clipRange"));
        setAccessibleName(ClipTranslation::tr("Clip range"));
        setAccessibleDescription(ClipTranslation::tr("Use left and right arrows to adjust the selected clip range."));
        setFocusPolicy(Qt::StrongFocus);
        setMinimumHeight(36);
        setMinimumWidth(250);
    }
    void setRange(double duration, double start, double end) {
        duration_ = std::max(0.0, duration);
        start_ = std::clamp(start, 0.0, duration_);
        end_ = std::clamp(end, start_, duration_);
        update();
    }
    double start() const { return start_; }
    double end() const { return end_; }
    void setChanged(std::function<void(double, double)> changed) { changed_ = std::move(changed); }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        enableChromeAntialiasing(painter);
        const qreal y = height() / 2.0;
        const qreal left = 14.0;
        const qreal right = width() - 14.0;
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(255, 255, 255, 28));
        painter.drawRoundedRect(QRectF(left, y - 3, right - left, 6), 3, 3);
        const qreal a = coordinate(start_);
        const qreal b = coordinate(end_);
        painter.setBrush(accent());
        painter.drawRoundedRect(QRectF(a, y - 3, std::max<qreal>(0, b - a), 6), 3, 3);
        painter.setBrush(QColor(240, 240, 245));
        painter.setPen(QPen(QColor(18, 19, 26), 1.5));
        painter.drawEllipse(QPointF(a, y), 8, 8);
        painter.drawEllipse(QPointF(b, y), 8, 8);
        painter.setPen(QPen(accent(), 2));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(QPointF(activeEnd_ ? b : a, y), 11, 11);
    }
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton || duration_ <= 0.0) return;
        setFocus(Qt::MouseFocusReason);
        const double seconds = valueAt(event->position().x());
        activeEnd_ = std::abs(seconds - end_) < std::abs(seconds - start_);
        updateValue(seconds);
        event->accept();
    }
    void mouseMoveEvent(QMouseEvent* event) override {
        if (event->buttons() & Qt::LeftButton) updateValue(valueAt(event->position().x()));
    }
    void keyPressEvent(QKeyEvent* event) override {
        if (event->key() == Qt::Key_Tab || event->key() == Qt::Key_Backtab) {
            QWidget::keyPressEvent(event);
            return;
        }
        double step = event->modifiers().testFlag(Qt::ShiftModifier) ? 1.0 : 0.1;
        if (event->key() == Qt::Key_Left || event->key() == Qt::Key_Down) step = -step;
        else if (event->key() != Qt::Key_Right && event->key() != Qt::Key_Up) {
            QWidget::keyPressEvent(event);
            return;
        }
        updateValue((activeEnd_ ? end_ : start_) + step);
        event->accept();
    }
private:
    qreal coordinate(double value) const {
        constexpr qreal inset = 14.0;
        return inset + (width() - 2.0 * inset) * (duration_ > 0.0 ? value / duration_ : 0.0);
    }
    double valueAt(qreal x) const {
        constexpr qreal inset = 14.0;
        return duration_ * std::clamp((x - inset) / std::max<qreal>(1.0, width() - 2.0 * inset), 0.0, 1.0);
    }
    void updateValue(double value) {
        value = std::clamp(value, 0.0, duration_);
        if (activeEnd_) end_ = std::clamp(value, start_, std::min(duration_, start_ + kMaximumClipDuration));
        else start_ = std::clamp(value, std::max(0.0, end_ - kMaximumClipDuration), end_);
        update();
        if (changed_) changed_(start_, end_);
    }
    double duration_ = 0.0;
    double start_ = 0.0;
    double end_ = 0.0;
    bool activeEnd_ = false;
    std::function<void(double, double)> changed_;
};

QString formatSeconds(double value) {
    const qint64 ms = static_cast<qint64>(std::llround(std::max(0.0, value) * 1000.0));
    return QStringLiteral("%1:%2.%3")
        .arg(ms / 60000, 2, 10, QLatin1Char('0'))
        .arg((ms / 1000) % 60, 2, 10, QLatin1Char('0'))
        .arg(ms % 1000, 3, 10, QLatin1Char('0'));
}

QDoubleSpinBox* makeTimeSpin(QWidget* parent, const QString& name) {
    auto* spin = new QDoubleSpinBox(parent);
    spin->setObjectName(name);
    spin->setDecimals(3);
    spin->setSingleStep(0.1);
    spin->setKeyboardTracking(false);
    spin->setSuffix(ClipTranslation::tr(" s"));
    spin->setMinimumWidth(112);
    spin->setAccessibleName(name == QLatin1String("clipStart") ? ClipTranslation::tr("Clip start time")
                                                                 : ClipTranslation::tr("Clip end time"));
    return spin;
}

} // namespace

class ClipDialog::Private final : public QObject {
public:
    explicit Private(ClipDialog* dialog, VideoSurface* source, const QString& caption)
        : QObject(dialog), dialog(dialog), source(source), caption(caption) {
        build();
        if (!source || source->sourceUrl().isEmpty()) {
            unavailable(QT_TRANSLATE_NOOP("mplayer::shell::ClipDialog", "The current video source is unavailable."));
            return;
        }
        sourceUrl = source->sourceUrl();
        connect(source, &VideoSurface::mediaInformationInvalidated, this, [this] {
            invalidated = true;
            exportWorker.cancel();
            preview->close();
            unavailable(QT_TRANSLATE_NOOP("mplayer::shell::ClipDialog", "The video changed. Close this window and open Cut moment again."));
        });
        connect(source, &VideoSurface::sourceEpochChanged, this, [this](uint64_t) {
            invalidated = true;
            exportWorker.cancel();
            preview->close();
            unavailable(QT_TRANSLATE_NOOP("mplayer::shell::ClipDialog", "The video changed. Close this window and open Cut moment again."));
        });
        connect(source, &VideoSurface::destroyed, this, [this] {
            invalidated = true;
            exportWorker.cancel();
            if (preview) preview->close();
            unavailable(QT_TRANSLATE_NOOP("mplayer::shell::ClipDialog", "The video player was closed."));
        });
        connect(source, &VideoSurface::durationChanged, this, [this](double seconds) {
            if (invalidated || exporting || !std::isfinite(seconds) || seconds <= 0.0) return;
            duration = seconds;
            if (!rangeInitialized) {
                initializeRange();
                return;
            }
            setTimes(start->value(), end->value(), 0);
        });
        connect(source, &VideoSurface::videoSizeChanged, this, [this](int width, int height) {
            if (invalidated || exporting || width <= 0 || height <= 0) return;
            size = QSize(width, height);
            exportButton->setEnabled(validRange());
            previewScrub->setEnabled(duration > 0.0);
        });
        connect(preview, &VideoSurface::errorOccurred, this, [this](const QString& reason) {
            if (!invalidated) {
                statusSource = QStringLiteral("Preview unavailable: %1");
                statusArgs = {reason};
                status->setText(ClipTranslation::tr("Preview unavailable: %1").arg(reason));
            }
        });
        connect(preview, &VideoSurface::durationChanged, this, [this](double seconds) {
            if (std::isfinite(seconds) && seconds > 0.0 && preview && start) {
                preview->setPaused(true);
                preview->seek(std::clamp(start->value(), 0.0, seconds));
            }
        });
        connect(source, &VideoSurface::mediaTracksChanged, this, [this] { captureTracks(); });
        connect(preview, &VideoSurface::mediaTracksChanged, this, [this] { syncPreviewAudio(); });
        connect(&exportWorker, &ClipExport::encodingStarted, this, [this](const QString& codec) {
            codecName = codec;
            setStatus(QT_TRANSLATE_NOOP("mplayer::shell::ClipDialog", "Encoding MP4 with %1…"), {codecName});
        });
        connect(&exportWorker, &ClipExport::progressChanged, this, [this](double fraction) {
            progress->setValue(static_cast<int>(std::clamp(fraction, 0.0, 1.0) * 1000.0));
            setStatus(QT_TRANSLATE_NOOP("mplayer::shell::ClipDialog", "Encoding MP4 with %1… %2%"),
                      {codecName.isEmpty() ? ClipTranslation::tr("video") : codecName,
                       QString::number(progress->value() / 10.0, 'f', 1)});
        });
        connect(&exportWorker, &ClipExport::finished, this,
                [this](bool succeeded, const QString& path, const QString& error) {
            exporting = false;
            progress->setVisible(false);
            cancel->setVisible(false);
            exportButton->setEnabled(!invalidated && validRange());
            start->setEnabled(!invalidated && rangeInitialized);
            end->setEnabled(!invalidated && rangeInitialized);
            range->setEnabled(!invalidated && rangeInitialized);
            quality->setEnabled(!invalidated && rangeInitialized);
            if (!succeeded) {
                if (error.isEmpty()) setStatus(QT_TRANSLATE_NOOP("mplayer::shell::ClipDialog", "Export failed."));
                else setExternalStatus(error);
                exportButtonState = ExportButtonState::Retry;
                retranslateUi();
                return;
            }
            completedPath = path;
            setStatus(QT_TRANSLATE_NOOP("mplayer::shell::ClipDialog", "Saved %1"), {QFileInfo(path).fileName()});
            exportButtonState = ExportButtonState::Another;
            retranslateUi();
            reveal->setVisible(true);
            copyPath->setVisible(true);
        });
        captureTracks();
        const QSize sourceSize(source->videoWidth(), source->videoHeight());
        if (sourceSize.isValid()) size = sourceSize;
        if (duration > 0.0) initializeRange();
        preview->setMuted(true);
        preview->open(sourceUrl);
        preview->setPaused(true);
        if (std::isfinite(source->duration()) && source->duration() > 0.0) {
            duration = source->duration();
            initializeRange();
        }
        retranslateUi();
    }

    ~Private() override { exportWorker.cancel(); }

    enum class ExportButtonState { Default, Retry, Another };

    void retranslateUi() {
        dialog->setWindowTitle(caption.trimmed().isEmpty()
                                   ? ClipTranslation::tr("Cut moment")
                                   : ClipTranslation::tr("Cut moment — %1").arg(caption));
        dialog->setStyleSheet(clipDialogStyleSheet());
        range->setAccessibleName(ClipTranslation::tr("Clip range"));
        range->setAccessibleDescription(ClipTranslation::tr("Use left and right arrows to adjust the selected clip range."));
        if (rangeTitle) rangeTitle->setText(ClipTranslation::tr("Clip range"));
        startLabel->setText(ClipTranslation::tr("Start"));
        endLabel->setText(ClipTranslation::tr("End"));
        start->setSuffix(ClipTranslation::tr(" s"));
        start->setAccessibleName(ClipTranslation::tr("Clip start time"));
        end->setSuffix(ClipTranslation::tr(" s"));
        end->setAccessibleName(ClipTranslation::tr("Clip end time"));
        qualityLabel->setText(ClipTranslation::tr("Maximum resolution"));
        quality->setItemText(0, ClipTranslation::tr("1080p"));
        quality->setItemText(1, ClipTranslation::tr("720p"));
        quality->setItemText(2, ClipTranslation::tr("480p"));
        previewScrub->setAccessibleName(ClipTranslation::tr("Scrub within selected moment"));
        previewTime->setText(previewPositionKnown
                                 ? ClipTranslation::tr("Preview %1").arg(formatSeconds(previewPosition))
                                 : ClipTranslation::tr("Preview starts at the selected in point"));
        if (rangeInitialized)
            durationLabel->setText(ClipTranslation::tr("%1 selected").arg(formatSeconds(end->value() - start->value())));
        playPreview->setText(playPreview->isChecked() ? ClipTranslation::tr("Pause preview") : ClipTranslation::tr("Play preview"));
        previewAudio->setText(previewAudio->isChecked() ? ClipTranslation::tr("Preview audio on") : ClipTranslation::tr("Preview audio off"));
        exportButton->setText(exporting ? ClipTranslation::tr("Exporting…")
                                       : exportButtonState == ExportButtonState::Retry ? ClipTranslation::tr("Try again")
                                       : exportButtonState == ExportButtonState::Another ? ClipTranslation::tr("Export another")
                                                                                         : ClipTranslation::tr("Export MP4…"));
        cancel->setText(ClipTranslation::tr("Cancel export"));
        reveal->setText(ClipTranslation::tr("Show in folder"));
        copyPath->setText(ClipTranslation::tr("Copy path"));
        if (!statusSource.isEmpty()) {
            QString text = ClipTranslation::tr(statusSource.toUtf8().constData());
            for (const QString& arg : statusArgs) text = text.arg(arg);
            status->setText(text);
        }
    }

    void setStatus(const char* sourceText, std::initializer_list<QString> args = {}) {
        statusSource = QString::fromUtf8(sourceText);
        statusArgs = QStringList(args);
        QString text = ClipTranslation::tr(sourceText);
        for (const QString& arg : statusArgs) text = text.arg(arg);
        status->setText(text);
    }

    void setExternalStatus(const QString& text) {
        statusSource.clear();
        statusArgs.clear();
        status->setText(text);
    }

    void build() {
        dialog->setObjectName(QStringLiteral("clipDialog"));
        dialog->setWindowTitle(ClipTranslation::tr("Cut moment"));
        dialog->setModal(false);
        dialog->setMinimumWidth(640);
        dialog->setMinimumHeight(520);
        dialog->resize(700, 580);
        dialog->setWindowFlag(Qt::WindowContextHelpButtonHint, false);

        const double radius = cornerRadius() > 0.0 ? cornerRadius() : 8.0;

        auto* root = new QVBoxLayout(dialog);
        root->setContentsMargins(14, 14, 14, 14);
        root->setSpacing(10);

        // Card 1: Video Preview & Transport Bar
        previewCard = new QFrame(dialog);
        previewCard->setObjectName(QStringLiteral("clipCard"));
        auto* previewLayout = new QVBoxLayout(previewCard);
        previewLayout->setContentsMargins(10, 10, 10, 10);
        previewLayout->setSpacing(8);

        preview = new VideoSurface(previewCard);
        preview->setObjectName(QStringLiteral("clipPreview"));
        preview->setMinimumHeight(180);
        preview->setMaximumHeight(320);
        preview->setDisplayCornerRadius(qMax(4.0, radius));
        preview->setFocusPolicy(Qt::StrongFocus);
        previewLayout->addWidget(preview, 1);

        auto* previewBar = new QHBoxLayout;
        previewBar->setContentsMargins(2, 0, 2, 0);
        previewBar->setSpacing(8);
        playPreview = new QPushButton(ClipTranslation::tr("Play preview"), previewCard);
        playPreview->setObjectName(QStringLiteral("clipPreviewPlay"));
        playPreview->setCheckable(true);
        previewAudio = new QPushButton(ClipTranslation::tr("Preview audio off"), previewCard);
        previewAudio->setObjectName(QStringLiteral("clipPreviewAudio"));
        previewAudio->setCheckable(true);
        previewScrub = new QSlider(Qt::Horizontal, previewCard);
        previewScrub->setObjectName(QStringLiteral("clipPreviewScrub"));
        previewScrub->setRange(0, 1000);
        previewScrub->setAccessibleName(ClipTranslation::tr("Scrub within selected moment"));
        previewScrub->setEnabled(false);
        previewTime = new QLabel(ClipTranslation::tr("Preview starts at the selected in point"), previewCard);
        previewTime->setObjectName(QStringLiteral("clipPreviewTime"));
        previewTime->setTextInteractionFlags(Qt::TextSelectableByMouse);
        previewBar->addWidget(playPreview);
        previewBar->addWidget(previewAudio);
        previewBar->addWidget(previewScrub, 1);
        previewBar->addWidget(previewTime);
        previewLayout->addLayout(previewBar);
        root->addWidget(previewCard, 1);

        // Card 2: Range & Cut Selection
        rangeCard = new QFrame(dialog);
        rangeCard->setObjectName(QStringLiteral("clipCard"));
        auto* rangeLayout = new QVBoxLayout(rangeCard);
        rangeLayout->setContentsMargins(12, 10, 12, 10);
        rangeLayout->setSpacing(8);

        auto* rangeHeader = new QHBoxLayout;
        rangeHeader->setContentsMargins(0, 0, 0, 0);
        rangeTitle = new QLabel(ClipTranslation::tr("Clip range"), rangeCard);
        rangeTitle->setObjectName(QStringLiteral("clipSectionTitle"));
        rangeHeader->addWidget(rangeTitle);
        rangeHeader->addStretch(1);
        durationLabel = new QLabel(rangeCard);
        durationLabel->setObjectName(QStringLiteral("clipDurationBadge"));
        rangeHeader->addWidget(durationLabel);
        rangeLayout->addLayout(rangeHeader);

        range = new ClipRange(rangeCard);
        rangeLayout->addWidget(range);

        auto* times = new QHBoxLayout;
        times->setContentsMargins(0, 0, 0, 0);
        times->setSpacing(6);
        startLabel = new QLabel(rangeCard);
        times->addWidget(startLabel);
        start = makeTimeSpin(rangeCard, QStringLiteral("clipStart"));
        times->addWidget(start);
        times->addSpacing(16);
        endLabel = new QLabel(rangeCard);
        times->addWidget(endLabel);
        end = makeTimeSpin(rangeCard, QStringLiteral("clipEnd"));
        times->addWidget(end);
        times->addStretch(1);
        rangeLayout->addLayout(times);
        root->addWidget(rangeCard);

        // Card 3: Export Options & Actions
        exportCard = new QFrame(dialog);
        exportCard->setObjectName(QStringLiteral("clipCard"));
        auto* exportLayout = new QVBoxLayout(exportCard);
        exportLayout->setContentsMargins(12, 10, 12, 10);
        exportLayout->setSpacing(8);

        auto* qualityRow = new QHBoxLayout;
        qualityRow->setContentsMargins(0, 0, 0, 0);
        qualityRow->setSpacing(8);
        qualityLabel = new QLabel(exportCard);
        qualityRow->addWidget(qualityLabel);
        quality = new QComboBox(exportCard);
        quality->setObjectName(QStringLiteral("clipQuality"));
        quality->addItem(ClipTranslation::tr("1080p"), 1080);
        quality->addItem(ClipTranslation::tr("720p"), 720);
        quality->addItem(ClipTranslation::tr("480p"), 480);
        qualityRow->addWidget(quality);
        qualityRow->addSpacing(12);
        status = new QLabel(ClipTranslation::tr("Select up to 60 seconds to export as MP4."), exportCard);
        status->setObjectName(QStringLiteral("clipStatus"));
        status->setWordWrap(true);
        qualityRow->addWidget(status, 1);
        exportLayout->addLayout(qualityRow);

        progress = new QProgressBar(exportCard);
        progress->setObjectName(QStringLiteral("clipProgress"));
        progress->setRange(0, 1000);
        progress->setTextVisible(false);
        progress->setVisible(false);
        exportLayout->addWidget(progress);

        auto* actions = new QHBoxLayout;
        actions->setContentsMargins(0, 0, 0, 0);
        actions->setSpacing(8);
        reveal = new QPushButton(ClipTranslation::tr("Show in folder"), exportCard);
        reveal->setVisible(false);
        copyPath = new QPushButton(ClipTranslation::tr("Copy path"), exportCard);
        copyPath->setVisible(false);
        actions->addWidget(reveal);
        actions->addWidget(copyPath);
        actions->addStretch(1);
        cancel = new QPushButton(ClipTranslation::tr("Cancel export"), exportCard);
        cancel->setObjectName(QStringLiteral("clipCancel"));
        cancel->setVisible(false);
        exportButton = new QPushButton(ClipTranslation::tr("Export MP4…"), exportCard);
        exportButton->setObjectName(QStringLiteral("clipExport"));
        exportButton->setDefault(true);
        actions->addWidget(cancel);
        actions->addWidget(exportButton);
        exportLayout->addLayout(actions);
        root->addWidget(exportCard);

        connect(&accentNotifier(), &AccentNotifier::changed, this, [this] {
            dialog->setStyleSheet(clipDialogStyleSheet());
            range->update();
        });
        setStatus("Select up to 60 seconds to export as MP4.");
        range->setChanged([this](double a, double b) { setTimes(a, b, false); });
        connect(start, &QDoubleSpinBox::valueChanged, this, [this](double value) {
            if (syncing) return;
            setTimes(value, end->value(), 1);
        });
        connect(end, &QDoubleSpinBox::valueChanged, this, [this](double value) {
            if (syncing) return;
            setTimes(start->value(), value, 2);
        });
        connect(playPreview, &QPushButton::toggled, this, [this](bool playing) {
            if (!preview || invalidated) return;
            preview->setPaused(!playing);
            playPreview->setText(playing ? ClipTranslation::tr("Pause preview") : ClipTranslation::tr("Play preview"));
        });
        connect(previewAudio, &QPushButton::toggled, this, [this](bool enabled) {
            if (!preview || invalidated) return;
            preview->setMuted(!enabled);
            previewAudio->setText(enabled ? ClipTranslation::tr("Preview audio on") : ClipTranslation::tr("Preview audio off"));
        });
        connect(previewScrub, &QSlider::valueChanged, this, [this](int value) {
            if (!scrubbingPreview && preview && duration > 0.0) {
                preview->seek(start->value() + (end->value() - start->value()) * value / 1000.0);
            }
        });
        connect(preview, &VideoSurface::positionChanged, this, [this](double position) {
            if (duration > 0.0) {
                scrubbingPreview = true;
                previewScrub->setValue(static_cast<int>(std::clamp(
                    (position - start->value()) / std::max(0.001, end->value() - start->value()), 0.0, 1.0) * 1000.0));
                scrubbingPreview = false;
            }
            previewPosition = position;
            previewPositionKnown = true;
            previewTime->setText(ClipTranslation::tr("Preview %1").arg(formatSeconds(position)));
            if (duration > 0.0 && position >= end->value()) {
                preview->setPaused(true);
                preview->seek(start->value());
                playPreview->setChecked(false);
            }
        });
        connect(exportButton, &QPushButton::clicked, this, [this] { chooseOutput(); });
        connect(cancel, &QPushButton::clicked, this, [this] {
            setStatus(QT_TRANSLATE_NOOP("mplayer::shell::ClipDialog", "Canceling export…"));
            cancel->setEnabled(false);
            exportWorker.cancel();
        });
        connect(reveal, &QPushButton::clicked, this, [this] {
            if (!completedPath.isEmpty()) QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(completedPath).absolutePath()));
        });
        connect(copyPath, &QPushButton::clicked, this, [this] {
            if (!completedPath.isEmpty()) QGuiApplication::clipboard()->setText(completedPath);
        });
        retranslateUi();
    }

    void initializeRange() {
        if (rangeInitialized || exporting || invalidated || !std::isfinite(duration) || duration <= 0.0) return;
        const double center = source ? std::clamp(source->position(), 0.0, duration) : 0.0;
        const double a = std::max(0.0, center - 5.0);
        const double b = std::min(duration, a + 10.0);
        const double boundedStart = std::max(0.0, b - kMaximumClipDuration);
        rangeInitialized = true;
        setTimes(std::max(a, boundedStart), b, 0);
        range->setEnabled(true);
        start->setEnabled(true);
        end->setEnabled(true);
        quality->setEnabled(true);
        playPreview->setEnabled(true);
        previewScrub->setEnabled(true);
        exportButton->setEnabled(size.isValid() && !invalidated);
        setStatus(QT_TRANSLATE_NOOP("mplayer::shell::ClipDialog", "Choose a moment up to 60 seconds long."));
    }

    void captureTracks() {
        externalAudio.clear();
        audioTrack.reset();
        if (!source || !source->mediaTracks().ready) return;
        const MediaTrackSelection selection = source->mediaTrackSelection();
        audioTrack = selection.audio;
        if (selection.audio.has_value()) {
            for (const MediaTrack& track : source->mediaTracks().tracks) {
                if (track.type == MediaTrackType::Audio && track.id == *selection.audio && track.external) {
                    externalAudio = track.externalFilename;
                    break;
                }
            }
        }
        if (preview && !externalAudio.isEmpty() && !previewExternalAudioAdded) {
            previewExternalAudioAdded = preview->addExternalTrack(externalAudio, false);
        }
        syncPreviewAudio();
    }

    void syncPreviewAudio() {
        if (!preview || !preview->mediaTracks().ready || !audioTrack.has_value()) return;
        if (!externalAudio.isEmpty() && !previewExternalAudioAdded) {
            previewExternalAudioAdded = preview->addExternalTrack(externalAudio, false);
            if (previewExternalAudioAdded) return;
        }
        const MediaTrackSnapshot& snapshot = preview->mediaTracks();
        for (const MediaTrack& track : snapshot.tracks) {
            if (track.type == MediaTrackType::Audio && track.id == *audioTrack) {
                preview->selectMediaTrack(MediaTrackType::Audio, *audioTrack,
                                          snapshot.coreIdentity, snapshot.sourceEpoch);
                return;
            }
        }
    }

    void setTimes(double a, double b, int editedField) {
        if (duration <= 0.0 || exporting || invalidated || !rangeInitialized) return;
        a = std::clamp(a, 0.0, duration);
        b = std::clamp(b, 0.0, duration);
        if (b <= a) {
            if (a >= duration) a = std::max(0.0, duration - 0.001);
            else b = std::min(duration, a + 0.001);
        }
        if (b - a > kMaximumClipDuration) {
            if (editedField == 1) b = std::min(duration, a + kMaximumClipDuration);
            else if (editedField == 2) a = std::max(0.0, b - kMaximumClipDuration);
            else b = a + kMaximumClipDuration;
        }
        syncing = true;
        start->setRange(0.0, duration);
        end->setRange(0.0, duration);
        start->setValue(a);
        end->setValue(b);
        durationLabel->setText(ClipTranslation::tr("%1 selected").arg(formatSeconds(b - a)));
        range->setRange(duration, a, b);
        syncing = false;
        exportButton->setEnabled(!invalidated && !exporting && size.isValid() && b > a && b - a <= kMaximumClipDuration);
        if (previewScrub) {
            scrubbingPreview = true;
            previewScrub->setValue(0);
            scrubbingPreview = false;
            previewScrub->setEnabled(duration > 0.0);
        }
        if (preview && !invalidated) {
            preview->setPaused(true);
            preview->seek(a);
            playPreview->setChecked(false);
            playPreview->setText(ClipTranslation::tr("Play preview"));
        }
    }

    bool validRange() const {
        if (!rangeInitialized || exporting || invalidated || !start || !end || !quality) return false;
        const double a = start->value();
        const double b = end->value();
        return !invalidated && std::isfinite(a) && std::isfinite(b) && a >= 0.0 && b > a &&
               b <= duration && b - a <= kMaximumClipDuration && size.isValid();
    }

    void chooseOutput() {
        if (exporting || !validRange() || !source) return;
        QPointer<Private> self(this);
        QPointer<ClipDialog> guardedDialog(dialog);
        QPointer<VideoSurface> guardedSource(source);
        const uint64_t sourceEpoch = source->sourceEpoch();
        const QString selectedSource = sourceUrl;
        const double selectedStart = start->value();
        const double selectedEnd = end->value();
        const QSize selectedSize = size;
        const int selectedHeight = quality->currentData().toInt();
        const std::optional<qint64> selectedAudioTrack = audioTrack;
        const QString selectedExternalAudio = externalAudio;
        QString suggestedName = QFileInfo(QUrl(selectedSource).path()).completeBaseName();
        if (suggestedName.isEmpty()) suggestedName = ClipTranslation::tr("clip");
        suggestedName += QStringLiteral(".mp4");
        QString path = QFileDialog::getSaveFileName(dialog, ClipTranslation::tr("Save clip"), suggestedName,
                                                    ClipTranslation::tr("MP4 video (*.mp4)"),
                                                    nullptr, QFileDialog::DontConfirmOverwrite);
        if (!self || !guardedDialog || !guardedSource) return;
        if (invalidated || guardedSource->sourceEpoch() != sourceEpoch || !validRange() ||
            sourceUrl != selectedSource) return;
        if (path.isEmpty()) return;
        if (!path.endsWith(QStringLiteral(".mp4"), Qt::CaseInsensitive)) path += QStringLiteral(".mp4");
        bool replaceExisting = false;
        if (QFileInfo::exists(path)) {
            const auto answer = QMessageBox::question(dialog, ClipTranslation::tr("Replace existing file?"),
                                                      ClipTranslation::tr("%1 already exists. Replace it after the new clip is complete?").arg(QFileInfo(path).fileName()),
                                                      QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (!self || !guardedDialog || !guardedSource) return;
            if (invalidated || guardedSource->sourceEpoch() != sourceEpoch || !validRange() ||
                sourceUrl != selectedSource) return;
            if (answer != QMessageBox::Yes) return;
            replaceExisting = true;
        }
        if (invalidated || guardedSource->sourceEpoch() != sourceEpoch || !validRange() ||
            sourceUrl != selectedSource || start->value() != selectedStart ||
            end->value() != selectedEnd || size != selectedSize ||
            quality->currentData().toInt() != selectedHeight) return;
        ClipExportRequest request;
        request.source = selectedSource;
        request.outputPath = path;
        request.startSeconds = selectedStart;
        request.endSeconds = selectedEnd;
        request.sourceSize = selectedSize;
        request.maximumHeight = selectedHeight;
        request.audioTrackId = selectedAudioTrack;
        request.externalAudioPath = selectedExternalAudio;
        request.replaceExisting = replaceExisting;
        request.sourceOptions = {
            {"user-agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
                           "Chrome/142.0.0.0 Safari/537.36"},
            {"http-header-fields", "Accept-Language: uk-UA"}};
        exporting = true;
        progress->setValue(0);
        progress->setVisible(true);
        cancel->setVisible(true);
        cancel->setEnabled(true);
        exportButton->setText(ClipTranslation::tr("Exporting…"));
        exportButton->setEnabled(false);
        start->setEnabled(false);
        end->setEnabled(false);
        range->setEnabled(false);
        quality->setEnabled(false);
        setStatus(QT_TRANSLATE_NOOP("mplayer::shell::ClipDialog", "Preparing export…"));
        exportButtonState = ExportButtonState::Default;
        exportWorker.start(request);
    }

    void unavailable(const char* sourceText) {
        setStatus(sourceText);
        range->setEnabled(false);
        start->setEnabled(false);
        end->setEnabled(false);
        quality->setEnabled(false);
        playPreview->setEnabled(false);
        previewAudio->setEnabled(false);
        previewScrub->setEnabled(false);
        exportButton->setEnabled(false);
    }

    ClipDialog* dialog;
    QPointer<VideoSurface> source;
    QString caption;
    VideoSurface* preview = nullptr;
    ClipExport exportWorker;
    ClipRange* range = nullptr;
    QDoubleSpinBox* start = nullptr;
    QDoubleSpinBox* end = nullptr;
    QComboBox* quality = nullptr;
    QLabel* status = nullptr;
    QLabel* startLabel = nullptr;
    QLabel* endLabel = nullptr;
    QLabel* qualityLabel = nullptr;
    QLabel* durationLabel = nullptr;
    QLabel* rangeTitle = nullptr;
    QLabel* previewTime = nullptr;
    QFrame* previewCard = nullptr;
    QFrame* rangeCard = nullptr;
    QFrame* exportCard = nullptr;
    QSlider* previewScrub = nullptr;
    QPushButton* playPreview = nullptr;
    QPushButton* previewAudio = nullptr;
    QPushButton* exportButton = nullptr;
    QPushButton* cancel = nullptr;
    QPushButton* reveal = nullptr;
    QPushButton* copyPath = nullptr;
    QProgressBar* progress = nullptr;
    QString sourceUrl;
    QString externalAudio;
    QString completedPath;
    std::optional<qint64> audioTrack;
    QSize size;
    double duration = 0.0;
    QString codecName;
    QString statusSource;
    QStringList statusArgs;
    double previewPosition = 0.0;
    ExportButtonState exportButtonState = ExportButtonState::Default;
    bool previewPositionKnown = false;
    bool syncing = false;
    bool exporting = false;
    bool rangeInitialized = false;
    bool previewExternalAudioAdded = false;
    bool invalidated = false;
    bool scrubbingPreview = false;
};

ClipDialog::ClipDialog(VideoSurface* source, const QString& caption, QWidget* parent)
    : QDialog(parent), d_(new Private(this, source, caption)) {}

void ClipDialog::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange && d_) d_->retranslateUi();
    QDialog::changeEvent(event);
}

ClipDialog::~ClipDialog() = default;

void ClipDialog::closeEvent(QCloseEvent* event) {
    if (d_) {
        if (d_->preview) d_->preview->setPaused(true);
        if (d_->playPreview) d_->playPreview->setChecked(false);
        if (d_->exporting) {
            d_->exportWorker.cancel();
            d_->setStatus(QT_TRANSLATE_NOOP("mplayer::shell::ClipDialog", "Canceling export…"));
        }
    }
    QDialog::closeEvent(event);
}

} // namespace mplayer::shell
