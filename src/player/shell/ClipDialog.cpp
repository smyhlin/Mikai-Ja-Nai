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
        painter.setBrush(palette().color(QPalette::Mid));
        painter.drawRoundedRect(QRectF(left, y - 3, right - left, 6), 3, 3);
        const qreal a = coordinate(start_);
        const qreal b = coordinate(end_);
        painter.setBrush(timelinePlayed());
        painter.drawRoundedRect(QRectF(a, y - 3, std::max<qreal>(0, b - a), 6), 3, 3);
        painter.setBrush(palette().color(QPalette::Button));
        painter.setPen(QPen(palette().color(QPalette::Mid), 1));
        painter.drawEllipse(QPointF(a, y), 12, 12);
        painter.drawEllipse(QPointF(b, y), 12, 12);
        painter.setPen(QPen(timelinePlayed(), 2));
        painter.drawEllipse(QPointF(activeEnd_ ? b : a, y), 13, 13);
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
        range->setAccessibleName(ClipTranslation::tr("Clip range"));
        range->setAccessibleDescription(ClipTranslation::tr("Use left and right arrows to adjust the selected clip range."));
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
        dialog->setMinimumWidth(540);
        dialog->setWindowFlag(Qt::WindowContextHelpButtonHint, false);
        auto* root = new QVBoxLayout(dialog);
        root->setContentsMargins(18, 16, 18, 16);
        root->setSpacing(12);
        preview = new VideoSurface(dialog);
        preview->setObjectName(QStringLiteral("clipPreview"));
        preview->setMinimumHeight(180);
        preview->setMaximumHeight(280);
        preview->setFocusPolicy(Qt::StrongFocus);
        root->addWidget(preview, 1);
        auto* previewBar = new QHBoxLayout;
        playPreview = new QPushButton(ClipTranslation::tr("Play preview"), dialog);
        previewAudio = new QPushButton(ClipTranslation::tr("Preview audio off"), dialog);
        previewAudio->setObjectName(QStringLiteral("clipPreviewAudio"));
        previewAudio->setCheckable(true);
        previewScrub = new QSlider(Qt::Horizontal, dialog);
        previewScrub->setObjectName(QStringLiteral("clipPreviewScrub"));
        previewScrub->setRange(0, 1000);
        previewScrub->setAccessibleName(ClipTranslation::tr("Scrub within selected moment"));
        playPreview->setCheckable(true);
        playPreview->setObjectName(QStringLiteral("clipPreviewPlay"));
        previewTime = new QLabel(ClipTranslation::tr("Preview starts at the selected in point"), dialog);
        previewTime->setTextInteractionFlags(Qt::TextSelectableByMouse);
        previewBar->addWidget(playPreview);
        previewBar->addWidget(previewAudio);
        previewBar->addWidget(previewScrub, 1);
        previewBar->addWidget(previewTime);
        root->addLayout(previewBar);
        range = new ClipRange(dialog);
        previewScrub->setEnabled(false);
        root->addWidget(range);
        auto* times = new QHBoxLayout;
        startLabel = new QLabel(dialog);
        times->addWidget(startLabel);
        start = makeTimeSpin(dialog, QStringLiteral("clipStart"));
        times->addWidget(start);
        times->addSpacing(8);
        endLabel = new QLabel(dialog);
        times->addWidget(endLabel);
        end = makeTimeSpin(dialog, QStringLiteral("clipEnd"));
        times->addWidget(end);
        times->addStretch(1);
        durationLabel = new QLabel(dialog);
        times->addWidget(durationLabel);
        root->addLayout(times);
        auto* qualityRow = new QHBoxLayout;
        qualityLabel = new QLabel(dialog);
        qualityRow->addWidget(qualityLabel);
        quality = new QComboBox(dialog);
        quality->setObjectName(QStringLiteral("clipQuality"));
        quality->addItem(ClipTranslation::tr("1080p"), 1080);
        quality->addItem(ClipTranslation::tr("720p"), 720);
        quality->addItem(ClipTranslation::tr("480p"), 480);
        qualityRow->addWidget(quality);
        qualityRow->addStretch(1);
        root->addLayout(qualityRow);
        status = new QLabel(ClipTranslation::tr("Select up to 60 seconds to export as MP4."), dialog);
        status->setObjectName(QStringLiteral("clipStatus"));
        status->setWordWrap(true);
        status->setMinimumHeight(36);
        root->addWidget(status);
        progress = new QProgressBar(dialog);
        progress->setObjectName(QStringLiteral("clipProgress"));
        progress->setRange(0, 1000);
        progress->setTextVisible(false);
        progress->setVisible(false);
        root->addWidget(progress);
        auto* resultActions = new QHBoxLayout;
        reveal = new QPushButton(ClipTranslation::tr("Show in folder"), dialog);
        copyPath = new QPushButton(ClipTranslation::tr("Copy path"), dialog);
        reveal->setVisible(false);
        copyPath->setVisible(false);
        resultActions->addWidget(reveal);
        resultActions->addWidget(copyPath);
        resultActions->addStretch(1);
        root->addLayout(resultActions);
        auto* actions = new QHBoxLayout;
        actions->addStretch(1);
        cancel = new QPushButton(ClipTranslation::tr("Cancel export"), dialog);
        cancel->setObjectName(QStringLiteral("clipCancel"));
        cancel->setVisible(false);
        exportButton = new QPushButton(ClipTranslation::tr("Export MP4…"), dialog);
        exportButton->setObjectName(QStringLiteral("clipExport"));
        exportButton->setDefault(true);
        actions->addWidget(cancel);
        actions->addWidget(exportButton);
        root->addLayout(actions);
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
    QLabel* previewTime = nullptr;
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
