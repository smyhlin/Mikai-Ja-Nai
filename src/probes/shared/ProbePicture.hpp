#pragma once

#include <QColor>
#include <QImage>
#include <QRect>
#include <QSize>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <cstdio>

#include "VideoSurface.hpp"

namespace mplayer::probes {

// 0x080808 sums to 24: legitimately near-black content counts, 0x000000 does not.
inline constexpr int kLitPixelSum = 12;

// A mostly-black band is ambiguous, so it is a skip, never a pass.
inline constexpr double kMinPictureLitFraction = 0.5;

// mpv_surface_smoke's motion measure: |dR|+|dG|+|dB| > 8 over >= 1% of the band.
inline constexpr int kMotionPixelDelta = 8;
inline constexpr double kMinMotionChangedFraction = 0.01;

// Scale the video into the framebuffer (never the reverse) and centre it: mpv
// letterboxes, and the inverted arguments read out of bounds on an aspect mismatch.
inline QRect pictureRect(const QSize& framebuffer, int videoWidth, int videoHeight) {
    if (framebuffer.isEmpty() || videoWidth <= 0 || videoHeight <= 0) {
        return {};
    }
    const QSize fitted = QSize(videoWidth, videoHeight)
                             .scaled(framebuffer, Qt::KeepAspectRatio)
                             .expandedTo(QSize(1, 1));
    return QRect(QPoint((framebuffer.width() - fitted.width()) / 2,
                        (framebuffer.height() - fitted.height()) / 2),
                 fitted);
}

struct Picture {
    QRect rect;
    long lit = 0;
    long sampled = 0;

    double litFraction() const { return sampled > 0 ? double(lit) / double(sampled) : 0.0; }
    bool isMostlyBlack() const { return sampled == 0 || lit * 2 <= sampled; }
};

inline Picture measurePicture(const QImage& frame, int videoWidth, int videoHeight) {
    Picture picture;
    picture.rect = pictureRect(frame.size(), videoWidth, videoHeight);
    if (frame.isNull() || picture.rect.isEmpty()) {
        return picture;
    }
    for (int y = picture.rect.top(); y <= picture.rect.bottom(); y += 2) {
        for (int x = picture.rect.left(); x <= picture.rect.right(); x += 2) {
            ++picture.sampled;
            const QColor pixel = frame.pixelColor(x, y);
            if (pixel.red() + pixel.green() + pixel.blue() > kLitPixelSum) ++picture.lit;
        }
    }
    return picture;
}

// The surface resets its fields at EOF, so a pre-EOF frame keeps its own geometry.
struct PlaybackSample {
    QImage framebuffer;
    double position = -1.0;
    int videoWidth = 0;
    int videoHeight = 0;
    bool contextReady = false;
    bool hasVideo = false;
    QSize surfaceSize;
    qreal devicePixelRatio = 1.0;

    bool hasPicture() const {
        return !framebuffer.isNull() && videoWidth > 0 && videoHeight > 0;
    }
};

inline PlaybackSample captureSample(VideoSurface& surface) {
    PlaybackSample sample;
    sample.framebuffer = surface.grabVideoFramebuffer();
    sample.position = surface.position();
    sample.videoWidth = surface.videoWidth();
    sample.videoHeight = surface.videoHeight();
    sample.contextReady = surface.isContextReady();
    sample.hasVideo = surface.hasVideo();
    sample.surfaceSize = surface.size();
    sample.devicePixelRatio = surface.devicePixelRatioF();
    return sample;
}

// Two samples on the media clock: a fixed wall-clock schedule reads past a short
// clip's end and pairs a reset position with a black framebuffer.
class PlaybackSampler {
public:
    explicit PlaybackSampler(VideoSurface& surface) : surface_(surface) {}

    bool tick();
    bool complete() const { return complete_; }
    const PlaybackSample& earlier() const { return first_; }
    const PlaybackSample& later() const { return later_; }

private:
    VideoSurface& surface_;
    PlaybackSample first_;
    PlaybackSample later_;
    bool complete_ = false;
};

inline bool PlaybackSampler::tick() {
    if (complete_) return true;
    if (!surface_.isContextReady()) return false;
    if (!surface_.hasVideo()) {
        if (later_.hasPicture() && later_.position > first_.position) complete_ = true;
        return complete_;
    }
    const double position = surface_.position();
    const double duration = surface_.duration();
    if (first_.position < 0.0) {
        const double firstAt =
            duration > 0.0 ? std::min(0.5, std::max(0.1, duration * 0.15)) : 0.5;
        if (position >= firstAt) {
            PlaybackSample sample = captureSample(surface_);
            if (sample.hasPicture()) first_ = sample;
        }
        return false;
    }
    const double advance =
        duration > 0.0 ? std::min(2.0, std::max(0.3, duration * 0.5)) : 2.0;
    if (position >= first_.position + advance) {
        PlaybackSample sample = captureSample(surface_);
        if (sample.hasPicture() || !later_.hasPicture()) later_ = sample;
        complete_ = true;
        return true;
    }
    if (!later_.hasPicture() || position - later_.position >= 0.2) {
        later_ = captureSample(surface_);
    }
    return false;
}

class Verdict {
public:
    void metric(const QString& text) { print("    ", text); }

    void require(bool satisfied, const QString& fact) {
        ++required_;
        print(satisfied ? "ok  " : "FAIL", fact);
        if (!satisfied) {
            failures_ << fact;
            ++failed_;
        }
    }

    void inconclusive(const QString& fact) {
        ++required_;
        print("SKIP", fact);
        skips_ << fact;
    }

    void instrumentation(bool available, const QString& fact) {
        print(available ? "ok  " : "n/a ", fact);
        if (!available) gaps_ << fact;
    }

    int report(const char* probe) const {
        std::fprintf(stdout,
                     "RESULT %s: %s (%d/%d requirements failed, %d skipped, %d instrumentation "
                     "unavailable)\n",
                     probe, failures_.isEmpty() ? (skips_.isEmpty() ? "PASS" : "SKIP") : "FAIL",
                     failed_, required_, int(skips_.size()), int(gaps_.size()));
        for (const QString& failure : failures_) {
            std::fprintf(stdout, "  failed: %s\n", qPrintable(failure));
        }
        for (const QString& skip : skips_) {
            std::fprintf(stdout, "  skipped: %s\n", qPrintable(skip));
        }
        for (const QString& gap : gaps_) {
            std::fprintf(stdout, "  instrumentation unavailable: %s\n", qPrintable(gap));
        }
        std::fflush(stdout);
        if (!failures_.isEmpty()) return 1;
        if (!skips_.isEmpty()) return 77;
        return 0;
    }

private:
    static void print(const char* tag, const QString& text) {
        std::fprintf(stdout, "%s %s\n", tag, qPrintable(text));
        std::fflush(stdout);
    }

    QStringList failures_;
    QStringList skips_;
    QStringList gaps_;
    int required_ = 0;
    int failed_ = 0;
};

inline void requirePlayback(Verdict& verdict, const PlaybackSample& earlier,
                            const PlaybackSample& later) {
    verdict.require(later.contextReady, QStringLiteral("GL context ready"));

    const bool sized = later.videoWidth > 0 && later.videoHeight > 0;
    verdict.require(later.hasVideo && sized,
                    QStringLiteral("video track with source dimensions (%1x%2, hasVideo=%3)")
                        .arg(later.videoWidth)
                        .arg(later.videoHeight)
                        .arg(int(later.hasVideo)));

    const bool captured = !later.framebuffer.isNull() && later.framebuffer.width() > 0 &&
                          later.framebuffer.height() > 0;
    verdict.require(captured, QStringLiteral("framebuffer captured (%1x%2)")
                                  .arg(later.framebuffer.width())
                                  .arg(later.framebuffer.height()));

    // Device pixels with one pixel of slack for ratio rounding.
    const QSize expected(qRound(later.surfaceSize.width() * later.devicePixelRatio),
                         qRound(later.surfaceSize.height() * later.devicePixelRatio));
    const bool ownFrame =
        captured && !expected.isEmpty() &&
        qAbs(later.framebuffer.width() - expected.width()) <= 1 &&
        qAbs(later.framebuffer.height() - expected.height()) <= 1;
    verdict.require(ownFrame,
                    QStringLiteral("framebuffer is the surface's own (%1x%2 vs surface %3x%4 "
                                   "at dpr %5)")
                        .arg(later.framebuffer.width())
                        .arg(later.framebuffer.height())
                        .arg(expected.width())
                        .arg(expected.height())
                        .arg(later.devicePixelRatio));

    verdict.require(earlier.position >= 0.0 && later.position > earlier.position,
                    QStringLiteral("position advanced (%1 -> %2 s)")
                        .arg(earlier.position, 0, 'f', 2)
                        .arg(later.position, 0, 'f', 2));

    const Picture picture =
        measurePicture(later.framebuffer, later.videoWidth, later.videoHeight);
    verdict.metric(QStringLiteral("picture band %1x%2+%3,%4: lit=%5/%6 (%7%)")
                       .arg(picture.rect.width())
                       .arg(picture.rect.height())
                       .arg(picture.rect.x())
                       .arg(picture.rect.y())
                       .arg(picture.lit)
                       .arg(picture.sampled)
                       .arg(100.0 * picture.litFraction(), 0, 'f', 1));
    if (picture.sampled == 0) {
        verdict.require(false, QStringLiteral("picture band has pixels to judge"));
        return;
    }
    if (picture.isMostlyBlack()) {
        verdict.inconclusive(
            QStringLiteral("picture band is mostly black: valid black content and a failed "
                           "render are indistinguishable"));
        return;
    }
    verdict.require(true, QStringLiteral("picture band carries real content (> %1% lit)")
                              .arg(100.0 * kMinPictureLitFraction, 0, 'f', 0));

    // Lit is not proof: a frozen backbuffer stays lit while the clock moves, so
    // the two samples must actually differ inside the same band.
    const bool sameGeometry =
        !earlier.framebuffer.isNull() && earlier.framebuffer.size() == later.framebuffer.size() &&
        earlier.videoWidth == later.videoWidth && earlier.videoHeight == later.videoHeight;
    if (!sameGeometry) {
        verdict.inconclusive(QStringLiteral("samples differ in geometry: motion not comparable"));
        return;
    }
    long changed = 0;
    long compared = 0;
    for (int y = picture.rect.top(); y <= picture.rect.bottom(); y += 2) {
        for (int x = picture.rect.left(); x <= picture.rect.right(); x += 2) {
            ++compared;
            const QColor before = earlier.framebuffer.pixelColor(x, y);
            const QColor after = later.framebuffer.pixelColor(x, y);
            if (qAbs(after.red() - before.red()) + qAbs(after.green() - before.green()) +
                    qAbs(after.blue() - before.blue()) > kMotionPixelDelta) {
                ++changed;
            }
        }
    }
    verdict.metric(QStringLiteral("motion band: changed=%1/%2 (%3%)")
                       .arg(changed)
                       .arg(compared)
                       .arg(compared > 0 ? 100.0 * double(changed) / double(compared) : 0.0, 0, 'f',
                            2));
    if (compared == 0) {
        verdict.inconclusive(QStringLiteral("no pixels to compare between samples"));
    } else if (double(changed) >= kMinMotionChangedFraction * double(compared)) {
        verdict.require(true, QStringLiteral("picture moves between samples (>= 1% of the band "
                                             "changed)"));
    } else {
        verdict.inconclusive(QStringLiteral("pixels did not move between samples: static content "
                                            "or a frozen backbuffer"));
    }
}

}  // namespace mplayer::probes
