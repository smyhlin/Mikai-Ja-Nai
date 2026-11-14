#include "ClipController.hpp"
#include "../shell/ShellTheme.hpp"

#include <QClipboard>
#include <QCoreApplication>
#include <QDebug>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLocale>
#include <QMutex>
#include <QQuickImageProvider>
#include <QStandardPaths>

#include <algorithm>
#include <cmath>
#include <utility>

namespace mplayer::quick {
namespace {
QString clipTime(double seconds) {
    const qint64 tenths = qRound64(std::max(0.0, seconds) * 10.0);
    const qint64 whole = tenths / 10;
    const QString tail = QStringLiteral("%1:%2.%3").arg(whole / 60 % 60, 2, 10, QChar('0'))
                             .arg(whole % 60, 2, 10, QChar('0')).arg(tenths % 10);
    return whole >= 3600 ? QStringLiteral("%1:%2").arg(whole / 3600).arg(tail) : tail;
}

double bitrateForHeight(int height) {
    return height >= 1080 ? 6.2e6 : height >= 720 ? 3.1e6 : 1.5e6;
}
}

struct ClipFrameStore {
    struct Frame { QImage image; quint64 revision = 0; };
    QMutex mutex;
    QVector<Frame> tiles;
    Frame still;
};

namespace {
class ClipFrameProvider final : public QQuickImageProvider {
public:
    explicit ClipFrameProvider(std::shared_ptr<ClipFrameStore> store)
        : QQuickImageProvider(QQuickImageProvider::Image), store_(std::move(store)) {}

    QImage requestImage(const QString& id, QSize* size, const QSize&) override {
        const QStringList parts = id.split(QLatin1Char('/'));
        if (parts.size() != 3) return {};
        bool rowOk = false;
        bool revisionOk = false;
        const int row = parts.at(1).toInt(&rowOk);
        const quint64 revision = parts.at(2).toULongLong(&revisionOk);
        if (!rowOk || !revisionOk || revision == 0) return {};
        QMutexLocker lock(&store_->mutex);
        const ClipFrameStore::Frame* frame = nullptr;
        if (parts.first() == QLatin1String("still")) frame = &store_->still;
        else if (parts.first() == QLatin1String("tile") && row >= 0 && row < store_->tiles.size())
            frame = &store_->tiles.at(row);
        if (frame == nullptr || frame->revision != revision) return {};
        if (size != nullptr) *size = frame->image.size();
        return frame->image;
    }

private:
    std::shared_ptr<ClipFrameStore> store_;
};
}

ClipThumbnailModel::ClipThumbnailModel(QObject* parent) : QAbstractListModel(parent) {}
int ClipThumbnailModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}
QVariant ClipThumbnailModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= rows_.size()) return {};
    const auto& row = rows_.at(index.row());
    if (role == ImageSourceRole) return row.source;
    if (role == TimeRole) return row.time;
    return {};
}
QHash<int, QByteArray> ClipThumbnailModel::roleNames() const {
    return {{ImageSourceRole, "imageSource"}, {TimeRole, "timestamp"}};
}
void ClipThumbnailModel::reset(int count, double start, double end) {
    beginResetModel();
    rows_.clear();
    rows_.reserve(count);
    for (int row = 0; row < count; ++row)
        rows_.append(Thumbnail{start + (end - start) * (row + 0.5) / count, {}});
    endResetModel();
}
void ClipThumbnailModel::setImage(int row, const QUrl& source) {
    if (row < 0 || row >= rows_.size()) return;
    rows_[row].source = source;
    emit dataChanged(index(row), index(row), {ImageSourceRole});
}
double ClipThumbnailModel::timeAt(int row) const { return rows_.at(row).time; }

ClipController::ClipController(VideoSession* source, QString caption, QObject* parent)
    : QObject(parent), source_(source), preview_(new VideoSession(this)),
      caption_(std::move(caption)), frameStore_(std::make_shared<ClipFrameStore>()) {
    preview_->setDiagnosticsPolling(false);
    preview_->setPaused(true);
    preview_->setMuted(false);
    connect(&shell::accentNotifier(), &shell::AccentNotifier::changed,
            this, &ClipController::themeChanged);
    thumbnailTimer_.setSingleShot(true);
    stillTimer_.setSingleShot(true);
    connect(&thumbnailTimer_, &QTimer::timeout, this, &ClipController::rebuildThumbnails);
    connect(&stillTimer_, &QTimer::timeout, this, &ClipController::queueStill);
    connect(&frames_, &shell::FramePreviewService::frameReady, this,
            [this](quint64 token, double, double decoded, const QImage& frame) {
                finishThumbnail(token, decoded, frame);
            });
    connect(&frames_, &shell::FramePreviewService::frameUnavailable, this,
            [this](quint64 token, double, const QString& reason) {
                finishThumbnail(token, 0.0, {}, reason);
            });
    connect(preview_, &VideoSession::mediaLoaded, this, [this](quint64) {
        if (invalidated_ || stopped_) return;
        previewLoaded_ = true;
        preview_->seekExact(pendingSeek_);
        preview_->setPaused(!playing_);
        syncPreviewAudio();
    });
    connect(preview_, &VideoSession::mediaTracksChanged, this, &ClipController::syncPreviewAudio);
    connect(preview_, &VideoSession::positionChanged, this, [this](double seconds) {
        if (invalidated_ || stopped_ || !previewLoaded_ || !std::isfinite(seconds)) return;
        if (seeking_) {
            if (std::abs(seconds - pendingSeek_) > 0.35) return;
            seeking_ = false;
        }
        position_ = std::clamp(seconds, windowStart(), windowEnd());
        emit positionChanged();
        if (playing_ && seconds >= selectionEnd() - 0.02) {
            pendingSeek_ = selectionStart();
            seeking_ = true;
            preview_->seekExact(pendingSeek_);
        }
    });
    connect(preview_, &VideoSession::mediaEnded, this, [this](quint64) {
        if (!playing_ || invalidated_ || stopped_) return;
        pendingSeek_ = selectionStart();
        seeking_ = true;
        preview_->seekExact(pendingSeek_);
        preview_->setPaused(false);
    });
    connect(preview_, &VideoSession::errorOccurred, this, [this](const QString& reason) {
        if (invalidated_ || stopped_) return;
        playing_ = false;
        previewLoading_ = false;
        previewOpened_ = false;
        previewLoaded_ = false;
        setStatus(QT_TR_NOOP("Preview unavailable: %1"), {reason});
        emit previewChanged();
    });
    connect(&exporter_, &shell::ClipExport::encodingStarted, this, [this](const QString& codec) {
        if (stopped_ || invalidated_) return;
        codec_ = codec;
        setStatus(QT_TR_NOOP("Encoding MP4 with %1…"), {codec});
    });
    connect(&exporter_, &shell::ClipExport::progressChanged, this, [this](double fraction) {
        if (stopped_ || invalidated_) return;
        progress_ = std::clamp(fraction, 0.0, 1.0);
        emit stateChanged();
    });
    connect(&exporter_, &shell::ClipExport::finished, this,
            [this](bool ok, const QString& path, const QString& reason) {
        if (stopped_ || invalidated_) return;
        const bool cancelled = exportState_ == ExportState::Cancelling;
        pendingExport_.reset();
        if (ok) {
            completedPath_ = path;
            progress_ = 1.0;
            exportState_ = ExportState::Saved;
            setStatus(QT_TR_NOOP("Saved %1"), {QFileInfo(path).fileName()});
        } else if (cancelled && reason.isEmpty()) {
            exportState_ = ExportState::Editing;
            setStatus(QT_TR_NOOP("Export cancelled."));
        } else {
            exportState_ = ExportState::Failed;
            if (reason.isEmpty()) setStatus(QT_TR_NOOP("Export failed."));
            else setExternalError(reason);
        }
        emit stateChanged();
    });
    if (source == nullptr || source->sourceUrl().isEmpty()) {
        invalidate(QT_TR_NOOP("The current video source is unavailable."));
        return;
    }
    sourceUrl_ = source->sourceUrl();
    sourceEpoch_ = source->sourceEpoch();
    anchor_ = std::isfinite(source->position()) ? source->position() : 0.0;
    sourceSize_ = QSize(source->videoWidth(), source->videoHeight());
    frames_.setSource(sourceUrl_, sourceEpoch_);
    connect(source, &VideoSession::mediaInformationInvalidated, this, [this] {
        invalidate(QT_TR_NOOP("The video changed. Close this window and open Cut moment again."));
    });
    connect(source, &VideoSession::sourceEpochChanged, this, [this](uint64_t) {
        invalidate(QT_TR_NOOP("The video changed. Close this window and open Cut moment again."));
    });
    connect(source, &QObject::destroyed, this, [this] {
        invalidate(QT_TR_NOOP("The video player was closed."));
    });
    connect(source, &VideoSession::durationChanged, this, &ClipController::initializeSelection);
    connect(source, &VideoSession::videoSizeChanged, this, [this](int width, int height) {
        if (invalidated_ || stopped_ || width <= 0 || height <= 0) return;
        sourceSize_ = QSize(width, height);
        emit selectionChanged();
        emit stateChanged();
    });
    connect(source, &VideoSession::streamStatsChanged, this,
            [this](double bytes, double, StreamFpsSource) {
        if (!std::isfinite(bytes) || bytes <= 0.0) return;
        streamBytesPerSecond_ = bytes;
        emit selectionChanged();
    });
    connect(source, &VideoSession::mediaTracksChanged, this, &ClipController::captureTracks);
    captureTracks();
    initializeSelection(source->duration());
}

ClipController::~ClipController() { shutdown(); }
QString ClipController::startText() const { return clipTime(selectionStart()); }
QString ClipController::endText() const { return clipTime(selectionEnd()); }
QString ClipController::windowStartText() const { return clipTime(windowStart()); }
QString ClipController::windowEndText() const { return clipTime(windowEnd()); }
QString ClipController::positionText() const { return clipTime(position_); }
QColor ClipController::accentInk() const { return shell::inkOn(shell::accent()); }
QQuickImageProvider* ClipController::createImageProvider() const { return new ClipFrameProvider(frameStore_); }
bool ClipController::busy() const {
    return exportState_ == ExportState::Encoding || exportState_ == ExportState::Cancelling;
}
bool ClipController::choosingOutput() const { return exportState_ == ExportState::Choosing; }
bool ClipController::overwritePending() const { return exportState_ == ExportState::Confirming; }
bool ClipController::failed() const { return invalidated_ || exportState_ == ExportState::Failed; }
bool ClipController::saved() const { return exportState_ == ExportState::Saved; }
bool ClipController::cancelPending() const { return exportState_ == ExportState::Cancelling; }
bool ClipController::editingEnabled() const {
    return !invalidated_ && !stopped_ && selection_.valid() && !busy() &&
           !choosingOutput() && !overwritePending();
}
bool ClipController::canExport() const { return editingEnabled() && sourceSize_.isValid(); }
QString ClipController::overwriteFileName() const { return QFileInfo(outputPath_).fileName(); }
QString ClipController::estimatedSize() const {
    const double ceiling = bitrateForHeight(qualityHeight_);
    const double source = streamBytesPerSecond_ * 8.0;
    const double bitrate = source > 0.0
        ? std::min(ceiling, source * std::min(1.0, ceiling / bitrateForHeight(1080))) : ceiling;
    return tr("≈ %1").arg(QLocale().formattedDataSize(qRound64(bitrate * selectionDuration() / 8.0)));
}
QString ClipController::statusText() const {
    if (!externalError_.isEmpty()) return externalError_;
    QString text = statusSource_.isEmpty() ? QString() : tr(statusSource_.constData());
    for (const auto& argument : statusArguments_) text = text.arg(argument);
    return text;
}
void ClipController::setStatus(const char* message, QStringList arguments) {
    statusSource_ = message;
    statusArguments_ = std::move(arguments);
    externalError_.clear();
    emit stateChanged();
}
void ClipController::setExternalError(const QString& message) {
    externalError_ = message;
    statusSource_.clear();
    statusArguments_.clear();
    emit stateChanged();
}
void ClipController::retranslate() {
    emit selectionChanged();
    emit positionChanged();
    emit stateChanged();
}
void ClipController::initializeSelection(double duration) {
    if (invalidated_ || stopped_ || selection_.valid()) return;
    const auto initial = initialClipSelection(duration, anchor_);
    if (!initial.valid()) return;
    selection_ = initial;
    position_ = initial.start;
    emit selectionChanged();
    emit positionChanged();
    emit stateChanged();
    stillRequestedTime_ = position_;
    stillTimer_.start(0);
    thumbnailTimer_.start(100);
    setStatus(QT_TR_NOOP("Drag the handles to trim. Drag between them to move the selection."));
}
void ClipController::applySelection(const ClipSelection& selection) {
    if (!selection.valid()) return;
    selection_ = selection;
    emit selectionChanged();
    emit stateChanged();
}
void ClipController::setSelection(double start, double end, int editedBoundary) {
    if (!editingEnabled() || editedBoundary < 0 || editedBoundary > 2) return;
    applySelection(boundedClipSelection(selection_, start, end, static_cast<ClipBoundary>(editedBoundary)));
}
void ClipController::moveSelection(double originalStart, double delta) {
    if (!editingEnabled() || !std::isfinite(originalStart) || !std::isfinite(delta)) return;
    applySelection(shiftedClipSelection(selection_, originalStart + delta - selection_.start));
}
void ClipController::setStartToCurrent() { setSelection(position_, selectionEnd(), 1); }
void ClipController::setEndToCurrent() { setSelection(selectionStart(), position_, 2); }
void ClipController::applyDurationPreset(double seconds) {
    if (!editingEnabled() || !std::isfinite(seconds) || seconds < 0.1 ||
        seconds > shell::kMaximumClipDuration) return;
    const double length = std::min(seconds, windowEnd() - windowStart());
    const double start = std::min(selectionStart(), windowEnd() - length);
    applySelection(boundedClipSelection(selection_, start, start + length));
}
void ClipController::setQualityHeight(int height) {
    if (!editingEnabled() || (height != 480 && height != 720 && height != 1080) ||
        height == qualityHeight_) return;
    qualityHeight_ = height;
    emit selectionChanged();
    emit stateChanged();
}
void ClipController::pausePreview() {
    playing_ = false;
    preview_->setPaused(true);
    emit previewChanged();
}
void ClipController::beginTimelineGesture() { if (editingEnabled()) pausePreview(); }
void ClipController::seekTo(double seconds) {
    if (!editingEnabled() || !std::isfinite(seconds)) return;
    pausePreview();
    position_ = std::clamp(seconds, windowStart(), windowEnd());
    pendingSeek_ = position_;
    seeking_ = previewOpened_;
    emit positionChanged();
    if (previewOpened_) {
        if (previewLoaded_) preview_->seekExact(position_);
    } else {
        stillRequestedTime_ = position_;
        stillTimer_.start(60);
    }
}
void ClipController::stepSeconds(double delta) {
    if (std::isfinite(delta)) seekTo(position_ + delta);
}
void ClipController::toggleMute() {
    if (!editingEnabled()) return;
    muted_ = !muted_;
    preview_->setMuted(muted_);
    emit previewChanged();
}
void ClipController::togglePlay() {
    if (!editingEnabled()) return;
    if (playing_) { pausePreview(); return; }
    playing_ = true;
    if (position_ < selectionStart() || position_ >= selectionEnd() - 0.02) position_ = selectionStart();
    pendingSeek_ = position_;
    seeking_ = true;
    if (!previewOpened_) {
        previewOpened_ = true;
        previewLoading_ = true;
        preview_->setPaused(true);
        if (!preview_->open(sourceUrl_)) {
            previewOpened_ = false;
            previewLoading_ = false;
            playing_ = false;
        }
    } else if (previewLoaded_) {
        preview_->seekExact(pendingSeek_);
        preview_->setPaused(false);
    }
    emit positionChanged();
    emit previewChanged();
}
void ClipController::notePresentedFrame() {
    if (!previewLoaded_ || seeking_ || invalidated_ || stopped_) return;
    if (videoVisible_ && !previewLoading_) return;
    videoVisible_ = true;
    previewLoading_ = false;
    emit previewChanged();
}
void ClipController::setFilmstripWidth(double width) {
    if (!std::isfinite(width) || width <= 0.0) return;
    const int count = std::clamp(static_cast<int>(std::ceil(std::min(width, 4096.0) / 114.0)), 8, 28);
    if (count == tileCount_) return;
    tileCount_ = count;
    if (selection_.valid() && !invalidated_ && !stopped_) thumbnailTimer_.start(80);
}
void ClipController::rebuildThumbnails() {
    if (!selection_.valid() || invalidated_ || stopped_) return;
    ++tileGeneration_;
    tileQueue_.erase(std::remove_if(tileQueue_.begin(), tileQueue_.end(),
                                   [](const TileRequest& tile) { return tile.row >= 0; }), tileQueue_.end());
    if (activeTile_ && activeTile_->request.row >= 0) {
        frames_.cancel();
        activeTile_.reset();
    }
    {
        QMutexLocker lock(&frameStore_->mutex);
        frameStore_->tiles.fill({}, tileCount_);
    }
    thumbnails_.reset(tileCount_, windowStart(), windowEnd());
    QVector<TileRequest> requested;
    requested.reserve(tileCount_);
    for (int row = 0; row < tileCount_; ++row)
        requested.append({row, thumbnails_.timeAt(row), tileGeneration_});
    std::sort(requested.begin(), requested.end(), [this](const auto& left, const auto& right) {
        return std::abs(left.seconds - selectionStart()) < std::abs(right.seconds - selectionStart());
    });
    tileQueue_ += requested;
    pumpThumbnails();
}
void ClipController::queueStill() {
    if (invalidated_ || stopped_ || previewOpened_) return;
    tileQueue_.erase(std::remove_if(tileQueue_.begin(), tileQueue_.end(),
                                   [](const TileRequest& tile) { return tile.row < 0; }), tileQueue_.end());
    tileQueue_.prepend({-1, stillRequestedTime_, tileGeneration_});
    pumpThumbnails();
}
void ClipController::pumpThumbnails() {
    if (activeTile_ || tileQueue_.isEmpty() || invalidated_ || stopped_) return;
    activeTile_ = ActiveTile{tileQueue_.takeFirst(), ++tileToken_};
    frames_.requestFrame(activeTile_->token, activeTile_->request.seconds);
}
void ClipController::finishThumbnail(quint64 token, double, const QImage& frame, const QString& reason) {
    if (!activeTile_ || token != activeTile_->token || invalidated_ || stopped_) return;
    const TileRequest request = activeTile_->request;
    activeTile_.reset();
    if (request.row < 0 && !previewOpened_ &&
        std::abs(request.seconds - stillRequestedTime_) < 0.001) {
        previewLoading_ = false;
        if (!frame.isNull()) {
            const quint64 revision = ++imageRevision_;
            {
                QMutexLocker lock(&frameStore_->mutex);
                frameStore_->still = {frame, revision};
            }
            previewSource_ = QUrl(QStringLiteral("image://clip-frames/still/0/%1").arg(revision));
        } else if (!reason.isEmpty()) {
            setStatus(QT_TR_NOOP("Preview unavailable: %1"), {reason});
        }
        emit previewChanged();
    } else if (request.row >= 0 && request.generation == tileGeneration_ && !frame.isNull()) {
        const quint64 revision = ++imageRevision_;
        const QImage thumbnail = frame.scaled(228, 128, Qt::KeepAspectRatioByExpanding,
                                              Qt::SmoothTransformation);
        {
            QMutexLocker lock(&frameStore_->mutex);
            frameStore_->tiles[request.row] = {thumbnail, revision};
        }
        thumbnails_.setImage(request.row,
            QUrl(QStringLiteral("image://clip-frames/tile/%1/%2").arg(request.row).arg(revision)));
    }
    pumpThumbnails();
}
void ClipController::captureTracks() {
    if (source_ == nullptr || invalidated_ || stopped_ || !source_->mediaTracks().ready) return;
    audioTrack_ = source_->mediaTrackSelection().audio;
    externalAudio_.clear();
    for (const auto& track : source_->mediaTracks().tracks) {
        if (track.type == MediaTrackType::Audio && audioTrack_ && track.id == *audioTrack_ && track.external) {
            externalAudio_ = track.externalFilename;
            break;
        }
    }
    syncPreviewAudio();
}
void ClipController::syncPreviewAudio() {
    if (!previewLoaded_ || invalidated_ || stopped_ || !preview_->mediaTracks().ready) return;
    const auto& snapshot = preview_->mediaTracks();
    const auto select = [this, &snapshot](qint64 id) {
        if (preview_->mediaTrackSelection().audio.value_or(0) != id)
            preview_->selectMediaTrack(MediaTrackType::Audio, id, snapshot.coreIdentity, snapshot.sourceEpoch);
    };
    if (!audioTrack_) {
        select(0);
        return;
    }
    if (!externalAudio_.isEmpty() && previewExternalAudio_ != externalAudio_) {
        if (preview_->addExternalTrack(externalAudio_, false)) previewExternalAudio_ = externalAudio_;
        return;
    }
    const MediaTrack* sourceTrack = nullptr;
    if (source_ && source_->mediaTracks().ready) {
        for (const auto& track : source_->mediaTracks().tracks) {
            if (track.type == MediaTrackType::Audio && track.id == *audioTrack_) { sourceTrack = &track; break; }
        }
    }
    const MediaTrack* fallback = nullptr;
    for (const auto& track : snapshot.tracks) {
        if (track.type != MediaTrackType::Audio) continue;
        if (track.external) {
            if (!externalAudio_.isEmpty() && track.externalFilename == externalAudio_) {
                select(track.id);
                return;
            }
            continue;
        }
        if (fallback == nullptr) fallback = &track;
        if (externalAudio_.isEmpty() && sourceTrack != nullptr &&
            (track.id == sourceTrack->id || (!sourceTrack->language.isEmpty() && track.language == sourceTrack->language))) {
            fallback = &track;
            break;
        }
    }
    if (fallback != nullptr) select(fallback->id);
}
void ClipController::requestExport() {
    if (!canExport() || !source_) return;
    pausePreview();
    shell::ClipExportRequest request;
    request.source = sourceUrl_;
    request.startSeconds = selectionStart();
    request.endSeconds = selectionEnd();
    request.sourceSize = sourceSize_;
    request.maximumHeight = qualityHeight_;
    request.audioTrackId = audioTrack_;
    request.externalAudioPath = externalAudio_;
    request.sourceOptions = {
        {"user-agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/142.0.0.0 Safari/537.36"},
        {"http-header-fields", "Accept-Language: uk-UA"}};
    pendingExport_ = std::move(request);
    exportState_ = ExportState::Choosing;
    QString name = QFileInfo(QUrl::fromUserInput(sourceUrl_).path()).completeBaseName();
    if (name.isEmpty()) name = tr("clip");
    const QString directory = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    emit stateChanged();
    emit outputRequested(QUrl::fromLocalFile(QDir(directory).filePath(name + QStringLiteral(".mp4"))));
}
bool ClipController::pendingExportIsCurrent() const {
    return pendingExport_ && source_ && !invalidated_ && !stopped_ && selection_.valid() &&
           source_->sourceEpoch() == sourceEpoch_ && source_->sourceUrl() == sourceUrl_ &&
           pendingExport_->startSeconds == selectionStart() && pendingExport_->endSeconds == selectionEnd() &&
           pendingExport_->sourceSize == sourceSize_ && pendingExport_->maximumHeight == qualityHeight_ &&
           pendingExport_->audioTrackId == audioTrack_ && pendingExport_->externalAudioPath == externalAudio_;
}
void ClipController::acceptOutput(const QUrl& path) {
    if (!choosingOutput()) return;
    if (!pendingExportIsCurrent()) {
        cancelOutput();
        setStatus(QT_TR_NOOP("The selection changed. Choose the output file again."));
        return;
    }
    if (!path.isLocalFile() || path.toLocalFile().isEmpty()) {
        cancelOutput();
        setStatus(QT_TR_NOOP("Choose a local MP4 output file."));
        return;
    }
    outputPath_ = path.toLocalFile();
    if (!outputPath_.endsWith(QLatin1String(".mp4"), Qt::CaseInsensitive)) outputPath_ += QStringLiteral(".mp4");
    const QFileInfo output(outputPath_);
    if (output.exists() || output.isSymbolicLink()) {
        exportState_ = ExportState::Confirming;
        emit stateChanged();
        return;
    }
    startExport();
}
void ClipController::cancelOutput() {
    if (!choosingOutput() && !overwritePending()) return;
    pendingExport_.reset();
    exportState_ = ExportState::Editing;
    emit stateChanged();
}
void ClipController::confirmOverwrite(bool replace) {
    if (!overwritePending()) return;
    if (!replace || !pendingExportIsCurrent()) { cancelOutput(); return; }
    pendingExport_->replaceExisting = true;
    startExport();
}
void ClipController::startExport() {
    if (!pendingExportIsCurrent()) { cancelOutput(); return; }
    pendingExport_->outputPath = outputPath_;
    exportState_ = ExportState::Encoding;
    completedPath_.clear();
    codec_.clear();
    progress_ = 0.0;
    setStatus(QT_TR_NOOP("Preparing export…"));
    exporter_.start(*pendingExport_);
}
void ClipController::cancelExport() {
    if (exportState_ != ExportState::Encoding) return;
    exportState_ = ExportState::Cancelling;
    setStatus(QT_TR_NOOP("Canceling export…"));
    exporter_.cancel();
}
void ClipController::revealOutput() {
    if (completedPath_.isEmpty()) return;
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(completedPath_).absolutePath())))
        setStatus(QT_TR_NOOP("Could not open the containing folder."));
}
void ClipController::copyOutputPath() {
    if (!completedPath_.isEmpty()) QGuiApplication::clipboard()->setText(completedPath_);
}
void ClipController::invalidate(const char* message) {
    if (invalidated_) return;
    invalidated_ = true;
    exportState_ = ExportState::Editing;
    previewLoading_ = false;
    playing_ = false;
    pendingExport_.reset();
    exporter_.cancel();
    frames_.shutdown();
    thumbnailTimer_.stop();
    stillTimer_.stop();
    preview_->close();
    setStatus(message);
    emit previewChanged();
}
void ClipController::requestClose() { emit closeRequested(); }
void ClipController::shutdown() {
    if (stopped_) return;
    stopped_ = true;
    playing_ = false;
    exporter_.cancel();
    frames_.shutdown();
    thumbnailTimer_.stop();
    stillTimer_.stop();
    tileQueue_.clear();
    activeTile_.reset();
    pendingExport_.reset();
    preview_->close();
}

}
