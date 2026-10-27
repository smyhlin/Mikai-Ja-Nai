#include <QApplication>
#include <QCommandLineParser>
#include <QColor>
#include <QDir>
#include <QElapsedTimer>
#include <QImage>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QOpenGLFunctions>
#include <QOpenGLWidget>
#include <QTimer>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <utility>

#include "ProbePicture.hpp"
#include "ProbeRuntime.hpp"
#include "VideoSurface.hpp"

namespace {

constexpr double kStartPositionToleranceSeconds = 1.0;

QImage readCurrentFramebuffer(mplayer::VideoSurface& surface) {
    if (!surface.context() || surface.width() <= 0 || surface.height() <= 0) return {};
    const qreal dpr = surface.devicePixelRatioF();
    const int width = qRound(surface.width() * dpr);
    const int height = qRound(surface.height() * dpr);
    if (width <= 0 || height <= 0) return {};
    QImage frame(width, height, QImage::Format_RGBA8888);
    if (frame.isNull()) return {};
    QOpenGLExtraFunctions* gl = surface.context()->extraFunctions();
    if (gl == nullptr) return {};

    surface.makeCurrent();
    GLint oldReadFramebuffer = 0;
    GLint oldReadBuffer = 0;
    GLint oldPackAlignment = 4;
    gl->glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &oldReadFramebuffer);
    gl->glGetIntegerv(GL_READ_BUFFER, &oldReadBuffer);
    gl->glGetIntegerv(GL_PACK_ALIGNMENT, &oldPackAlignment);
    gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, surface.defaultFramebufferObject());
    gl->glReadBuffer(GL_COLOR_ATTACHMENT0);
    gl->glPixelStorei(GL_PACK_ALIGNMENT, 1);
    gl->glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, frame.bits());
    const GLenum readError = gl->glGetError();
    gl->glPixelStorei(GL_PACK_ALIGNMENT, oldPackAlignment);
    gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(oldReadFramebuffer));
    gl->glReadBuffer(static_cast<GLenum>(oldReadBuffer));
    surface.doneCurrent();
    if (readError != GL_NO_ERROR) return {};
    return frame.flipped(Qt::Vertical);
}

bool isTenBitOrHigherFormat(const QString& value) {
    const QString format = value.trimmed().toLower();
    return format.contains(QStringLiteral("p010")) ||
           format.contains(QStringLiteral("p012")) ||
           format.contains(QStringLiteral("p016")) ||
           format.contains(QStringLiteral("10le")) ||
           format.contains(QStringLiteral("10be")) ||
           format.contains(QStringLiteral("12le")) ||
           format.contains(QStringLiteral("12be")) ||
           format.contains(QStringLiteral("14le")) ||
           format.contains(QStringLiteral("14be")) ||
           format.contains(QStringLiteral("16le")) ||
           format.contains(QStringLiteral("16be"));
}

bool hasVisibleDarkRange(const QImage& image) {
    if (image.isNull()) return false;
    int varying = 0;
    int sampled = 0;
    int maximum = 0;
    int minimum = 255;
    for (int y = 0; y < image.height(); y += 3) {
        for (int x = 0; x < image.width(); x += 3) {
            const QColor pixel = image.pixelColor(x, y);
            const int value = std::max({pixel.red(), pixel.green(), pixel.blue()});
            maximum = std::max(maximum, value);
            minimum = std::min(minimum, value);
            if (value > 4) ++varying;
            ++sampled;
        }
    }
    return sampled > 0 && varying * 100 >= sampled && maximum >= 12 && maximum > minimum;
}

std::optional<int> countOffEightBitLatticePixels(mplayer::VideoSurface& surface) {
    if (surface.context() == nullptr || surface.width() <= 0 || surface.height() <= 0)
        return std::nullopt;
    QOpenGLContext* previous = QOpenGLContext::currentContext();
    if (previous != surface.context()) surface.makeCurrent();
    if (QOpenGLContext::currentContext() != surface.context()) {
        if (previous != surface.context()) {
            surface.doneCurrent();
            if (previous) previous->makeCurrent(previous->surface());
        }
        return std::nullopt;
    }

    QOpenGLExtraFunctions* gl = surface.context()->extraFunctions();
    if (gl == nullptr) {
        if (previous != surface.context()) {
            surface.doneCurrent();
            if (previous) previous->makeCurrent(previous->surface());
        }
        return std::nullopt;
    }

    constexpr int sampleWidth = 256;
    const qreal dpr = surface.devicePixelRatioF();
    const int width = qRound(surface.width() * dpr);
    const int height = qRound(surface.height() * dpr);
    if (width < sampleWidth || height <= 0) {
        if (previous != surface.context()) {
            surface.doneCurrent();
            if (previous) previous->makeCurrent(previous->surface());
        }
        return std::nullopt;
    }

    std::array<quint16, sampleWidth * 4> pixels{};
    GLint oldReadFramebuffer = 0;
    GLint oldReadBuffer = 0;
    GLint oldPackAlignment = 4;
    GLint oldPackRowLength = 0;
    GLint oldPackSkipPixels = 0;
    GLint oldPackSkipRows = 0;
    GLint oldPixelPackBuffer = 0;
    gl->glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &oldReadFramebuffer);
    gl->glGetIntegerv(GL_READ_BUFFER, &oldReadBuffer);
    gl->glGetIntegerv(GL_PACK_ALIGNMENT, &oldPackAlignment);
    gl->glGetIntegerv(GL_PACK_ROW_LENGTH, &oldPackRowLength);
    gl->glGetIntegerv(GL_PACK_SKIP_PIXELS, &oldPackSkipPixels);
    gl->glGetIntegerv(GL_PACK_SKIP_ROWS, &oldPackSkipRows);
    gl->glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &oldPixelPackBuffer);
    gl->glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    gl->glBindFramebuffer(GL_READ_FRAMEBUFFER,
                          static_cast<GLuint>(surface.defaultFramebufferObject()));
    gl->glReadBuffer(GL_COLOR_ATTACHMENT0);
    gl->glPixelStorei(GL_PACK_ALIGNMENT, 1);
    gl->glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    gl->glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    gl->glPixelStorei(GL_PACK_SKIP_ROWS, 0);
    const int x = width / 2 - sampleWidth / 2;
    const int y = height / 2;
    gl->glReadPixels(x, y, sampleWidth, 1, GL_RGBA, GL_UNSIGNED_SHORT, pixels.data());
    const GLenum readError = gl->glGetError();
    gl->glPixelStorei(GL_PACK_ALIGNMENT, oldPackAlignment);
    gl->glPixelStorei(GL_PACK_ROW_LENGTH, oldPackRowLength);
    gl->glPixelStorei(GL_PACK_SKIP_PIXELS, oldPackSkipPixels);
    gl->glPixelStorei(GL_PACK_SKIP_ROWS, oldPackSkipRows);
    gl->glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(oldPixelPackBuffer));
    gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(oldReadFramebuffer));
    gl->glReadBuffer(static_cast<GLenum>(oldReadBuffer));
    if (previous != surface.context()) {
        surface.doneCurrent();
        if (previous) previous->makeCurrent(previous->surface());
    }
    if (readError != GL_NO_ERROR) return std::nullopt;

    int offLattice = 0;
    for (int pixel = 0; pixel < sampleWidth; ++pixel) {
        for (int channel = 0; channel < 3; ++channel) {
            const int value = pixels[pixel * 4 + channel];
            const int nearestEightBit = ((value + 128) / 257) * 257;
            if (std::abs(value - nearestEightBit) >= 64) ++offLattice;
        }
    }
    return offLattice;
}

bool imagesDiffer(const QImage& left, const QImage& right) {
    if (left.isNull() || right.isNull() || left.size() != right.size()) return false;
    const QImage a = left.convertToFormat(QImage::Format_RGB32);
    const QImage b = right.convertToFormat(QImage::Format_RGB32);
    int changed = 0;
    int compared = 0;
    for (int y = 4; y < a.height(); y += 5) {
        for (int x = 4; x < a.width(); x += 5) {
            ++compared;
            const QColor first = a.pixelColor(x, y);
            const QColor second = b.pixelColor(x, y);
            if (std::abs(first.red() - second.red()) +
                    std::abs(first.green() - second.green()) +
                    std::abs(first.blue() - second.blue()) > 6) {
                ++changed;
            }
        }
    }
    return compared > 0 && changed * 100 > compared;
}

class HdrAcceptance {
public:
    HdrAcceptance(QString hdr, QString sdr, QString artifacts, bool tenBitFbo,
                  bool naturalMedia, std::optional<double> startSeconds, QApplication& app)
        : hdr_(std::move(hdr)), sdr_(std::move(sdr)),
          artifacts_(std::move(artifacts)), tenBitFbo_(tenBitFbo),
          naturalMedia_(naturalMedia), startSeconds_(startSeconds), app_(app) {}

    int run() {
        surface_.setWindowTitle(QStringLiteral("hdr_playback_probe"));
        if (tenBitFbo_)
            surface_.setTextureFormat(mplayer::kGlInternalFormatRgb10A2);
        surface_.resize(854, 480);
        surface_.show();
        mplayer::VideoSurface::setHdrToneMappingPolicy(
            mplayer::VideoSurface::HdrToneMappingPolicy::Auto);
        mplayer::VideoSurface::setAnimeJanaiHdrOverride(false);
        fixesEnabled_ = mplayer::VideoSurface::hdrRenderingFixesEnabled();
        QObject::connect(&surface_, &mplayer::VideoSurface::errorOccurred, &surface_,
                         [this](const QString& message) {
                             verdict_.require(false, QStringLiteral("mpv playback error: %1").arg(message));
                         });
        QObject::connect(&surface_, &mplayer::VideoSurface::anime4kGated, &surface_,
                         [this](const QString&) { ++gateNotices_; });
        QObject::connect(&surface_, &QOpenGLWidget::frameSwapped, &surface_, [this]() {
            if (!surface_.context()) return;
            surface_.makeCurrent();
            for (int i = 0; i < 32; ++i) {
                const GLenum error = surface_.context()->functions()->glGetError();
                if (error == GL_NO_ERROR) break;
                ++glErrors_;
            }
            surface_.doneCurrent();
        });
        if (!surface_.open(hdr_)) {
            std::fprintf(stderr, "hdr_playback_probe: cannot open HDR source: %s\n", qPrintable(hdr_));
            return 2;
        }
        surface_.setMuted(true);
        clock_.start();
        timer_.setInterval(100);
        QObject::connect(&timer_, &QTimer::timeout, &surface_, [this]() { tick(); });
        timer_.start();
        QTimer::singleShot(90000, &surface_, [this]() {
            if (!finished_) failAndFinish(QStringLiteral("acceptance timed out"));
        });
        return app_.exec();
    }

private:
    enum class Stage { HdrLive, HdrPaused, StableApplied, VividApplied, SdrLive, Finished };
    std::optional<QString> readMpvTextProperty(const QString& name) {
        const auto value = surface_.probeMpvProperty(
            name, mplayer::ProbePropertyKind::Text);
        if (!value.has_value() || value->kind != mplayer::ProbePropertyValue::Kind::Text) {
            return std::nullopt;
        }
        return value->text;
    }

    QString readTextProperty(const QString& name) {
        return readMpvTextProperty(name).value_or(QString{});
    }

    std::optional<double> readMpvNumberProperty(const QString& name) {
        const auto value = surface_.probeMpvProperty(
            name, mplayer::ProbePropertyKind::Number);
        if (!value.has_value() || !value->isNumeric() ||
            !std::isfinite(value->asNumber())) {
            return std::nullopt;
        }
        return value->asNumber();
    }

    void saveArtifact(const QString& name, const QImage& image) {
        if (artifacts_.isEmpty()) return;
        if (image.isNull() || !QDir().mkpath(artifacts_) ||
            !image.save(QDir(artifacts_).filePath(name), "PNG")) {
            verdict_.require(false, QStringLiteral("cannot save requested image artifact %1").arg(name));
        }
    }


    void failAndFinish(const QString& reason) {
        verdict_.require(false, reason);
        finish();
    }

    void finish() {
        if (finished_) return;
        finished_ = true;
        timer_.stop();
        verdict_.require(glErrors_ == 0,
                         QStringLiteral("no OpenGL error during live HDR/SDR rendering (count %1)")
                             .arg(glErrors_));
        verdict_.metric(QStringLiteral("HDR fixes %1; GL errors %2; HDR gate notices %3")
                            .arg(fixesEnabled_ ? QStringLiteral("enabled") : QStringLiteral("disabled"))
                            .arg(glErrors_)
                            .arg(gateNotices_));
        surface_.close();
        surface_.hide();
        app_.exit(verdict_.report("hdr_playback_probe"));
    }

    void tick() {
        if (finished_) return;
        const qint64 now = clock_.elapsed();
        const auto profile = surface_.sourceColorProfile();
        const auto frames = surface_.playbackDiagnostics().renderedOutputFrames;
        switch (stage_) {
            case Stage::HdrLive: {
                if (!profile.isHdr() || frames < 3) return;
                if (startSeconds_.has_value() && !startSeekIssued_) {
                    surface_.seek(*startSeconds_);
                    startSeekIssued_ = true;
                    startSeekAt_ = now;
                    return;
                }
                if (startSeconds_.has_value() && !startPositionVerified_) {
                    const auto position = readMpvNumberProperty(QStringLiteral("time-pos"));
                    if (!position.has_value() ||
                        std::abs(*position - *startSeconds_) > kStartPositionToleranceSeconds) {
                        if (now - startSeekAt_ > 15000) {
                            failAndFinish(QStringLiteral("HDR seek did not settle within %1 s of requested position %2 "
                                                         "(actual %3)")
                                              .arg(kStartPositionToleranceSeconds, 0, 'f', 3)
                                              .arg(*startSeconds_, 0, 'f', 3)
                                              .arg(position.value_or(-1.0), 0, 'f', 3));
                        }
                        return;
                    }
                    if (!startPositionReached_) {
                        startPositionReached_ = true;
                        startPositionFrameBaseline_ = frames;
                        return;
                    }
                    if (frames <= startPositionFrameBaseline_) {
                        if (now - startSeekAt_ > 15000) {
                            failAndFinish(QStringLiteral("no rendered HDR frame advanced after the seek reached "
                                                         "the requested position"));
                        }
                        return;
                    }
                    verdict_.metric(QStringLiteral("HDR seek requested=%1 s actual=%2 s; rendered frames advanced %3")
                                        .arg(*startSeconds_, 0, 'f', 6)
                                        .arg(*position, 0, 'f', 6)
                                        .arg(frames - startPositionFrameBaseline_));
                    startPositionVerified_ = true;
                    surface_.setPaused(true);
                    return;
                }
                if (startPositionVerified_ && !surface_.pauseObserved()) return;
                verdict_.require(profile.hasTransferMetadata(),
                                 QStringLiteral("HDR source transfer metadata is observed from mpv"));
                verdict_.require(profile.gamma == QStringLiteral("pq") ||
                                     profile.gamma == QStringLiteral("smpte2084"),
                                 QStringLiteral("fixture reports the PQ transfer (%1)").arg(profile.gamma));
                verdict_.require(profile.primaries == QStringLiteral("bt.2020"),
                                 QStringLiteral("fixture reports BT.2020 primaries (%1)")
                                     .arg(profile.primaries));
                const QString hwPixelFormat = readTextProperty(
                    QStringLiteral("video-params/hw-pixelformat"));
                const QString decoderPixelFormat = readTextProperty(
                    QStringLiteral("video-dec-params/pixelformat"));
                const QString decoderHwPixelFormat = readTextProperty(
                    QStringLiteral("video-dec-params/hw-pixelformat"));
                const bool sourceIsTenBit = isTenBitOrHigherFormat(profile.pixelformat) ||
                                            isTenBitOrHigherFormat(hwPixelFormat) ||
                                            isTenBitOrHigherFormat(decoderPixelFormat) ||
                                            isTenBitOrHigherFormat(decoderHwPixelFormat);
                verdict_.require(sourceIsTenBit,
                                 QStringLiteral("mpv reports a 10-bit source pixel format "
                                                "(video=%1 hw=%2 decoder=%3 decoder-hw=%4)")
                                     .arg(profile.pixelformat, hwPixelFormat,
                                          decoderPixelFormat, decoderHwPixelFormat));
                verdict_.metric(QStringLiteral("mpv pixel formats video=%1 hw=%2 decoder=%3 decoder-hw=%4")
                                    .arg(profile.pixelformat, hwPixelFormat,
                                         decoderPixelFormat, decoderHwPixelFormat));
                verdict_.require(surface_.renderTargetFormatProbed() &&
                                     surface_.renderTargetInternalFormat() != 0,
                                 QStringLiteral("the rendered Qt FBO reports a supported sized color format"));
                verdict_.metric(QStringLiteral("HDR gamma=%1 primaries=%2 pixel-format=%3 FBO=0x%4")
                                    .arg(profile.gamma, profile.primaries, profile.pixelformat)
                                    .arg(surface_.renderTargetInternalFormat(), 0, 16));
                if (tenBitFbo_) {
                    const auto bits = surface_.renderTargetColorBits();
                    verdict_.require(
                        surface_.renderTargetInternalFormat() == mplayer::kGlInternalFormatRgb10A2 &&
                            bits.red == 10 && bits.green == 10 && bits.blue == 10 &&
                            surface_.renderTargetDepthBitsPerChannel() == 10,
                        QStringLiteral("requested RGB10_A2 FBO reports R/G/B and render depth of 10 "
                                       "(format=0x%1 R=%2 G=%3 B=%4 depth=%5)")
                            .arg(surface_.renderTargetInternalFormat(), 0, 16)
                            .arg(bits.red)
                            .arg(bits.green)
                            .arg(bits.blue)
                            .arg(surface_.renderTargetDepthBitsPerChannel()));
                }
                surface_.setAnime4KMode(mplayer::anime4k::Mode::FastA);
                stage_ = Stage::HdrPaused;
                stageAt_ = now;
                break;
            }
            case Stage::HdrPaused:
                if (fixesEnabled_) {
                    if (gateNotices_ == 0 || !surface_.effectiveShaderChain().isEmpty()) {
                        if (now - stageAt_ > 3000) {
                            failAndFinish(QStringLiteral("Anime4K was not gated on the observed HDR source"));
                        }
                        return;
                    }
                    if (!hdrShaderPropertyVerified_) {
                        const auto shaders = readMpvTextProperty(QStringLiteral("glsl-shaders"));
                        if (!shaders.has_value() || !shaders->trimmed().isEmpty()) {
                            if (now - stageAt_ > 3000) {
                                failAndFinish(QStringLiteral("mpv glsl-shaders did not become empty after the HDR gate"));
                            }
                            return;
                        }
                        hdrShaderPropertyVerified_ = true;
                    }
                } else {
                    if (gateNotices_ != 0) {
                        failAndFinish(QStringLiteral("MIKAI_HDR_FIXES=0 unexpectedly applied the HDR Anime4K gate"));
                        return;
                    }
                    if (!rollbackShaderRequested_) {
                        if (surface_.effectiveShaderChain().isEmpty()) {
                            if (now - stageAt_ > 3000) {
                                failAndFinish(QStringLiteral("rollback mode did not restore the requested HDR shader"));
                            }
                            return;
                        }
                        verdict_.require(surface_.anime4KMode() == mplayer::anime4k::Mode::FastA,
                                         QStringLiteral("rollback mode allows the requested HDR shader chain"));
                        const auto shaders = readMpvTextProperty(QStringLiteral("glsl-shaders"));
                        if (!shaders.has_value() || shaders->trimmed().isEmpty()) {
                            if (now - stageAt_ > 3000) {
                                failAndFinish(QStringLiteral("mpv glsl-shaders did not expose the requested HDR chain"));
                            }
                            return;
                        }
                        surface_.setAnime4KMode(mplayer::anime4k::Mode::Off);
                        rollbackShaderRequested_ = true;
                        return;
                    }
                    if (!surface_.effectiveShaderChain().isEmpty()) return;
                    if (!rollbackShaderPropertyRemoved_) {
                        const auto shaders = readMpvTextProperty(QStringLiteral("glsl-shaders"));
                        if (!shaders.has_value() || !shaders->trimmed().isEmpty()) {
                            if (now - stageAt_ > 3000) {
                                failAndFinish(QStringLiteral("mpv glsl-shaders did not clear the rollback chain"));
                            }
                            return;
                        }
                        rollbackShaderPropertyRemoved_ = true;
                    }
                }
                if (fixesEnabled_) {
                    verdict_.require(surface_.anime4KMode() == mplayer::anime4k::Mode::FastA,
                                     QStringLiteral("the HDR gate retains the requested preset"));
                } else {
                    verdict_.require(surface_.anime4KMode() == mplayer::anime4k::Mode::Off &&
                                         surface_.effectiveShaderChain().isEmpty(),
                                     QStringLiteral("rollback shader was explicitly removed before policy comparison"));
                }
                surface_.setPaused(true);
                if (!surface_.pauseObserved()) return;
                if (!pausedIdentityCaptured_) {
                    const auto sourceTime = readMpvNumberProperty(QStringLiteral("time-pos"));
                    if (!sourceTime.has_value()) {
                        if (now - stageAt_ > 3000) {
                            failAndFinish(QStringLiteral("mpv source time was unavailable after pausing HDR playback"));
                        }
                        return;
                    }
                    if (startSeconds_.has_value() &&
                        std::abs(*sourceTime - *startSeconds_) > kStartPositionToleranceSeconds) {
                        failAndFinish(QStringLiteral("paused HDR position %1 s is outside %2 s of requested %3 s")
                                          .arg(*sourceTime, 0, 'f', 6)
                                          .arg(kStartPositionToleranceSeconds, 0, 'f', 3)
                                          .arg(*startSeconds_, 0, 'f', 6));
                        return;
                    }
                    pausedSourceEpoch_ = surface_.sourceEpoch();
                    pausedSourceTime_ = *sourceTime;
                    pausedIdentityCapturedAt_ = now;
                    pausedIdentityCaptured_ = true;
                    return;
                }
                if (now - pausedIdentityCapturedAt_ < 1000) return;
                if (!autoCaptured_) {
                    autoFrame_ = surface_.grabVideoFramebuffer();
                    saveArtifact(QStringLiteral("auto.png"), autoFrame_);
                    verdict_.require(!autoFrame_.isNull(),
                                     QStringLiteral("Auto policy produced a real paused HDR framebuffer"));
                    verdict_.require(hasVisibleDarkRange(autoFrame_),
                                     QStringLiteral("HDR source produces non-black visible framebuffer detail"));
                    verdict_.metric(QStringLiteral("Auto HDR capture source epoch=%1 source time-pos=%2 s")
                                        .arg(pausedSourceEpoch_)
                                        .arg(pausedSourceTime_, 0, 'f', 6));
                    autoCaptured_ = true;
                }
                if (tenBitFbo_ && !precisionLatticeVerified_) {
                    const auto offLattice =
                        countOffEightBitLatticePixels(surface_);
                    if (!offLattice.has_value()) {
                        failAndFinish(QStringLiteral("10-bit HDR scanline readback failed"));
                        return;
                    }
                    verdict_.require(*offLattice > 0,
                                     QStringLiteral("10-bit HDR output has samples off the 8-bit lattice "
                                                    "(%1 RGB channels)")
                                         .arg(*offLattice));
                    verdict_.metric(QStringLiteral("10-bit HDR scanline off-8-bit-lattice channels: %1")
                                        .arg(*offLattice));
                    precisionLatticeVerified_ = true;
                }
                surface_.setHdrToneMappingPolicy(mplayer::VideoSurface::HdrToneMappingPolicy::Stable);
                if (mplayer::VideoSurface::hdrToneMappingPolicy() !=
                    mplayer::VideoSurface::HdrToneMappingPolicy::Stable) {
                    failAndFinish(QStringLiteral("Stable tone-mapping policy request was not retained"));
                    return;
                }
                stage_ = Stage::StableApplied;
                stageAt_ = now;
                break;
            case Stage::StableApplied:
                if (now - stageAt_ < 1500) return;
                stableFrame_ = surface_.grabVideoFramebuffer();
                saveArtifact(QStringLiteral("stable.png"), stableFrame_);
                surface_.setHdrToneMappingPolicy(mplayer::VideoSurface::HdrToneMappingPolicy::Vivid);
                stage_ = Stage::VividApplied;
                stageAt_ = now;
                break;
            case Stage::VividApplied: {
                if (now - stageAt_ < 1500) return;
                if (!vividCaptured_) {
                    vividFrame_ = surface_.grabVideoFramebuffer();
                    saveArtifact(QStringLiteral("vivid.png"), vividFrame_);
                    verdict_.require(!stableFrame_.isNull() && !vividFrame_.isNull(),
                                     QStringLiteral("Stable and Vivid policies produced real HDR framebuffers"));
                    const bool policyFramesDiffer = imagesDiffer(stableFrame_, vividFrame_);
                    if (naturalMedia_) {
                        verdict_.metric(QStringLiteral("natural-media Stable/Vivid captures: %1 at the same paused source frame")
                                            .arg(policyFramesDiffer
                                                     ? QStringLiteral("material difference observed")
                                                     : QStringLiteral("no material difference at the authored-fixture threshold")));
                    }
                    if (fixesEnabled_) {
                        if (!naturalMedia_) {
                            verdict_.require(policyFramesDiffer,
                                             QStringLiteral("Stable and Vivid policies changed the rendered HDR output"));
                        }
                    } else {
                        verdict_.require(!policyFramesDiffer,
                                         QStringLiteral("MIKAI_HDR_FIXES=0 suppresses policy changes to HDR rendering"));
                    }
                    vividCaptured_ = true;
                }
                if (surface_.sourceEpoch() != pausedSourceEpoch_) {
                    failAndFinish(QStringLiteral("source epoch changed during paused tone-mapping comparison"));
                    return;
                }
                const auto finalSourceTime = readMpvNumberProperty(QStringLiteral("time-pos"));
                if (!finalSourceTime.has_value()) {
                    if (now - stageAt_ > 5000) {
                        failAndFinish(QStringLiteral("mpv source time was unavailable after tone-mapping comparison"));
                    }
                    return;
                }
                if (std::abs(*finalSourceTime - pausedSourceTime_) > 0.01) {
                    failAndFinish(QStringLiteral("mpv source position advanced during paused tone-mapping comparison"));
                    return;
                }
                verdict_.require(surface_.sourceEpoch() == pausedSourceEpoch_,
                                 QStringLiteral("source epoch remained unchanged during paused policy comparison"));
                verdict_.require(std::abs(*finalSourceTime - pausedSourceTime_) <= 0.01,
                                 QStringLiteral("mpv source position remained stable during paused policy comparison"));
                surface_.setHdrToneMappingPolicy(mplayer::VideoSurface::HdrToneMappingPolicy::Auto);
                surface_.setPaused(false);
                if (!surface_.open(sdr_)) {
                    failAndFinish(QStringLiteral("cannot reopen the surface on the SDR source"));
                    return;
                }
                sdrSourceEpoch_ = surface_.sourceEpoch();
                stage_ = Stage::SdrLive;
                stageAt_ = now;
                break;
            }
            case Stage::SdrLive: {
                if (surface_.sourceEpoch() != sdrSourceEpoch_ || profile.gamma.isEmpty() || frames < 3) return;
                if (profile.isHdr()) {
                    if (now - stageAt_ > 5000) failAndFinish(QStringLiteral("SDR source remained classified as HDR"));
                    return;
                }
                verdict_.require(!profile.requiresSdrGate(),
                                 QStringLiteral("known SDR transfer is not gated (%1)").arg(profile.gamma));
                surface_.setAnime4KMode(mplayer::anime4k::Mode::FastA);
                if (surface_.effectiveShaderChain().isEmpty()) {
                    if (now - stageAt_ > 5000) failAndFinish(QStringLiteral("Anime4K did not activate on SDR"));
                    return;
                }
                const auto shaders = readMpvTextProperty(QStringLiteral("glsl-shaders"));
                if (!shaders.has_value() || shaders->trimmed().isEmpty()) {
                    if (now - stageAt_ > 5000) {
                        failAndFinish(QStringLiteral("mpv glsl-shaders did not expose the SDR Anime4K chain"));
                    }
                    return;
                }
                verdict_.require(surface_.anime4KMode() == mplayer::anime4k::Mode::FastA,
                                 QStringLiteral("Anime4K preset remains active on SDR"));
                saveArtifact(QStringLiteral("sdr.png"), readCurrentFramebuffer(surface_));
                finish();
                break;
            }
            case Stage::Finished:
                break;
        }
    }

    QString hdr_;
    QString sdr_;
    QString artifacts_;
    bool tenBitFbo_ = false;
    bool naturalMedia_ = false;
    std::optional<double> startSeconds_;
    QApplication& app_;
    mplayer::VideoSurface surface_;
    mplayer::probes::Verdict verdict_;
    QElapsedTimer clock_;
    QTimer timer_;
    QImage stableFrame_;
    QImage vividFrame_;
    QImage autoFrame_;
    Stage stage_ = Stage::HdrLive;
    qint64 stageAt_ = 0;
    int gateNotices_ = 0;
    int glErrors_ = 0;
    quint64 pausedSourceEpoch_ = 0;
    quint64 sdrSourceEpoch_ = 0;
    quint64 startPositionFrameBaseline_ = 0;
    double pausedSourceTime_ = 0.0;
    qint64 pausedIdentityCapturedAt_ = 0;
    qint64 startSeekAt_ = 0;
    bool pausedIdentityCaptured_ = false;
    bool startSeekIssued_ = false;
    bool startPositionReached_ = false;
    bool startPositionVerified_ = false;
    bool autoCaptured_ = false;
    bool vividCaptured_ = false;
    bool precisionLatticeVerified_ = false;
    bool rollbackShaderRequested_ = false;
    bool hdrShaderPropertyVerified_ = false;
    bool rollbackShaderPropertyRemoved_ = false;
    bool fixesEnabled_ = true;
    bool finished_ = false;
};

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication::setAttribute(Qt::AA_UseDesktopOpenGL);
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    if (!mplayer::probes::selectRuntime()) return 2;
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({QStringLiteral("hdr"), QStringLiteral("generated PQ/Main10 input"), QStringLiteral("file")});
    parser.addOption({QStringLiteral("sdr"), QStringLiteral("generated known-SDR input"), QStringLiteral("file")});
    parser.addOption({QStringLiteral("artifacts"),
                      QStringLiteral("write requested frame captures as PNGs"),
                      QStringLiteral("directory")});
    parser.addOption({QStringLiteral("fbo10"),
                      QStringLiteral("request and verify an RGB10_A2 render target")});
    parser.addOption({QStringLiteral("natural-media"),
                      QStringLiteral("natural footage has no authored-frame identity: record Stable/Vivid "
                                     "effect without requiring a difference")});
    parser.addOption({QStringLiteral("start"),
                      QStringLiteral("seek only the HDR source to this position in seconds"),
                      QStringLiteral("seconds")});
    parser.process(app);
    if (!parser.isSet(QStringLiteral("hdr")) || !parser.isSet(QStringLiteral("sdr"))) {
        std::fprintf(stderr, "usage: hdr_playback_probe --hdr <PQ/Main10> --sdr <BT.709> "
                                                          "[--start <seconds>] [--fbo10] [--natural-media] "
                             "[--artifacts <directory>]\n");
        return 2;
    }
    std::optional<double> startSeconds;
    if (parser.isSet(QStringLiteral("start"))) {
        bool parsed = false;
        const double value = parser.value(QStringLiteral("start")).toDouble(&parsed);
        if (!parsed || !std::isfinite(value) || value < 0.0) {
            std::fprintf(stderr, "hdr_playback_probe: --start requires a finite nonnegative number of seconds\n");
            return 2;
        }
        startSeconds = value;
    }
    HdrAcceptance probe(parser.value(QStringLiteral("hdr")), parser.value(QStringLiteral("sdr")),
                        parser.value(QStringLiteral("artifacts")),
                        parser.isSet(QStringLiteral("fbo10")),
                        parser.isSet(QStringLiteral("natural-media")), startSeconds, app);
    return probe.run();
}
