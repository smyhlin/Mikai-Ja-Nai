#include "ClipController.hpp"

#include <QCoreApplication>
#include <QDebug>
#include <QLocale>
#include <QMetaObject>
#include <QPointer>
#include <QRunnable>
#include <QThreadPool>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>

namespace mplayer::quick {

struct ClipEstimateSlot {
    ClipController* target = nullptr;
    std::atomic_bool alive{true};
    std::atomic<quint64> generation{0};
};

struct ClipEstimateBatch {
    std::vector<QImage> images;
    std::vector<double> times;
};

bool ClipController::estimatedSizeReady() const {
    return sizeEstimate_.has_value() && !invalidated_ && !stopped_;
}

QString ClipController::estimatedSize() const {
    if (!estimatedSizeReady()) {
        return estimateUnavailable_ || invalidated_ || stopped_
            ? tr("Size estimate unavailable") : tr("Analysing size…");
    }
    const QLocale locale;
    return tr("≈ %1 (%2–%3)").arg(locale.formattedDataSize(sizeEstimate_->expectedBytes),
        locale.formattedDataSize(sizeEstimate_->lowerBytes),
        locale.formattedDataSize(sizeEstimate_->upperBytes));
}

QString ClipController::estimatedSizeDetails() const {
    if (invalidated_ || stopped_) return tr("The source is no longer available for estimation.");
    if (!estimateError_.isEmpty()) return tr("Size estimate unavailable: %1").arg(estimateError_);
    if (!std::isfinite(sourceFps_) || sourceFps_ <= 0.0 ||
        sourceSize_.width() < 2 || sourceSize_.height() < 2) {
        return tr("A size estimate needs known source dimensions and frame rate.");
    }
    if (!estimatedSizeReady()) {
        return estimateUnavailable_ ? tr("No usable source frames could be analysed.")
                                    : tr("Decoding a few frames from the selected clip; no trial encoding.");
    }
    const QString codec = estimateProfile_->encoder == shell::ClipVideoEncoder::H264
        ? tr("H.264, CRF 23, veryfast") : tr("MPEG-4 Part 2, QP 4");
    QString text = tr("Uncalibrated %1 estimate including MP4 overhead. The range is approximate, not a guaranteed file size.")
        .arg(codec);
    if (estimateFeatures_->temporalPairCount == 0) {
        text += QLatin1Char('\n') + tr("Motion uses a prior; sampled frames are not consecutive.");
    }
    const bool includeAudio = !externalAudio_.isEmpty() || audioTrack_.value_or(0) > 0;
    text += QLatin1Char('\n') + (includeAudio
        ? tr("AAC assumes 192 kb/s, with a rough 64–320 kb/s allowance. The encoder default may differ.")
        : tr("No exported audio track."));
    text += QLatin1Char('\n') + tr("Decoded source samples: %1.").arg(estimateFeatures_->sampleCount);
    return text;
}

void ClipController::initializeSizeEstimator() {
    estimateSlot_ = std::make_shared<ClipEstimateSlot>();
    estimateSlot_->target = this;
    sourceFps_ = source_->sourceFramesPerSecond();
    estimateTimer_.setSingleShot(true);
    connect(&estimateTimer_, &QTimer::timeout, this, &ClipController::queueEstimateSamples);
}

void ClipController::scheduleSizeEstimate() {
    if (!estimateSlot_ || invalidated_ || stopped_) return;
    ++estimateGeneration_;
    estimateSlot_->generation.store(estimateGeneration_, std::memory_order_release);
    estimateFeatures_.reset();
    sizeEstimate_.reset();
    estimateBatch_.reset();
    estimatePendingFrames_ = 0;
    estimateAnalyzing_ = false;
    estimateUnavailable_ = false;
    estimateError_.clear();
    tileQueue_.erase(std::remove_if(tileQueue_.begin(), tileQueue_.end(),
        [](const TileRequest& request) { return request.row == -2; }), tileQueue_.end());
    if (activeTile_ && activeTile_->request.row == -2) {
        frames_.cancel();
        activeTile_.reset();
    }
    emit estimateChanged();
    if (selection_.valid()) estimateTimer_.start(140);
}

void ClipController::queueEstimateSamples() {
    if (invalidated_ || stopped_ || !selection_.valid() || !estimateSlot_) return;
    if (source_ && source_->sourceEpoch() == sourceEpoch_) {
        sourceFps_ = source_->sourceFramesPerSecond();
    }
    if (!std::isfinite(sourceFps_) || sourceFps_ <= 0.0 ||
        !shell::clipOutputSize(sourceSize_, qualityHeight_)) {
        estimateUnavailable_ = true;
        emit estimateChanged();
        return;
    }
    constexpr int sampleCount = 3;
    estimateBatch_ = std::make_shared<ClipEstimateBatch>();
    estimateBatch_->images.reserve(sampleCount);
    estimateBatch_->times.reserve(sampleCount);
    estimatePendingFrames_ = 0;
    const double maxReuseDelta = std::max(1.5, selectionDuration() / 4.0);
    int insertion = 0;
    while (insertion < tileQueue_.size() && tileQueue_[insertion].row == -1) ++insertion;
    int inserted = 0;
    for (int i = 0; i < sampleCount; ++i) {
        const double seconds = selectionStart() + selectionDuration() * (i + 0.5) / sampleCount;
        if (const auto cached = frames_.nearestCachedFrame(seconds, maxReuseDelta)) {
            const double sampleTime = cached->decodedSeconds >= 0.0 ? cached->decodedSeconds
                                                                    : cached->requestedSeconds;
            if (!cached->image.isNull() && std::isfinite(sampleTime) &&
                sampleTime >= selectionStart() && sampleTime <= selectionEnd()) {
                const bool duplicate = std::any_of(
                    estimateBatch_->times.begin(), estimateBatch_->times.end(),
                    [sampleTime](double time) { return std::abs(time - sampleTime) < 0.001; });
                if (!duplicate) {
                    estimateBatch_->images.push_back(cached->image);
                    estimateBatch_->times.push_back(sampleTime);
                    continue;
                }
            }
        }
        ++estimatePendingFrames_;
        tileQueue_.insert(insertion + inserted, {-2, seconds, estimateGeneration_, false});
        ++inserted;
    }
    if (estimatePendingFrames_ == 0) {
        analyzeEstimateSamples();
    } else {
        pumpThumbnails();
    }
}

void ClipController::consumeEstimateFrame(quint64 generation, double decodedSeconds,
                                        const QImage& frame, const QString& reason) {
    if (generation != estimateGeneration_ || !estimateBatch_ || estimatePendingFrames_ <= 0) return;
    if (!frame.isNull() && std::isfinite(decodedSeconds) &&
        decodedSeconds >= selectionStart() && decodedSeconds <= selectionEnd()) {
        const bool duplicate = std::any_of(estimateBatch_->times.begin(), estimateBatch_->times.end(),
            [decodedSeconds](double time) { return std::abs(time - decodedSeconds) < 0.001; });
        if (!duplicate) {
            estimateBatch_->images.push_back(frame);
            estimateBatch_->times.push_back(decodedSeconds);
        }
    } else if (!reason.isEmpty()) {
        estimateError_ = reason;
    }
    --estimatePendingFrames_;
    if (estimatePendingFrames_ == 0) analyzeEstimateSamples();
}

void ClipController::analyzeEstimateSamples() {
    if (!estimateBatch_ || estimateBatch_->images.empty()) {
        estimateUnavailable_ = true;
        qWarning() << "Clip size estimate: no usable decoded source frames";
        emit estimateChanged();
        return;
    }
    estimateAnalyzing_ = true;
    const auto slot = estimateSlot_;
    const quint64 generation = estimateGeneration_;
    const double fps = sourceFps_;
    auto batch = std::move(estimateBatch_);
    const auto knownProfile = estimateProfile_;
    QThreadPool::globalInstance()->start(QRunnable::create(
        [slot, generation, fps, batch = std::move(batch), knownProfile] {
            const auto current = [&] {
                return slot->alive.load(std::memory_order_acquire) &&
                    slot->generation.load(std::memory_order_acquire) == generation;
            };
            if (!current()) return;
            auto profile = knownProfile;
            QString error;
            if (!profile) {
                shell::ClipEncodingProfile resolved{};
                if (shell::probeClipEncodingProfile(resolved, error)) profile = resolved;
            }
            std::vector<shell::ClipAnalysisFrame> frames;
            std::vector<double> times;
            if (profile) {
                frames.reserve(batch->images.size());
                times.reserve(batch->times.size());
                for (std::size_t i = 0; i < batch->images.size(); ++i) {
                    if (!current()) return;
                    auto frame = shell::analyzeClipFrame(batch->images[i]);
                    if (!frame) continue;
                    frames.push_back(std::move(*frame));
                    times.push_back(batch->times[i]);
                }
            }
            const auto features = shell::summarizeClipSamples(frames, times, fps);
            if (!current()) return;
            auto* dispatcher = QCoreApplication::instance();
            if (dispatcher == nullptr) return;
            QMetaObject::invokeMethod(dispatcher, [slot, generation, profile, features, error] {
                if (!slot->alive.load(std::memory_order_acquire) ||
                    slot->generation.load(std::memory_order_acquire) != generation) return;
                const QPointer<ClipController> guard(slot->target);
                auto* target = guard.data();
                if (target == nullptr || target->invalidated_ || target->stopped_ ||
                    !target->source_ || target->source_->sourceEpoch() != target->sourceEpoch_) return;
                target->estimateAnalyzing_ = false;
                target->estimateProfile_ = profile;
                target->estimateUnavailable_ = !profile || features.sampleCount == 0;
                target->estimateError_ = error;
                if (features.sampleCount > 0) target->estimateFeatures_ = features;
                if (target->estimateUnavailable_) {
                    qWarning() << "Clip size estimate: encoder profile or content analysis unavailable";
                }
                target->refreshSizeEstimate();
            }, Qt::QueuedConnection);
        }));
}

void ClipController::refreshSizeEstimate() {
    if (invalidated_ || stopped_ || !estimateSlot_) return;
    sizeEstimate_.reset();
    if (estimateProfile_ && estimateFeatures_ && source_ && source_->mediaTracks().ready) {
        const auto output = shell::clipOutputSize(sourceSize_, qualityHeight_);
        if (output) {
            const bool includeAudio = !externalAudio_.isEmpty() || audioTrack_.value_or(0) > 0;
            sizeEstimate_ = shell::estimateClipSize(*estimateProfile_, *output, sourceFps_,
                selectionDuration(), includeAudio, *estimateFeatures_);
        }
    } else if (!estimateUnavailable_ && !estimateFeatures_ && selection_.valid() &&
               !estimateTimer_.isActive() && !estimateAnalyzing_ && estimatePendingFrames_ == 0) {
        scheduleSizeEstimate();
        return;
    }
    emit estimateChanged();
}

void ClipController::stopSizeEstimator() {
    estimateTimer_.stop();
    if (estimateSlot_) {
        estimateSlot_->alive.store(false, std::memory_order_release);
        estimateSlot_->target = nullptr;
    }
    estimateBatch_.reset();
    estimateFeatures_.reset();
    sizeEstimate_.reset();
    estimatePendingFrames_ = 0;
    estimateAnalyzing_ = false;
    estimateUnavailable_ = true;
    emit estimateChanged();
}

}  // namespace mplayer::quick
