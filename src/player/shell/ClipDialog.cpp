#include "ClipDialog.hpp"

#include "ClipExport.hpp"
#include "ClipSheet.hpp"
#include "FramePreviewService.hpp"
#include "ShellTheme.hpp"
#include "ShellWidgets.hpp"
#include "../core/MediaTracks.hpp"
#include "../core/VideoSurface.hpp"

#include <QCloseEvent>
#include <QClipboard>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHash>
#include <QKeyEvent>
#include <QLabel>
#include <QMessageBox>
#include <QPointer>
#include <QTimer>
#include <QPushButton>
#include <QStringList>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace mplayer::shell {
namespace {

class ClipTranslation {
    Q_DECLARE_TR_FUNCTIONS(mplayer::shell::ClipDialog)
};

/// How far around the moment the scene may be scrubbed. A whole episode on one
/// rail cannot be dragged to the frame, and the sheet is the one gerund the user
/// acts on, so the window - not the file - is what it maps.
constexpr double kWindowRadiusSeconds = 120.0;
constexpr int kThumbnailHeight = 96;

/// Bits per second an H.264 encode of this height needs in practice. Used only
/// for the printed estimate; the exporter decides the real size.
double encodeBitsPerSecond(int height) {
    if (height >= 1080) return 6.2e6;
    if (height >= 720) return 3.1e6;
    return 1.5e6;
}

QString formatSize(qint64 bytes) {
    const double mib = double(bytes) / (1024.0 * 1024.0);
    if (mib >= 1024.0) return ClipTranslation::tr("≈ %1 GB").arg(QLocale().toString(mib / 1024.0, 'f', 2));
    if (mib >= 9.95) return ClipTranslation::tr("≈ %1 MB").arg(QLocale().toString(mib, 'f', 0));
    return ClipTranslation::tr("≈ %1 MB").arg(QLocale().toString(mib, 'f', 1));
}

QString clipDialogStyleSheet() {
    const double radius = cornerRadius() > 0.0 ? cornerRadius() : 8.0;
    const QString radiusPx = QString::number(radius, 'f', 1);
    return QStringLiteral(
               "QDialog#clipDialog { background-color: #0a0a0e; color: #f4f4f5; }"
               "QFrame#clipCard { background-color: #0d0e13;"
               "  border: 1px solid rgba(255, 255, 255, 0.08); border-radius: %1px; }"
               "QLabel { color: #a1a1aa; background: transparent; font-size: 12px; }"
               "QPushButton#clipGhost { background: transparent; color: #d4d4d8; border: 1px solid"
               "  rgba(255, 255, 255, 0.12); border-radius: %1px; padding: 0px 12px;"
               "  min-height: 28px; font-size: 12px; font-weight: 500; }"
               "QPushButton#clipGhost:hover { background: rgba(255, 255, 255, 0.08); color: #ffffff; }"
               "QPushButton#clipGhost:disabled { color: rgba(255, 255, 255, 0.30); }")
        .arg(radiusPx);
}

}  // namespace

class ClipDialog::Private final : public QObject {
public:
    explicit Private(ClipDialog* dialog, VideoSurface* source, const QString& caption)
        : QObject(dialog), dialog(dialog), source(source), caption(caption) {
        build();
        if (!source || source->sourceUrl().isEmpty()) {
            unavailable(QT_TRANSLATE_NOOP("mplayer::shell::ClipDialog",
                                           "The current video source is unavailable."));
            return;
        }
        sourceUrl = source->sourceUrl();
        thumbnails.setSource(sourceUrl, source->sourceEpoch());
        connect(&thumbnails, &FramePreviewService::frameReady, this,
                [this](quint64 token, double, double, const QImage& image) {
                    onTileReady(token, image);
                });
        connect(source, &VideoSurface::mediaInformationInvalidated, this, [this] {
            invalidated = true;
            exportWorker.cancel();
            thumbnails.shutdown();
            preview->close();
            unavailable(QT_TRANSLATE_NOOP("mplayer::shell::ClipDialog",
                                          "The video changed. Close this window and open Cut moment again."));
        });
        connect(source, &VideoSurface::sourceEpochChanged, this, [this](uint64_t epoch) {
            invalidated = true;
            exportWorker.cancel();
            thumbnails.setSource(sourceUrl, epoch);
            preview->close();
            unavailable(QT_TRANSLATE_NOOP("mplayer::shell::ClipDialog",
                                          "The video changed. Close this window and open Cut moment again."));
        });
        connect(source, &VideoSurface::destroyed, this, [this] {
            invalidated = true;
            exportWorker.cancel();
            thumbnails.shutdown();
            if (preview) preview->close();
            unavailable(QT_TRANSLATE_NOOP("mplayer::shell::ClipDialog", "The video player was closed."));
        });
        connect(source, &VideoSurface::durationChanged, this, [this](double seconds) {
            if (invalidated || exporting || !std::isfinite(seconds) || seconds <= 0.0) return;
            duration = seconds;
            initializeWindow();
        });
        connect(source, &VideoSurface::videoSizeChanged, this, [this](int width, int height) {
            if (invalidated || exporting || width <= 0 || height <= 0) return;
            size = QSize(width, height);
            refreshDownload();
        });
        connect(preview, &VideoSurface::errorOccurred, this, [this](const QString& reason) {
            if (!invalidated) setExternalStatus(ClipTranslation::tr("Preview unavailable: %1").arg(reason));
        });
        connect(preview, &VideoSurface::durationChanged, this, [this](double seconds) {
            if (std::isfinite(seconds) && seconds > 0.0 && preview != nullptr) {
                preview->setPaused(true);
                preview->seek(std::clamp(sheet->selection().start, windowStart, windowEnd));
            }
        });
        connect(source, &VideoSurface::mediaTracksChanged, this, [this] { captureTracks(); });
        connect(source, &VideoSurface::streamStatsChanged, this,
                [this](double bytesPerSecond, double, StreamFpsSource) {
                    if (!std::isfinite(bytesPerSecond) || bytesPerSecond <= 0.0) return;
                    streamBytesPerSecond = bytesPerSecond;
                    refreshDownload();
                });
        tileWatchdog.setSingleShot(true);
        connect(&tileWatchdog, &QTimer::timeout, this, [this] {
            // The request is dropped, not retried: a source that cannot answer it
            // will not answer the retry either, and the strip keeps moving.
            thumbnails.cancel();
            tileInFlight = false;
            pumpTile();
        });
        connect(&thumbnails, &FramePreviewService::frameUnavailable, this,
                [this](quint64 token, double, const QString&) { finishTile(token); });
        connect(preview, &VideoSurface::mediaTracksChanged, this, [this] { syncPreviewAudio(); });
        connect(&exportWorker, &ClipExport::encodingStarted, this, [this](const QString& codec) {
            codecName = codec;
            setStatus(QT_TRANSLATE_NOOP("mplayer::shell::ClipDialog", "Encoding MP4 with %1…"), {codecName});
        });
        connect(&exportWorker, &ClipExport::progressChanged, this, [this](double fraction) {
            const double clamped = std::clamp(fraction, 0.0, 1.0);
            sheet->setProgress(clamped);
            setStatus(QT_TRANSLATE_NOOP("mplayer::shell::ClipDialog", "Encoding MP4 with %1… %2%"),
                      {codecName.isEmpty() ? ClipTranslation::tr("video") : codecName,
                       QString::number(clamped * 100.0, 'f', 1)});
        });
        connect(&exportWorker, &ClipExport::finished, this,
                [this](bool succeeded, const QString& path, const QString& error) {
            exporting = false;
            sheet->setExporting(false);
            sheet->setProgress(-1.0);
            cancel->setVisible(false);
            sheet->setEnabledForSource(!invalidated);
            qualityHeight = sheet->quality();
            if (!succeeded) {
                if (error.isEmpty()) setStatus(QT_TRANSLATE_NOOP("mplayer::shell::ClipDialog", "Export failed."));
                else setExternalStatus(error);
                exportButtonState = ExportButtonState::Retry;
                retranslateUi();
                return;
            }
            completedPath = path;
            setStatus(QT_TRANSLATE_NOOP("mplayer::shell::ClipDialog", "Saved %1"),
                      {QFileInfo(path).fileName()});
            exportButtonState = ExportButtonState::Another;
            retranslateUi();
            reveal->setVisible(true);
            copyPath->setVisible(true);
        });
        captureTracks();
        const QSize sourceSize(source->videoWidth(), source->videoHeight());
        if (sourceSize.isValid()) size = sourceSize;
        if (std::isfinite(source->duration()) && source->duration() > 0.0) {
            duration = source->duration();
        }
        initializeWindow();
        preview->setMuted(true);
        preview->open(sourceUrl);
        preview->setPaused(true);
        retranslateUi();
    }

    ~Private() override {
        tileWatchdog.stop();
        exportWorker.cancel();
        thumbnails.shutdown();
    }

    enum class ExportButtonState { Default, Retry, Another };

    void retranslateUi() {
        dialog->setWindowTitle(caption.trimmed().isEmpty()
                                   ? ClipTranslation::tr("Cut moment")
                                   : ClipTranslation::tr("Cut moment — %1").arg(caption));
        dialog->setStyleSheet(clipDialogStyleSheet());
        cancel->setText(ClipTranslation::tr("Cancel"));
        reveal->setText(ClipTranslation::tr("Show in folder"));
        copyPath->setText(ClipTranslation::tr("Copy path"));
        refreshDownload();
        if (!statusSource.isEmpty()) {
            QString text = ClipTranslation::tr(statusSource.toUtf8().constData());
            for (const QString& arg : statusArgs) text = text.arg(arg);
            setExternalStatus(text);
        }
    }

    /// The one thing the action button says, derived from the state it is in:
    /// the estimate while idle, progress while working, and the two follow-ups
    /// after an export.
    void refreshDownload() {
        if (sheet == nullptr) return;
        if (exporting) return;
        const double length = std::max(0.0, sheet->selection().length());
        const int height = sheet->quality() > 0 ? sheet->quality() : 1080;
        const qint64 bytes = qint64(estimatedBitsPerSecond(height) * length / 8.0);
        const QString size = length > 0.0 ? formatSize(bytes) : QString();
        const bool ready = validRange() && !invalidated;
        sheet->setDownloadLabel(
            exportButtonState == ExportButtonState::Retry ? ClipTranslation::tr("Try again")
            : exportButtonState == ExportButtonState::Another ? ClipTranslation::tr("Export another")
                                                              : ClipTranslation::tr("Download %1").arg(size),
            ready);
        sheet->setPlayEnabled(!invalidated && preview != nullptr);
    }

    void setStatus(const char* sourceText, std::initializer_list<QString> args = {}) {
        statusSource = QString::fromUtf8(sourceText);
        statusArgs = QStringList(args);
        QString text = ClipTranslation::tr(sourceText);
        for (const QString& arg : statusArgs) text = text.arg(arg);
        if (sheet != nullptr) sheet->setStatus(text);
    }

    void setExternalStatus(const QString& text) {
        statusSource.clear();
        statusArgs.clear();
        if (sheet != nullptr) sheet->setStatus(text);
    }

    bool handleKeyPress(QKeyEvent* event) {
        if (event->key() == Qt::Key_Space) {
            togglePlay();
            return true;
        }
        if (event->key() == Qt::Key_BracketLeft) {
            setStartToCurrent();
            return true;
        }
        if (event->key() == Qt::Key_BracketRight) {
            setEndToCurrent();
            return true;
        }
        if (event->key() == Qt::Key_Left) {
            stepSeconds(event->modifiers().testFlag(Qt::ShiftModifier) ? -0.1 : -1.0);
            return true;
        }
        if (event->key() == Qt::Key_Right) {
            stepSeconds(event->modifiers().testFlag(Qt::ShiftModifier) ? 0.1 : 1.0);
            return true;
        }
        if (event->key() == Qt::Key_Escape) return false;
        return false;
    }

    void build() {
        dialog->setObjectName(QStringLiteral("clipDialog"));
        dialog->setWindowTitle(ClipTranslation::tr("Cut moment"));
        dialog->setModal(false);
        dialog->setMinimumWidth(760);
        dialog->resize(980, 720);
        dialog->setWindowFlag(Qt::WindowContextHelpButtonHint, false);

        const double radius = cornerRadius() > 0.0 ? cornerRadius() : 8.0;

        auto* root = new QVBoxLayout(dialog);
        root->setContentsMargins(14, 14, 14, 12);
        root->setSpacing(10);

        previewCard = new QFrame(dialog);
        previewCard->setObjectName(QStringLiteral("clipCard"));
        auto* previewLayout = new QVBoxLayout(previewCard);
        previewLayout->setContentsMargins(8, 8, 8, 8);
        previewLayout->setSpacing(0);

        preview = new VideoSurface(previewCard);
        preview->setObjectName(QStringLiteral("clipPreview"));
        preview->setMinimumHeight(240);
        preview->setDisplayCornerRadius(qMax(4.0, radius));
        preview->setFocusPolicy(Qt::StrongFocus);
        previewLayout->addWidget(preview, 1);
        root->addWidget(previewCard, 1);

        sheet = new ClipSheet(dialog);
        sheet->setMaximumSpan(kMaximumClipDuration);
        sheet->setQualityOptions({1080, 720, 480}, 1080);
        root->addWidget(sheet);

        auto* footer = new QHBoxLayout;
        footer->setContentsMargins(2, 0, 2, 0);
        footer->setSpacing(8);
        progressRow = new QWidget(dialog);
        auto* footerButtons = new QHBoxLayout(progressRow);
        footerButtons->setContentsMargins(0, 0, 0, 0);
        footerButtons->setSpacing(8);
        reveal = new QPushButton(progressRow);
        reveal->setObjectName(QStringLiteral("clipGhost"));
        reveal->setVisible(false);
        copyPath = new QPushButton(progressRow);
        copyPath->setObjectName(QStringLiteral("clipGhost"));
        copyPath->setVisible(false);
        cancel = new QPushButton(progressRow);
        cancel->setObjectName(QStringLiteral("clipGhost"));
        cancel->setVisible(false);
        footerButtons->addWidget(reveal);
        footerButtons->addWidget(copyPath);
        footerButtons->addStretch(1);
        footerButtons->addWidget(cancel);
        root->addWidget(progressRow);

        connect(sheet, &ClipSheet::selectionChanged, this,
                [this](double start, double end) { setTimes(start, end, 0); });
        connect(sheet, &ClipSheet::playToggled, this, [this] { togglePlay(); });
        connect(sheet, &ClipSheet::qualityChanged, this, [this](int height) {
            qualityHeight = height;
            refreshDownload();
        });
        connect(sheet, &ClipSheet::downloadRequested, this, [this] { chooseOutput(); });
        connect(sheet, &ClipSheet::closeRequested, this, [this] { dialog->close(); });
        connect(sheet, &ClipSheet::seekRequested, this, [this](double seconds) {
            if (preview && !invalidated) {
                preview->setPaused(true);
                preview->seek(std::clamp(seconds, windowStart, windowEnd));
                setPlaying(false);
            }
        });
        connect(sheet, &ClipSheet::durationPresetRequested, this,
                [this](int seconds) { applyDurationPreset(double(seconds)); });
        connect(sheet, &ClipSheet::boundaryFromPlayheadRequested, this, [this](bool end) {
            if (end) setEndToCurrent();
            else setStartToCurrent();
        });
        connect(sheet, &ClipSheet::tilesNeeded, this, [this](int) { requestTiles(); });

        connect(preview, &VideoSurface::positionChanged, this, [this](double position) {
            previewPosition = position;
            previewPositionKnown = true;
            if (sheet != nullptr) sheet->setPlayhead(position);
            // The selection is a playback loop, not a cage: it only pulls the
            // preview back while the preview is *playing*. A playhead the user
            // placed after the selection stays there, or the moment they want to
            // inspect could never be reached.
            const bool playing = playingState || (preview != nullptr && preview->isPlaying());
            if (playing && duration > 0.0 && position >= sheet->selection().end - 0.02) {
                preview->setPaused(true);
                preview->seek(sheet->selection().start);
                setPlaying(false);
            }
        });

        connect(cancel, &QPushButton::clicked, this, [this] {
            setStatus(QT_TRANSLATE_NOOP("mplayer::shell::ClipDialog", "Canceling export…"));
            cancel->setEnabled(false);
            exportWorker.cancel();
        });
        connect(reveal, &QPushButton::clicked, this, [this] {
            if (!completedPath.isEmpty())
                QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(completedPath).absolutePath()));
        });
        connect(copyPath, &QPushButton::clicked, this, [this] {
            if (!completedPath.isEmpty()) QGuiApplication::clipboard()->setText(completedPath);
        });
        // The sheet restyles itself; the dialog's own chrome (ghost buttons)
        // carries the radius, so it has to follow the accent change too.
        connect(&accentNotifier(), &AccentNotifier::changed, this, [this] {
            dialog->setStyleSheet(clipDialogStyleSheet());
        });
        retranslateUi();
    }

    /// What this clip will weigh, in bits per second.
    ///
    /// The exporter re-encodes at a constant quality, so no exact size exists in
    /// advance. The best available predictor is the rate the source itself
    /// delivers at: it carries the content's complexity, and a lower height
    /// scales it down in proportion to the height table. The table is also the
    /// ceiling, because a clip never promises more than its encode target.
    double estimatedBitsPerSecond(int height) const {
        const double ceiling = encodeBitsPerSecond(height);
        if (!(streamBytesPerSecond > 0.0)) return ceiling;
        const double source = streamBytesPerSecond * 8.0;
        const double scale = std::min(1.0, ceiling / encodeBitsPerSecond(1080));
        return std::min(ceiling, source * scale);
    }

    void setPlaying(bool playing) {
        playingState = playing;
        if (sheet != nullptr) sheet->setPlaying(playing);
    }

    void togglePlay() {
        if (!preview || invalidated) return;
        const bool willPlay = !preview->isPlaying();
        preview->setPaused(!willPlay);
        setPlaying(willPlay);
    }

    void stepSeconds(double delta) {
        if (!preview || invalidated || duration <= 0.0) return;
        const double target = std::clamp(previewPosition + delta, windowStart, windowEnd);
        preview->seek(target);
    }

    void setStartToCurrent() {
        if (duration <= 0.0 || invalidated || !sheet) return;
        const double current = std::clamp(previewPosition, windowStart, windowEnd);
        setTimes(current, sheet->selection().end, 1);
    }

    void setEndToCurrent() {
        if (duration <= 0.0 || invalidated || !sheet) return;
        const double current = std::clamp(previewPosition, windowStart, windowEnd);
        setTimes(sheet->selection().start, current, 2);
    }

    void applyDurationPreset(double seconds) {
        if (duration <= 0.0 || invalidated || !sheet) return;
        const double currentStart = sheet->selection().start;
        double targetEnd = currentStart + seconds;
        if (targetEnd > windowEnd) {
            targetEnd = windowEnd;
            setTimes(std::max(windowStart, targetEnd - seconds), targetEnd, 0);
        } else {
            setTimes(currentStart, targetEnd, 2);
        }
    }

    /// The ±2 minute window around the moment the viewer opened the sheet. The
    /// selection starts as a ten-second bite around that moment, and nothing can
    /// be dragged outside the slice.
    void initializeWindow() {
        if (invalidated || exporting || !std::isfinite(duration) || duration <= 0.0) return;
        const double center = source ? std::clamp(source->position(), 0.0, duration) : 0.0;
        windowStart = std::max(0.0, center - kWindowRadiusSeconds);
        windowEnd = std::min(duration, windowStart + 2.0 * kWindowRadiusSeconds);
        if (windowEnd - windowStart < 1.0) {
            windowStart = 0.0;
            windowEnd = duration;
        }
        windowInitialized = true;
        sheet->setWindow(windowStart, windowEnd);
        sheet->setEnabledForSource(true);
        const double a = std::clamp(center - 5.0, windowStart, std::max(windowStart, windowEnd - 0.2));
        const double b = std::min(windowEnd, a + 10.0);
        setTimes(a, b, 0);
        requestTiles();
        preview->seek(a);
        setStatus(QT_TRANSLATE_NOOP("mplayer::shell::ClipDialog",
                                    "Drag the selection, then download. The window follows the moment you opened."));
    }

    /// One frame per strip slot, from the decoder service rather than the
    /// preview surface: seeking the visible surface for ten thumbnails would
    /// make the preview stutter and would move the playhead under the user.
    ///
    /// Requested one at a time. The service answers the newest request and drops
    /// the rest, so ten requests in a row would leave exactly one frame on the
    /// strip - the last one - which is what a strip full of empty plates was.
    void requestTiles() {
        if (invalidated || !windowInitialized || windowEnd <= windowStart) return;
        const int count = sheet->tileCount();
        if (count <= 0) return;
        tileQueue.clear();
        tileSlots.clear();
        tileInFlight = false;
        tileWatchdog.stop();
        thumbnails.cancel();
        // Every slot is cleared with the new geometry: slots are a mapping, and
        // leaving an old frame at a new index would label the wrong instant.
        sheet->setTiles(QVector<QPixmap>(count));
        const double span = windowEnd - windowStart;
        const double anchor = sheet->selection().start;
        QVector<QPair<int, double>> waiting;
        for (int i = 0; i < count; ++i) {
            const double seconds = windowStart + span * (i + 0.5) / count;
            const auto cached = tileCache.constFind(tileKey(seconds));
            if (cached != tileCache.constEnd()) {
                sheet->setTile(i, *cached);
            } else {
                waiting.append({i, seconds});
            }
        }
        // The frames around the moment being judged are the ones the choice is
        // made from, so they are decoded first; the rest follow while the user
        // is still deciding.
        std::sort(waiting.begin(), waiting.end(),
                  [anchor](const QPair<int, double>& left, const QPair<int, double>& right) {
                      return std::abs(left.second - anchor) < std::abs(right.second - anchor);
                  });
        tileQueue = waiting;
        pumpTile();
    }

    /// Half-second buckets, the same granularity the service's own store uses,
    /// so a resize or a reopen reuses what is already decoded.
    static qint64 tileKey(double seconds) { return qint64(std::llround(seconds * 2.0)); }

    void pumpTile() {
        if (tileInFlight || tileQueue.isEmpty() || invalidated) return;
        const auto next = tileQueue.takeFirst();
        const quint64 token = ++tileToken;
        tileSlots.insert(token, next.first);
        tileTimes.insert(token, next.second);
        tileInFlight = true;
        // A frame the service never answers must not stall the strip forever; the
        // slot stays a plate and the next one is asked for.
        tileWatchdog.start(3000);
        thumbnails.requestFrame(token, next.second);
    }

    void finishTile(quint64 token) {
        if (!tileInFlight) return;
        tileSlots.remove(token);
        tileInFlight = false;
        tileWatchdog.stop();
        pumpTile();
    }

    void onTileReady(quint64 token, const QImage& image) {
        if (invalidated || image.isNull()) {
            finishTile(token);
            return;
        }
        const auto slot = tileSlots.constFind(token);
        if (slot == tileSlots.constEnd()) return;
        const int index = *slot;
        const double seconds = tileTimes.take(token);
        finishTile(token);
        const QSize target(kThumbnailHeight * 16 / 9, kThumbnailHeight);
        const QPixmap tile = QPixmap::fromImage(image.scaled(target, Qt::KeepAspectRatioByExpanding,
                                                             Qt::SmoothTransformation));
        sheet->setTile(index, tile);
        storeTile(tileKey(seconds), tile);
    }

    void storeTile(qint64 key, const QPixmap& tile) {
        if (key < 0 || tile.isNull()) return;
        if (tileCache.contains(key)) return;
        // Bounded: a strip sweep is ten frames, and a session that changes the
        // window a few times still fits in a few dozen.
        while (tileCacheOrder.size() >= 48) tileCache.remove(tileCacheOrder.takeFirst());
        tileCache.insert(key, tile);
        tileCacheOrder.append(key);
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
                preview->selectMediaTrack(MediaTrackType::Audio, *audioTrack, snapshot.coreIdentity,
                                          snapshot.sourceEpoch);
                return;
            }
        }
    }

    void setTimes(double a, double b, int editedField) {
        if (duration <= 0.0 || exporting || invalidated || !windowInitialized || !sheet) return;
        a = std::clamp(a, windowStart, windowEnd);
        b = std::clamp(b, windowStart, windowEnd);
        if (b <= a) {
            if (a >= windowEnd) a = std::max(windowStart, windowEnd - 0.001);
            else b = std::min(windowEnd, a + 0.001);
        }
        if (b - a > kMaximumClipDuration) {
            if (editedField == 1) b = std::min(windowEnd, a + kMaximumClipDuration);
            else if (editedField == 2) a = std::max(windowStart, b - kMaximumClipDuration);
            else b = a + kMaximumClipDuration;
        }
        syncing = true;
        sheet->setSelection(a, b);
        syncing = false;
        refreshDownload();
        // The preview keeps its own position: dragging a boundary must not move
        // the playhead, or the moment being judged changes while it is judged.
    }

    bool validRange() const {
        if (!windowInitialized || exporting || invalidated || !sheet || !size.isValid()) return false;
        const ClipSheet::Range selection = sheet->selection();
        return selection.length() > 0.0 && selection.length() <= kMaximumClipDuration + 0.001 &&
               selection.start >= windowStart - 0.001 && selection.end <= windowEnd + 0.001;
    }

    void chooseOutput() {
        if (exporting || !validRange() || !source) return;
        QPointer<Private> self(this);
        QPointer<ClipDialog> guardedDialog(dialog);
        QPointer<VideoSurface> guardedSource(source);
        const uint64_t sourceEpoch = source->sourceEpoch();
        const QString selectedSource = sourceUrl;
        const ClipSheet::Range selectedRange = sheet->selection();
        const double selectedStart = selectedRange.start;
        const double selectedEnd = selectedRange.end;
        const QSize selectedSize = size;
        const int selectedHeight = sheet->quality() > 0 ? sheet->quality() : 1080;
        const std::optional<qint64> selectedAudioTrack = audioTrack;
        const QString selectedExternalAudio = externalAudio;
        QString suggestedName = QFileInfo(QUrl(selectedSource).path()).completeBaseName();
        if (suggestedName.isEmpty()) suggestedName = ClipTranslation::tr("clip");
        suggestedName += QStringLiteral(".mp4");
        QString path = QFileDialog::getSaveFileName(dialog, ClipTranslation::tr("Save clip"), suggestedName,
                                                    ClipTranslation::tr("MP4 video (*.mp4)"), nullptr,
                                                    QFileDialog::DontConfirmOverwrite);
        if (!self || !guardedDialog || !guardedSource) return;
        if (invalidated || guardedSource->sourceEpoch() != sourceEpoch || !validRange() ||
            sourceUrl != selectedSource) return;
        if (path.isEmpty()) return;
        if (!path.endsWith(QStringLiteral(".mp4"), Qt::CaseInsensitive)) path += QStringLiteral(".mp4");
        bool replaceExisting = false;
        if (QFileInfo::exists(path)) {
            const auto answer = QMessageBox::question(
                dialog, ClipTranslation::tr("Replace existing file?"),
                ClipTranslation::tr("%1 already exists. Replace it after the new clip is complete?")
                    .arg(QFileInfo(path).fileName()),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (!self || !guardedDialog || !guardedSource) return;
            if (invalidated || guardedSource->sourceEpoch() != sourceEpoch || !validRange() ||
                sourceUrl != selectedSource) return;
            if (answer != QMessageBox::Yes) return;
            replaceExisting = true;
        }
        if (invalidated || guardedSource->sourceEpoch() != sourceEpoch || !validRange() ||
            sourceUrl != selectedSource || sheet->selection().start != selectedStart ||
            sheet->selection().end != selectedEnd || size != selectedSize ||
            sheet->quality() != selectedHeight) {
            return;
        }
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
        sheet->setProgress(0.0);
        sheet->setExporting(true);
        cancel->setVisible(true);
        cancel->setEnabled(true);
        exportButtonState = ExportButtonState::Default;
        setStatus(QT_TRANSLATE_NOOP("mplayer::shell::ClipDialog", "Preparing export…"));
        exportWorker.start(request);
    }

    void unavailable(const char* sourceText) {
        setStatus(sourceText);
        sheet->setEnabledForSource(false);
        refreshDownload();
    }

    ClipDialog* dialog;
    QPointer<VideoSurface> source;
    QString caption;
    VideoSurface* preview = nullptr;
    QFrame* previewCard = nullptr;
    ClipSheet* sheet = nullptr;
    ClipExport exportWorker;
    FramePreviewService thumbnails;
    QWidget* progressRow = nullptr;
    QPushButton* cancel = nullptr;
    QPushButton* reveal = nullptr;
    QPushButton* copyPath = nullptr;
    QHash<quint64, int> tileSlots;
    QVector<QPair<int, double>> tileQueue;
    QHash<qint64, QPixmap> tileCache;
    QVector<qint64> tileCacheOrder;
    QHash<quint64, double> tileTimes;
    QTimer tileWatchdog;
    quint64 tileToken = 0;
    bool tileInFlight = false;
    double streamBytesPerSecond = 0.0;
    QString sourceUrl;
    QString externalAudio;
    QString completedPath;
    std::optional<qint64> audioTrack;
    QSize size;
    double duration = 0.0;
    double windowStart = 0.0;
    double windowEnd = 0.0;
    double previewPosition = 0.0;
    int qualityHeight = 1080;
    QString codecName;
    QString statusSource;
    QStringList statusArgs;
    ExportButtonState exportButtonState = ExportButtonState::Default;
    bool previewPositionKnown = false;
    bool syncing = false;
    bool exporting = false;
    bool windowInitialized = false;
    bool previewExternalAudioAdded = false;
    bool invalidated = false;
    bool playingState = false;
};

ClipDialog::ClipDialog(VideoSurface* source, const QString& caption, QWidget* parent)
    : QDialog(parent), d_(new Private(this, source, caption)) {}

void ClipDialog::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange && d_) d_->retranslateUi();
    QDialog::changeEvent(event);
}

void ClipDialog::keyPressEvent(QKeyEvent* event) {
    if (d_ && d_->handleKeyPress(event)) {
        event->accept();
        return;
    }
    QDialog::keyPressEvent(event);
}

ClipDialog::~ClipDialog() = default;

void ClipDialog::closeEvent(QCloseEvent* event) {
    if (d_) {
        if (d_->preview) d_->preview->setPaused(true);
        if (d_->exporting) {
            d_->exportWorker.cancel();
            d_->setStatus(QT_TRANSLATE_NOOP("mplayer::shell::ClipDialog", "Canceling export…"));
        }
    }
    QDialog::closeEvent(event);
}

}  // namespace mplayer::shell
