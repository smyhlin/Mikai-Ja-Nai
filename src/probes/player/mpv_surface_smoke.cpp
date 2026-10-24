// Two contracts on one real surface: the single-clip proof, and
// --render-lifecycle, which drives the lifecycle/presentation transitions.
// --dpr applies QT_SCALE_FACTOR before QApplication is constructed: Qt 6 has no
// per-widget device pixel ratio, and the framebuffer dimensions are what the
// lifecycle stages judge.

#include <QApplication>
#include <QByteArray>
#include <QColor>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFont>
#include <QImage>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QPainter>
#include <QRect>
#include <QSize>
#include <QString>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QWidget>
#include <QWindow>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <vector>

#include "ProbePicture.hpp"
#include "ProbeRuntime.hpp"
#include "VideoSurface.hpp"

namespace {

// ── options, shared by both contracts ─────────────────────────────────────

struct Options {
    bool renderLifecycle = false;
    double dpr = 0.0;
    QString outPrefix = QStringLiteral("mpv_surface_lifecycle");
    QString source;
};

void usage() {
    std::fprintf(stderr,
                 "usage: mpv_surface_smoke [target]\n"
                 "       mpv_surface_smoke --render-lifecycle [media] [--dpr <ratio>] "
                 "[--out <prefix>]\n"
                 "  --render-lifecycle runs the presentation/lifecycle contract instead of the "
                 "single-clip proof.\n"
                 "  --dpr sets QT_SCALE_FACTOR before QApplication; Qt multiplies the monitor's "
                 "native ratio by this factor.\n"
                 "  default media for the lifecycle contract: "
                 "av://lavfi:testsrc2=size=640x360:rate=25\n"
                 "  --out prefixes the screenshot artifacts (default mpv_surface_lifecycle).\n");
}

// Only the harness's own flags are consumed here; everything else reaches Qt.
Options parseOptions(int argc, char** argv, std::vector<char*>* passthrough) {
    Options options;
    passthrough->clear();
    if (argc > 0) passthrough->push_back(argv[0]);
    for (int index = 1; index < argc; ++index) {
        const QString argument = QString::fromLocal8Bit(argv[index]);
        if (argument == QLatin1String("--render-lifecycle")) {
            options.renderLifecycle = true;
        } else if (argument == QLatin1String("--help") || argument == QLatin1String("-h")) {
            usage();
            std::exit(0);
        } else if (argument == QLatin1String("--dpr") || argument == QLatin1String("--out")) {
            if (index + 1 >= argc) {
                std::fprintf(stderr, "mpv_surface_smoke: %s needs a value\n",
                             qPrintable(argument));
                usage();
                std::exit(2);
            }
            const QString value = QString::fromLocal8Bit(argv[++index]);
            if (argument == QLatin1String("--dpr")) {
                bool numeric = false;
                const double parsed = value.toDouble(&numeric);
                if (!numeric || !std::isfinite(parsed) || !(parsed > 0.0)) {
                    std::fprintf(stderr,
                                 "mpv_surface_smoke: --dpr needs a finite, positive ratio "
                                 "(got \"%s\")\n",
                                 qPrintable(value));
                    std::exit(2);
                }
                options.dpr = parsed;
            } else {
                options.outPrefix = value;
            }
        } else {
            passthrough->push_back(argv[index]);
            if (!argument.startsWith(QLatin1Char('-')) && options.source.isEmpty()) {
                options.source = argument;
            }
        }
    }
    return options;
}

// ── lifecycle plumbing ────────────────────────────────────────────────────

void pumpEvents(int ms) {
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(2);
    }
}

template <typename Predicate>
bool waitUntil(Predicate ready, int timeoutMs) {
    QElapsedTimer clock;
    clock.start();
    while (!ready() && clock.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(2);
    }
    return ready();
}

// Counts Qt's own GL entry points, so a stage rests on paints and context
// recreations that really happened.
class LifecycleSurface : public mplayer::VideoSurface {
public:
    explicit LifecycleSurface(QWidget* parent = nullptr) : mplayer::VideoSurface(parent) {}

    quint64 paints = 0;
    int glInits = 0;
    int resizes = 0;
    QSize lastResizeLogical;

protected:
    void initializeGL() override {
        ++glInits;
        mplayer::VideoSurface::initializeGL();
    }
    void resizeGL(int width, int height) override {
        ++resizes;
        lastResizeLogical = QSize(width, height);
        mplayer::VideoSurface::resizeGL(width, height);
    }
    void paintGL() override {
        ++paints;
        mplayer::VideoSurface::paintGL();
    }
};

// Its own paint count keeps the unrelated-repaint stage from being vacuous.
class RepaintCounter : public QWidget {
public:
    explicit RepaintCounter(QWidget* parent = nullptr) : QWidget(parent) {}

    int repaints = 0;

protected:
    void paintEvent(QPaintEvent*) override {
        ++repaints;
        QPainter painter(this);
        painter.fillRect(rect(), QColor(18, 84, 148));
        painter.setPen(QColor(255, 255, 255));
        painter.drawText(rect(), Qt::AlignCenter, QStringLiteral("unrelated"));
    }
};

// An opaque colour no fixture carries, plus real glyphs: the painter's own
// commands, identifiable in the framebuffer.
const QColor kOverlayFill(255, 0, 255);
constexpr int kOverlayLogicalHeight = 26;
constexpr int kOverlayColourTolerance = 9;

struct OverlayBand {
    long interior = 0;
    long fill = 0;
    long glyph = 0;

    double fillFraction() const { return interior > 0 ? double(fill) / double(interior) : 0.0; }
};

// The band in device rows, with one row of slack at each edge: a boundary pixel
// blended into the video decides nothing.
QRect overlayRows(const QImage& frame, qreal dpr) {
    if (frame.isNull()) return {};
    const int rows = std::min(frame.height(), std::max(1, qRound(kOverlayLogicalHeight * dpr)));
    return QRect(0, 1, frame.width(), std::max(0, rows - 2));
}

OverlayBand measureOverlayBand(const QImage& frame, qreal dpr) {
    OverlayBand band;
    const QRect rows = overlayRows(frame, dpr);
    if (rows.isEmpty()) return band;
    for (int y = rows.top(); y <= rows.bottom(); ++y) {
        for (int x = rows.left(); x <= rows.right(); ++x) {
            const QColor pixel = frame.pixelColor(x, y);
            ++band.interior;
            const int distance = std::abs(pixel.red() - kOverlayFill.red()) +
                                 std::abs(pixel.green() - kOverlayFill.green()) +
                                 std::abs(pixel.blue() - kOverlayFill.blue());
            if (distance <= kOverlayColourTolerance) {
                ++band.fill;
            } else {
                ++band.glyph;
            }
        }
    }
    return band;
}

struct PictureStats {
    QRect region;
    long sampled = 0;
    long lit = 0;
    long changed = 0;
    long compared = 0;

    double litFraction() const { return sampled > 0 ? double(lit) / double(sampled) : 0.0; }
    double changedFraction() const {
        return compared > 0 ? double(changed) / double(compared) : 0.0;
    }
};

// The video band below the HUD: the harness's own paint takes part in no motion
// judgement.
QRect pictureRegion(const QImage& frame, int videoWidth, int videoHeight, qreal dpr) {
    if (frame.isNull()) return {};
    const QRect rows = overlayRows(frame, dpr);
    const QRect below(0, rows.bottom() + 1, frame.width(), frame.height() - rows.bottom() - 1);
    return mplayer::probes::pictureRect(frame.size(), videoWidth, videoHeight)
        .intersected(below);
}

PictureStats statsOf(const QImage& frame, const QImage& before, int videoWidth, int videoHeight,
                     qreal dpr) {
    PictureStats stats;
    stats.region = pictureRegion(frame, videoWidth, videoHeight, dpr);
    if (stats.region.isEmpty()) return stats;
    // A pair only exists at one geometry: a resize between the grabs compares
    // unrelated pixels.
    const bool pair = !before.isNull() && before.size() == frame.size();
    for (int y = stats.region.top(); y <= stats.region.bottom(); y += 2) {
        for (int x = stats.region.left(); x <= stats.region.right(); x += 2) {
            ++stats.sampled;
            const QColor pixel = frame.pixelColor(x, y);
            if (pixel.red() + pixel.green() + pixel.blue() > mplayer::probes::kLitPixelSum) {
                ++stats.lit;
            }
            if (!pair) continue;
            const QColor previous = before.pixelColor(x, y);
            ++stats.compared;
            if (std::abs(pixel.red() - previous.red()) +
                    std::abs(pixel.green() - previous.green()) +
                    std::abs(pixel.blue() - previous.blue()) >
                mplayer::probes::kMotionPixelDelta) {
                ++stats.changed;
            }
        }
    }
    return stats;
}

// One pixel of slack for Qt's rounding; the logical size at a ratio above 1 is
// still a mismatch, not a rounding.
bool framebufferMatches(const QSize& actual, const QSize& logical, qreal dpr) {
    if (actual.isEmpty() || logical.isEmpty()) return false;
    return std::abs(actual.width() - qRound(logical.width() * dpr)) <= 1 &&
           std::abs(actual.height() - qRound(logical.height() * dpr)) <= 1;
}

bool saveArtifact(const QString& path, const QImage& frame) {
    if (frame.isNull()) return false;
    const bool saved = frame.save(path);
    std::fprintf(stderr, "artifact %s %dx%d dpr=%.2f %s\n", qPrintable(path), frame.width(),
                 frame.height(), frame.devicePixelRatio(),
                 saved ? "written" : "FAILED");
    return saved;
}

int runRenderLifecycle(const Options& options) {
    using namespace mplayer::probes;

    Verdict verdict;
    const QString source =
        options.source.isEmpty() ? QStringLiteral("av://lavfi:testsrc2=size=640x360:rate=25")
                                 : options.source;
    std::fprintf(stderr, "render lifecycle target=%s\n", qPrintable(source));

    // The strip shares this mapped window with the surface, so an unrelated
    // repaint is one that really happens; on top because a covered window stops
    // being exposed and the paint decisions would then be about the desktop.
    QWidget host;
    host.setWindowFlag(Qt::WindowStaysOnTopHint, true);
    host.setWindowTitle(QStringLiteral("mpv_surface_smoke render lifecycle"));
    host.resize(640, 420);
    LifecycleSurface* surface = new LifecycleSurface(&host);
    surface->setGeometry(0, 60, 640, 360);
    RepaintCounter* strip = new RepaintCounter(&host);
    strip->setGeometry(0, 0, 640, 60);
    strip->show();

    QString mediaError;
    int mediaErrorCount = 0;
    bool expectingSourceFailure = false;
    bool sourceFailureSeen = false;
    QObject::connect(surface, &mplayer::VideoSurface::errorOccurred, surface,
                     [&](const QString& message) {
                         if (expectingSourceFailure) {
                             sourceFailureSeen = true;
                             std::fprintf(stderr, "expected source failure: %s\n", qPrintable(message));
                             return;
                         }
                         mediaError = message;
                         ++mediaErrorCount;
                         std::fprintf(stderr, "media error: %s\n", qPrintable(message));
                     });

    QObject::connect(surface, &mplayer::VideoSurface::paintOverlay, surface,
                     [surface](QPainter* painter) {
                         if (painter == nullptr) return;
                         // Logical coordinates: the contract's space, ratio applied
                         // by the engine.
                         const QRectF band(0, 0, surface->width(), kOverlayLogicalHeight);
                         painter->fillRect(band, kOverlayFill);
                         QFont font = painter->font();
                         font.setBold(true);
                         font.setPixelSize(14);
                         painter->setFont(font);
                         painter->setPen(QColor(255, 255, 255));
                         painter->drawText(band.adjusted(6, 0, -6, 0),
                                           int(Qt::AlignLeft) | int(Qt::AlignVCenter),
                                           QStringLiteral("MIKAI HUD 0123456789"));
                     },
                     Qt::DirectConnection);

    // A request made before the first show must retain both its URL and pause
    // intent until Qt creates the GL context and the core accepts the file.
    {
        LifecycleSurface deferred;
        deferred.setWindowFlag(Qt::WindowStaysOnTopHint, true);
        deferred.resize(320, 180);
        const bool accepted = deferred.open(source);
        deferred.setPaused(true);
        verdict.require(accepted, QStringLiteral("a pre-show media request was accepted"));
        deferred.show();
        const bool deferredPaused = accepted && waitUntil(
            [&] {
                return deferred.isContextReady() && deferred.hasVideo() &&
                       deferred.videoWidth() > 0 && deferred.pauseObserved() &&
                       !deferred.isPlaying();
            },
            8000);
        verdict.require(deferredPaused,
                        QStringLiteral("pre-show open creates video and applies deferred pause"));
        if (deferredPaused) {
            const double parkedAt = deferred.position();
            pumpEvents(350);
            verdict.require(deferred.pauseObserved() && !deferred.isPlaying() &&
                                std::abs(deferred.position() - parkedAt) <= 0.12,
                            QStringLiteral("the deferred pre-show pause keeps the core parked"));
        }
    }

    host.show();
    const auto renderedFrames = [surface] {
        return surface->playbackDiagnostics().renderedOutputFrames;
    };

    host.show();
    host.raise();
    host.activateWindow();
    if (!surface->open(source)) {
        verdict.require(false, QStringLiteral("the surface accepted %1").arg(source));
        return verdict.report("mpv_surface_smoke:render-lifecycle");
    }

    const qreal dpr = surface->devicePixelRatioF();
    verdict.metric(QStringLiteral("device pixel ratio %1").arg(dpr, 0, 'f', 2));
    verdict.instrumentation(std::abs(dpr - std::round(dpr)) > 0.001,
                            QStringLiteral("fractional device pixel ratio in effect"));

    // ── a live, moving, colourful picture ─────────────────────────────────
    const bool live = waitUntil(
        [&] {
            return surface->isContextReady() && surface->hasVideo() &&
                   surface->videoWidth() > 0 && renderedFrames() >= 3;
        },
        8000);
    verdict.require(surface->isContextReady(), QStringLiteral("GL context ready"));
    verdict.require(live, QStringLiteral("three output frames were rendered within 8 s"));
    if (!live) return verdict.report("mpv_surface_smoke:render-lifecycle");
    verdict.require(surface->videoWidth() > 0 && surface->videoHeight() > 0,
                    QStringLiteral("source geometry %1x%2")
                        .arg(surface->videoWidth())
                        .arg(surface->videoHeight()));

    // Read once, and from the context itself: this machine has selected llvmpipe
    // without QT_OPENGL=desktop, and every pixel claim below is about whichever
    // renderer actually got the surface.
    if (surface->isContextReady()) {
        surface->makeCurrent();
        if (QOpenGLFunctions* gl = surface->context()->functions()) {
            const auto readString = [gl](GLenum name) {
                const GLubyte* bytes = gl->glGetString(name);
                return bytes != nullptr
                           ? QByteArray(reinterpret_cast<const char*>(bytes))
                           : QByteArray("unavailable");
            };
            const QByteArray vendor = readString(GL_VENDOR);
            const QByteArray renderer = readString(GL_RENDERER);
            const QByteArray version = readString(GL_VERSION);
            std::fprintf(stderr, "GL vendor=%s renderer=%s version=%s\n", vendor.constData(),
                         renderer.constData(), version.constData());
        }
        surface->doneCurrent();
    }

    const auto decoder = surface->probeMpvProperty(QStringLiteral("hwdec-current"),
                                                   mplayer::ProbePropertyKind::Text);
    const QString decoderName =
        decoder && decoder->kind == mplayer::ProbePropertyValue::Kind::Text
            ? decoder->text : QStringLiteral("unavailable");
    std::fprintf(stderr, "hwdec-current=%s\n", qPrintable(decoderName));

    const quint64 framesAt = renderedFrames();
    const QImage playing = surface->grabFramebuffer();
    waitUntil([&] { return renderedFrames() >= framesAt + 4; }, 3000);
    const QImage playingLater = surface->grabFramebuffer();
    const PictureStats motion =
        statsOf(playingLater, playing, surface->videoWidth(), surface->videoHeight(), dpr);
    verdict.require(!playing.isNull() && playing.size() == playingLater.size(),
                    QStringLiteral("two framebuffers at one geometry (%1x%2)")
                        .arg(playingLater.width())
                        .arg(playingLater.height()));
    verdict.require(motion.litFraction() >= kMinPictureLitFraction,
                    QStringLiteral("the playing picture is lit (%1% of the band)")
                        .arg(100.0 * motion.litFraction(), 0, 'f', 1));
    verdict.require(motion.changedFraction() >= kMinMotionChangedFraction,
                    QStringLiteral("the playing picture moves (%1% of the band changed)")
                        .arg(100.0 * motion.changedFraction(), 0, 'f', 2));
    verdict.require(saveArtifact(options.outPrefix + QStringLiteral("_playing.png"), playingLater),
                    QStringLiteral("artifact %1_playing.png written").arg(options.outPrefix));

    // PNG writes above can leave the published HUD clock stale.
    const auto positionAt = surface->probeMpvProperty(
        QStringLiteral("time-pos"), mplayer::ProbePropertyKind::Number);
    QElapsedTimer wall;
    wall.start();
    pumpEvents(1200);
    const auto positionAfter = surface->probeMpvProperty(
        QStringLiteral("time-pos"), mplayer::ProbePropertyKind::Number);
    const double elapsed = wall.elapsed() / 1000.0;
    const bool clockReadable = positionAt && positionAt->isNumeric() &&
                               positionAfter && positionAfter->isNumeric();
    verdict.require(clockReadable, QStringLiteral("both media-clock endpoints came from the core"));
    if (clockReadable) {
        const double advanced = positionAfter->asNumber() - positionAt->asNumber();
        verdict.metric(QStringLiteral("media clock +%1 s over %2 s of wall clock")
                           .arg(advanced, 0, 'f', 3)
                           .arg(elapsed, 0, 'f', 3));
        verdict.require(std::abs(advanced - elapsed) <= 0.25,
                        QStringLiteral("the media clock tracks the wall clock (drift %1 s)")
                            .arg(advanced - elapsed, 0, 'f', 3));
    }

    // ── paused repaints with the HUD on screen ────────────────────────────
    surface->setPaused(true);
    verdict.require(
        waitUntil([&] { return surface->pauseObserved() && !surface->isPlaying(); }, 3000),
        QStringLiteral("the core observed the pause"));
    pumpEvents(400);
    surface->update();
    pumpEvents(200);
    const QImage parked = surface->grabFramebuffer();
    const PictureStats parkedPicture =
        statsOf(parked, QImage(), surface->videoWidth(), surface->videoHeight(), dpr);
    verdict.require(parkedPicture.litFraction() >= kMinPictureLitFraction,
                    QStringLiteral("the parked frame keeps a lit picture (%1%)")
                        .arg(100.0 * parkedPicture.litFraction(), 0, 'f', 1));

    const OverlayBand parkedBand = measureOverlayBand(parked, dpr);
    verdict.require(parkedBand.fillFraction() >= 0.80 && parkedBand.glyph >= 60,
                    QStringLiteral("the parked frame carries the HUD band (fill %1%, glyph %2 px)")
                        .arg(100.0 * parkedBand.fillFraction(), 0, 'f', 1)
                        .arg(parkedBand.glyph));

    // How much of the band differs between two readbacks of the same parked frame.
    const auto bandChanged = [&](const QImage& after, const QImage& before) {
        if (after.isNull() || before.isNull() || after.size() != before.size()) return -1.0;
        const QRect rows = overlayRows(before, dpr);
        if (rows.isEmpty()) return -1.0;
        long changed = 0;
        long compared = 0;
        for (int y = rows.top(); y <= rows.bottom(); ++y) {
            for (int x = rows.left(); x <= rows.right(); x += 2) {
                ++compared;
                const QColor left = after.pixelColor(x, y);
                const QColor right = before.pixelColor(x, y);
                if (std::abs(left.red() - right.red()) + std::abs(left.green() - right.green()) +
                        std::abs(left.blue() - right.blue()) >
                    mplayer::probes::kMotionPixelDelta) {
                    ++changed;
                }
            }
        }
        return compared > 0 ? double(changed) / double(compared) : -1.0;
    };

    // A video-only readback must not contain the HUD: what comparisons and
    // clipboard frames read, and what the harness's own suppression used to
    // stand in for. Same criterion, now on the production path.
    const QImage clean = surface->grabVideoFramebuffer();
    verdict.require(!clean.isNull() && clean.size() == parked.size(),
                    QStringLiteral("the video-only readback has the parked framebuffer's geometry "
                                   "(%1x%2 vs %3x%4)")
                        .arg(clean.width())
                        .arg(clean.height())
                        .arg(parked.width())
                        .arg(parked.height()));
    const OverlayBand cleanBand = measureOverlayBand(clean, dpr);
    verdict.require(cleanBand.fillFraction() < 0.5,
                    QStringLiteral("the video-only readback carries no HUD band (%1% of the band "
                                   "is the overlay's colour)")
                        .arg(100.0 * cleanBand.fillFraction(), 0, 'f', 1));
    verdict.require(cleanBand.fill * 4 < parkedBand.fill,
                    QStringLiteral("the video-only readback keeps at most a quarter of the HUD's "
                                   "overlay-coloured pixels (%1 vs %2)")
                        .arg(cleanBand.fill)
                        .arg(parkedBand.fill));
    verdict.require(bandChanged(clean, parked) >= 0.5,
                    QStringLiteral("the video-only readback's band is not the HUD band (%1% of the "
                                   "band differs)")
                        .arg(100.0 * bandChanged(clean, parked), 0, 'f', 1));
    const PictureStats cleanVideo =
        statsOf(clean, parked, surface->videoWidth(), surface->videoHeight(), dpr);
    verdict.require(cleanVideo.compared > 0 && cleanVideo.changedFraction() <= 0.02,
                    QStringLiteral("the video-only readback left the parked picture unchanged "
                                   "(%1% changed)")
                        .arg(100.0 * cleanVideo.changedFraction(), 0, 'f', 2));
    verdict.require(saveArtifact(options.outPrefix + QStringLiteral("_video_only.png"), clean),
                    QStringLiteral("artifact %1_video_only.png written").arg(options.outPrefix));

    // Suppressing the overlay for a readback must restore it, not consume it: the
    // next ordinary paint has to show the same HUD again.
    surface->update();
    pumpEvents(200);
    const QImage hudRestored = surface->grabFramebuffer();
    const OverlayBand restoredBand = measureOverlayBand(hudRestored, dpr);
    verdict.require(restoredBand.fillFraction() >= 0.80 && restoredBand.glyph >= 60,
                    QStringLiteral("the HUD is back on the next paint after the video-only "
                                   "readback (fill %1%, glyph %2 px)")
                        .arg(100.0 * restoredBand.fillFraction(), 0, 'f', 1)
                        .arg(restoredBand.glyph));
    const double restoredBandChange = bandChanged(hudRestored, parked);
    verdict.require(restoredBandChange >= 0.0 && restoredBandChange <= 0.10,
                    QStringLiteral("the restoring paint is the same HUD band again (%1% of the band "
                                   "differs)")
                        .arg(100.0 * restoredBandChange, 0, 'f', 1));
    const PictureStats restoredDrift =
        statsOf(hudRestored, parked, surface->videoWidth(), surface->videoHeight(), dpr);
    verdict.require(restoredDrift.compared > 0 && restoredDrift.changedFraction() <= 0.02,
                    QStringLiteral("the parked picture is untouched across the readback and the "
                                   "restoring paint (%1% changed)")
                        .arg(100.0 * restoredDrift.changedFraction(), 0, 'f', 2));

    const quint64 framesParked = renderedFrames();
    QImage lastRepaint;
    for (int repaint = 1; repaint <= 4; ++repaint) {
        const quint64 paintsBefore = surface->paints;
        surface->update();
        pumpEvents(160);
        // Read before the grab: grabFramebuffer() paints on its own and would
        // otherwise hide an update() that scheduled nothing.
        const quint64 paintsAfterUpdate = surface->paints;
        lastRepaint = surface->grabFramebuffer();
        verdict.require(paintsAfterUpdate >= paintsBefore + 1,
                        QStringLiteral("paused repaint %1 was driven by update() alone (paints "
                                       "%2 -> %3)")
                            .arg(repaint)
                            .arg(paintsBefore)
                            .arg(paintsAfterUpdate));
        const OverlayBand band = measureOverlayBand(lastRepaint, dpr);
        verdict.require(band.fillFraction() >= 0.80 && band.glyph >= 60,
                        QStringLiteral("paused repaint %1 carries the HUD band (fill %2%, glyph "
                                       "%3 px)")
                            .arg(repaint)
                            .arg(100.0 * band.fillFraction(), 0, 'f', 1)
                            .arg(band.glyph));
        const PictureStats drift = statsOf(lastRepaint, parked, surface->videoWidth(),
                                          surface->videoHeight(), dpr);
        verdict.require(drift.compared > 0 && drift.changedFraction() <= 0.02,
                        QStringLiteral("paused repaint %1 kept the parked picture (changed %2%)")
                            .arg(repaint)
                            .arg(100.0 * drift.changedFraction(), 0, 'f', 2));
    }
    verdict.require(renderedFrames() <= framesParked + 1,
                    QStringLiteral("a paused repaint rendered no new output frame (%1 -> %2)")
                        .arg(framesParked)
                        .arg(renderedFrames()));
    verdict.require(saveArtifact(options.outPrefix + QStringLiteral("_paused_hud.png"),
                                 lastRepaint),
                    QStringLiteral("artifact %1_paused_hud.png written").arg(options.outPrefix));

    // A paused core cannot distinguish a retained snapshot from a cached-frame redraw.
    surface->setPaused(false);
    const bool resumedForHold = waitUntil([&] { return !surface->pauseObserved(); }, 4000);
    verdict.require(resumedForHold, QStringLiteral("playback resumed for the hold freeze check"));
    pumpEvents(300);
    const QImage motionStart = surface->grabFramebuffer();
    pumpEvents(400);
    const QImage motionEnd = surface->grabFramebuffer();
    const PictureStats holdMotion =
        statsOf(motionEnd, motionStart, surface->videoWidth(), surface->videoHeight(), dpr);
    if (resumedForHold && holdMotion.compared > 0 &&
        holdMotion.changedFraction() >= kMinMotionChangedFraction) {
        const double positionBeforeHold = surface->position();
        surface->setEngineFrameHold(true);
        // Acquisition may consume one pending frame; measure stability from the acquired picture.
        const QImage acquiredHold = surface->grabFramebuffer();
        pumpEvents(500);
        verdict.require(surface->position() >= positionBeforeHold + 0.25,
                        QStringLiteral("the media clock advances while the picture is held"));
        const QImage held = surface->grabFramebuffer();
        const PictureStats frozen =
            statsOf(held, acquiredHold, surface->videoWidth(), surface->videoHeight(), dpr);
        const double freezeTolerance = std::min(0.02, holdMotion.changedFraction() / 4.0);
        verdict.require(frozen.compared > 0 && frozen.changedFraction() <= freezeTolerance,
                        QStringLiteral("a transition hold freezes the live picture (%1% changed)")
                            .arg(100.0 * frozen.changedFraction(), 0, 'f', 2));
        surface->setEngineFrameHold(false);
    } else {
        verdict.inconclusive(
            QStringLiteral("transition-hold freeze needs observable motion (%1% changed)")
                .arg(100.0 * holdMotion.changedFraction(), 0, 'f', 2));
    }
    surface->setPaused(true);
    verdict.require(waitUntil([&] { return surface->pauseObserved(); }, 3000),
                    QStringLiteral("the player returned to pause before the paused hold check"));

    // Exercise the transition hold without requiring a TensorRT runtime: the
    // paused picture must survive HUD-only paints, then a real output frame must
    // replace it after the hold is released.
    const QImage beforeHold = surface->grabFramebuffer();
    surface->setEngineFrameHold(true);
    surface->update();
    pumpEvents(200);
    const QImage held = surface->grabFramebuffer();
    const PictureStats heldPicture =
        statsOf(held, beforeHold, surface->videoWidth(), surface->videoHeight(), dpr);
    verdict.require(heldPicture.compared > 0 && heldPicture.changedFraction() <= 0.02,
                    QStringLiteral("a transition hold keeps the paused video unchanged through a "
                                   "HUD repaint (%1% changed)")
                        .arg(100.0 * heldPicture.changedFraction(), 0, 'f', 2));
    surface->setEngineFrameHold(false);
    surface->update();
    pumpEvents(200);
    const QImage waitingForOutput = surface->grabFramebuffer();
    const PictureStats waitingPicture =
        statsOf(waitingForOutput, held, surface->videoWidth(), surface->videoHeight(), dpr);
    verdict.require(waitingPicture.compared > 0 && waitingPicture.changedFraction() <= 0.02,
                    QStringLiteral("releasing while paused keeps the snapshot until fresh output "
                                   "(%1% changed)")
                        .arg(100.0 * waitingPicture.changedFraction(), 0, 'f', 2));

    const quint64 framesBeforeHandoff = renderedFrames();
    surface->setPaused(false);
    const bool handedOff = waitUntil(
        [&] {
            return !surface->pauseObserved() &&
                   renderedFrames() >= framesBeforeHandoff + 3;
        },
        4000);
    verdict.require(handedOff,
                    QStringLiteral("the released hold hands off to three fresh output frames"));
    if (handedOff) {
        const QImage fresh = surface->grabFramebuffer();
        const PictureStats replacement =
            statsOf(fresh, held, surface->videoWidth(), surface->videoHeight(), dpr);
        verdict.require(replacement.changedFraction() >= kMinMotionChangedFraction,
                        QStringLiteral("fresh output replaces the retained picture (%1% changed)")
                            .arg(100.0 * replacement.changedFraction(), 0, 'f', 2));
    }
    surface->setPaused(true);
    verdict.require(waitUntil([&] { return surface->pauseObserved(); }, 3000),
                    QStringLiteral("the player returned to pause after the hold handoff"));

    const double beforeDoubleToggle = surface->position();
    surface->togglePause();
    surface->togglePause();
    pumpEvents(250);
    verdict.require(surface->pauseObserved() && !surface->isPlaying() &&
                        std::abs(surface->position() - beforeDoubleToggle) <= 0.10,
                    QStringLiteral("two rapid toggles preserve pause and the parked timestamp"));

    // ── an unrelated repaint drives nothing ───────────────────────────────
    const quint64 paintsBeforeStrip = surface->paints;
    const quint64 framesBeforeStrip = renderedFrames();
    // The top strip stays inside a host Windows may clamp to a small desktop.
    const QPoint stripOrigin = strip->mapToGlobal(QPoint(0, 0));
    const QRect stripLocal = strip->geometry();
    QWindow* hostWindow = host.windowHandle();
    std::fprintf(stderr,
                 "unrelated strip: %dx%d local %d,%d global %d,%d visible=%d visibleToHost=%d "
                 "updates=%d host %dx%d windowOpen=%d exposed=%d surface %d,%d %dx%d\n",
                 strip->width(), strip->height(), stripLocal.x(), stripLocal.y(), stripOrigin.x(),
                 stripOrigin.y(), int(strip->isVisible()), int(strip->isVisibleTo(&host)),
                 int(strip->updatesEnabled()), host.width(), host.height(),
                 int(hostWindow != nullptr),
                 int(hostWindow != nullptr && hostWindow->isExposed()), surface->x(), surface->y(),
                 surface->width(), surface->height());
    int stripUpdatePaints = 0;
    for (int round = 1; round <= 3; ++round) {
        const int before = strip->repaints;
        strip->update();
        pumpEvents(200);
        stripUpdatePaints += strip->repaints - before;
        if (strip->repaints == before) strip->repaint();
        std::fprintf(stderr, "unrelated strip round %d: repaints=%d\n", round, strip->repaints);
    }
    verdict.metric(QStringLiteral("unrelated widget: %1 paints from update(), %2 in total")
                       .arg(stripUpdatePaints)
                       .arg(strip->repaints));
    verdict.require(strip->repaints >= 2,
                    QStringLiteral("the unrelated widget repainted (%1 times)")
                        .arg(strip->repaints));
    verdict.require(surface->paints == paintsBeforeStrip,
                    QStringLiteral("an unrelated repaint left the surface unpainted (paints %1 "
                                   "-> %2)")
                        .arg(paintsBeforeStrip)
                        .arg(surface->paints));
    verdict.require(renderedFrames() == framesBeforeStrip,
                    QStringLiteral("an unrelated repaint rendered no output frame (%1 -> %2)")
                        .arg(framesBeforeStrip)
                        .arg(renderedFrames()));

    // ── hidden while playing, then restored ───────────────────────────────
    surface->setPaused(false);
    verdict.require(waitUntil([&] { return !surface->pauseObserved(); }, 3000),
                    QStringLiteral("playback resumed after the pause"));
    verdict.require(waitUntil([&] { return renderedFrames() >= framesBeforeStrip + 4; }, 4000),
                    QStringLiteral("four output frames were rendered after the resume"));
    const QImage beforeHide = surface->grabFramebuffer();
    const double hiddenPositionAt = surface->position();
    const quint64 paintsBeforeHide = surface->paints;
    const quint64 framesBeforeHide = renderedFrames();
    surface->hide();
    pumpEvents(1200);
    const double hiddenAdvance = surface->position() - hiddenPositionAt;
    verdict.metric(QStringLiteral("while hidden: clock +%1 s, paints +%2, output frames +%3")
                       .arg(hiddenAdvance, 0, 'f', 3)
                       .arg(surface->paints - paintsBeforeHide)
                       .arg(renderedFrames() - framesBeforeHide));
    verdict.require(surface->paints == paintsBeforeHide,
                    QStringLiteral("a hidden surface did not paint (%1 -> %2)")
                        .arg(paintsBeforeHide)
                        .arg(surface->paints));
    verdict.require(renderedFrames() == framesBeforeHide,
                    QStringLiteral("a hidden surface rendered no output frame (%1 -> %2)")
                        .arg(framesBeforeHide)
                        .arg(renderedFrames()));

    surface->show();
    verdict.require(waitUntil([&] { return renderedFrames() >= framesBeforeHide + 3; }, 4000),
                    QStringLiteral("the restored surface rendered new output frames"));
    // Progress is required across the whole hidden window, not from the clock the
    // mirror happened to publish while nothing was on screen: a hidden surface
    // that stops the film is what this catches.
    const double restoredAdvance = surface->position() - hiddenPositionAt;
    verdict.require(restoredAdvance >= 0.9,
                    QStringLiteral("the film advanced across the hidden window (+%1 s while it "
                                   "was hidden for 1.2 s)")
                        .arg(restoredAdvance, 0, 'f', 3));
    const QImage afterHide = surface->grabFramebuffer();
    const PictureStats restored =
        statsOf(afterHide, beforeHide, surface->videoWidth(), surface->videoHeight(), dpr);
    verdict.require(restored.litFraction() >= kMinPictureLitFraction,
                    QStringLiteral("the restored picture is lit (%1%)")
                        .arg(100.0 * restored.litFraction(), 0, 'f', 1));
    verdict.require(restored.changedFraction() >= kMinMotionChangedFraction,
                    QStringLiteral("the restored picture is not the pre-hide frame (%1% changed)")
                        .arg(100.0 * restored.changedFraction(), 0, 'f', 2));
    verdict.require(saveArtifact(options.outPrefix + QStringLiteral("_hidden_restore.png"),
                                 afterHide),
                    QStringLiteral("artifact %1_hidden_restore.png written").arg(options.outPrefix));

    auto* cover = new QWidget(&host);
    cover->setGeometry(surface->geometry());
    cover->setAutoFillBackground(true);
    cover->setAttribute(Qt::WA_OpaquePaintEvent);
    cover->show();
    cover->raise();
    pumpEvents(200);
    verdict.require(surface->isVisible() && surface->visibleRegion().isEmpty(),
                    QStringLiteral("an opaque sibling covers the still-visible video widget"));
    const quint64 occludedPaints = surface->paints;
    const quint64 occludedFrames = renderedFrames();
    const double occludedAt = surface->position();
    QElapsedTimer occludedWall;
    occludedWall.start();
    pumpEvents(1200);
    const double occludedAdvance = surface->position() - occludedAt;
    verdict.require(surface->paints == occludedPaints && renderedFrames() == occludedFrames,
                    QStringLiteral("a fully occluded video widget performs no video paints"));
    verdict.require(std::abs(occludedAdvance - occludedWall.elapsed() / 1000.0) <= 0.25,
                    QStringLiteral("playback keeps advancing while an opaque sibling covers video"));
    delete cover;
    verdict.require(waitUntil([&] { return renderedFrames() >= occludedFrames + 3; }, 4000),
                    QStringLiteral("new video frames resume after removing the opaque sibling"));
    const QImage afterOcclusion = surface->grabFramebuffer();
    const PictureStats occlusionRecovery =
        statsOf(afterOcclusion, afterHide, surface->videoWidth(), surface->videoHeight(), dpr);
    verdict.require(occlusionRecovery.litFraction() >= kMinPictureLitFraction &&
                        occlusionRecovery.changedFraction() >= kMinMotionChangedFraction,
                    QStringLiteral("removing the opaque sibling reveals fresh lit video"));
    verdict.require(saveArtifact(options.outPrefix + QStringLiteral("_occlusion_restore.png"),
                                 afterOcclusion),
                    QStringLiteral("opaque-sibling recovery evidence was saved"));

    // ── minimized, then restored ──────────────────────────────────────────
    const quint64 framesBeforeMinimize = renderedFrames();
    const QImage beforeMinimize = surface->grabFramebuffer();
    const double minimizedPositionAt = surface->position();
    host.showMinimized();
    pumpEvents(200);
    const quint64 minimizedPaints = surface->paints;
    const quint64 minimizedFrames = renderedFrames();
    pumpEvents(700);
    const double minimizedAdvance = surface->position() - minimizedPositionAt;
    verdict.require(host.isMinimized(),
                    QStringLiteral("the platform placed the host in the minimized state"));
    verdict.require(surface->paints == minimizedPaints && renderedFrames() == minimizedFrames,
                    QStringLiteral("the settled minimized surface performs no video paints"));
    verdict.metric(QStringLiteral("while minimized: clock +%1 s, paints +%2")
                       .arg(minimizedAdvance, 0, 'f', 3)
                       .arg(surface->paints - minimizedPaints));
    host.showNormal();
    const bool minimizedRestored =
        waitUntil([&] { return renderedFrames() >= framesBeforeMinimize + 3; }, 6000);
    verdict.require(minimizedRestored,
                    QStringLiteral("the minimized surface rendered new output frames after "
                                   "restore"));
    verdict.require(surface->position() - minimizedPositionAt >= 0.3,
                    QStringLiteral("the film advanced across the minimized window (+%1 s)")
                        .arg(surface->position() - minimizedPositionAt, 0, 'f', 3));
    if (minimizedRestored) {
        const QImage afterMinimize = surface->grabFramebuffer();
        const PictureStats stats =
            statsOf(afterMinimize, beforeMinimize, surface->videoWidth(), surface->videoHeight(),
                    dpr);
        verdict.require(stats.litFraction() >= kMinPictureLitFraction,
                        QStringLiteral("the picture after minimize/restore is lit (%1%)")
                            .arg(100.0 * stats.litFraction(), 0, 'f', 1));
        verdict.require(stats.changedFraction() >= kMinMotionChangedFraction,
                        QStringLiteral("the picture after minimize/restore moved (%1%)")
                            .arg(100.0 * stats.changedFraction(), 0, 'f', 2));
        verdict.require(saveArtifact(options.outPrefix + QStringLiteral("_minimized_restore.png"),
                                     afterMinimize),
                        QStringLiteral("artifact %1_minimized_restore.png written")
                            .arg(options.outPrefix));
    }

    // ── a resize at a fractional device pixel ratio ───────────────────────
    const auto resizeViewport = [&](const QSize& target, const QString& stage) {
        const quint64 framesBeforeResize = renderedFrames();
        surface->resize(target);
        verdict.require(waitUntil([&] { return surface->lastResizeLogical == target; }, 2000),
                        QStringLiteral("%1: resizeGL saw %2x%3 (got %4x%5)")
                            .arg(stage)
                            .arg(target.width())
                            .arg(target.height())
                            .arg(surface->lastResizeLogical.width())
                            .arg(surface->lastResizeLogical.height()));
        QImage frame;
        const bool sized = waitUntil(
            [&] {
                frame = surface->grabFramebuffer();
                return framebufferMatches(frame.size(), target, dpr);
            },
            3000);
        verdict.require(sized, QStringLiteral("%1: the framebuffer is the device-pixel size of "
                                              "%2x%3 at ratio %4, expected %5x%6 (got %7x%8)")
                                   .arg(stage)
                                   .arg(target.width())
                                   .arg(target.height())
                                   .arg(dpr, 0, 'f', 2)
                                   .arg(qRound(target.width() * dpr))
                                   .arg(qRound(target.height() * dpr))
                                   .arg(frame.width())
                                   .arg(frame.height()));
        // The empty framebuffer from a resize must come back centred, not a
        // logical-sized viewport in one corner.
        const PictureStats stats =
            statsOf(frame, QImage(), surface->videoWidth(), surface->videoHeight(), dpr);
        verdict.require(stats.litFraction() >= kMinPictureLitFraction,
                        QStringLiteral("%1: the resized framebuffer carries a lit, centred picture "
                                       "(%2%)")
                            .arg(stage)
                            .arg(100.0 * stats.litFraction(), 0, 'f', 1));
        std::fprintf(stderr, "%s: video band %dx%d+%d,%d lit=%ld/%ld\n", qPrintable(stage),
                     stats.region.width(), stats.region.height(), stats.region.x(),
                     stats.region.y(), stats.lit, stats.sampled);
        waitUntil([&] { return renderedFrames() >= framesBeforeResize + 2; }, 2000);
        return frame;
    };

    const QImage smallPlaying =
        resizeViewport(QSize(501, 283), QStringLiteral("resize to 501x283 while playing"));
    verdict.require(saveArtifact(options.outPrefix + QStringLiteral("_fbo_playing.png"),
                                 smallPlaying),
                    QStringLiteral("artifact %1_fbo_playing.png written").arg(options.outPrefix));

    surface->setPaused(true);
    // Nothing below is a paused-frame claim until the core itself confirms the
    // pause: the flag is observation-driven, never assumed.
    verdict.require(waitUntil([&] { return surface->pauseObserved(); }, 3000),
                    QStringLiteral("the core observed the pause before the paused resize"));
    pumpEvents(300);
    const quint64 paintsBeforePausedResize = surface->paints;
    surface->resize(QSize(444, 250));
    // No update() here: paused, with no new frame, resizeGL's own request is the
    // only thing that can repaint the empty framebuffer.
    verdict.require(
        waitUntil([&] { return surface->paints >= paintsBeforePausedResize + 1; }, 2000),
        QStringLiteral("a paused resize repainted without a new frame (paints %1 -> %2)")
            .arg(paintsBeforePausedResize)
            .arg(surface->paints));
    const QImage pausedSmall = surface->grabFramebuffer();
    verdict.require(framebufferMatches(pausedSmall.size(), QSize(444, 250), dpr),
                    QStringLiteral("paused framebuffer is 444x250 at ratio %1, expected %2x%3 "
                                   "(got %4x%5)")
                        .arg(dpr, 0, 'f', 2)
                        .arg(qRound(444 * dpr))
                        .arg(qRound(250 * dpr))
                        .arg(pausedSmall.width())
                        .arg(pausedSmall.height()));
    const OverlayBand pausedBand = measureOverlayBand(pausedSmall, dpr);
    verdict.require(pausedBand.fillFraction() >= 0.80 && pausedBand.glyph >= 60,
                    QStringLiteral("the HUD band survived the paused resize (fill %1%, glyph %2 px)")
                        .arg(100.0 * pausedBand.fillFraction(), 0, 'f', 1)
                        .arg(pausedBand.glyph));
    const PictureStats pausedSmallStats =
        statsOf(pausedSmall, QImage(), surface->videoWidth(), surface->videoHeight(), dpr);
    verdict.require(pausedSmallStats.litFraction() >= kMinPictureLitFraction,
                    QStringLiteral("the paused resize left a lit, centred picture (%1%)")
                        .arg(100.0 * pausedSmallStats.litFraction(), 0, 'f', 1));
    verdict.require(saveArtifact(options.outPrefix + QStringLiteral("_fbo_paused.png"), pausedSmall),
                    QStringLiteral("artifact %1_fbo_paused.png written").arg(options.outPrefix));

    const quint64 framesAtResume = renderedFrames();
    surface->setPaused(false);
    verdict.require(waitUntil([&] { return renderedFrames() >= framesAtResume + 3; }, 4000),
                    QStringLiteral("playback resumed after the paused resize"));
    resizeViewport(QSize(640, 360), QStringLiteral("restore 640x360"));

    // ── a second surface destroyed mid-playback ───────────────────────────
    {
        LifecycleSurface* doomed = new LifecycleSurface;
        doomed->setWindowFlag(Qt::WindowStaysOnTopHint, true);
        doomed->resize(320, 180);
        QObject::connect(doomed, &mplayer::VideoSurface::errorOccurred, doomed,
                         [](const QString& message) {
                             std::fprintf(stderr, "second surface error: %s\n",
                                          qPrintable(message));
                         });
        doomed->show();
        const bool doomedLive =
            doomed->open(source) &&
            waitUntil(
                [&] {
                    return doomed->isContextReady() && doomed->hasVideo() &&
                           doomed->playbackDiagnostics().renderedOutputFrames >= 3;
                },
                6000);
        verdict.require(doomedLive,
                        QStringLiteral("a second surface rendered three output frames before it "
                                       "was destroyed"));
        verdict.metric(QStringLiteral("second surface painted %1 times before destruction")
                           .arg(doomed->paints));
        delete doomed;  // mid-playback teardown
        pumpEvents(400);
    }
    // The next frames are what prove the process survived intact.
    const quint64 framesAfterDoomed = renderedFrames();
    verdict.require(waitUntil([&] { return renderedFrames() >= framesAfterDoomed + 3; }, 4000),
                    QStringLiteral("the main surface still rendered output frames after the "
                                   "second one was destroyed"));
    const QImage afterDoomed = surface->grabFramebuffer();
    const PictureStats afterDoomedStats =
        statsOf(afterDoomed, QImage(), surface->videoWidth(), surface->videoHeight(), dpr);
    verdict.require(afterDoomedStats.litFraction() >= kMinPictureLitFraction,
                    QStringLiteral("the surviving surface still shows a lit picture (%1%)")
                        .arg(100.0 * afterDoomedStats.litFraction(), 0, 'f', 1));

    // ── reparent while paused: a real context recreation restores intent ──
    surface->setPaused(true);
    const bool pauseBeforeReparent =
        waitUntil([&] { return surface->pauseObserved() && !surface->isPlaying(); }, 3000);
    verdict.require(pauseBeforeReparent,
                    QStringLiteral("the core observed pause before GL recreation"));
    const double pausePositionBeforeReparent = surface->position();
    const int initsBeforeReparent = surface->glInits;
    QWidget host2;
    host2.setWindowFlag(Qt::WindowStaysOnTopHint, true);
    host2.setWindowTitle(QStringLiteral("mpv_surface_smoke render lifecycle (reparented)"));
    host2.resize(700, 500);
    surface->setParent(&host2);
    surface->setGeometry(10, 10, 600, 340);
    host2.show();
    host2.raise();
    host2.activateWindow();
    surface->show();
    // No counter is carried across the reparent: a recreated context may reset
    // the render count and the source epoch, so the proof is fresh motion in the
    // new context, not a number that grew.
    const bool reparented =
        waitUntil([&] { return surface->isContextReady() && surface->hasVideo(); }, 8000);
    verdict.require(reparented,
                    QStringLiteral("the reparented surface reports a ready context and video"));
            verdict.instrumentation(surface->glInits > initsBeforeReparent,
                                QStringLiteral("the reparent recreated the GL context (initializeGL "
                                           "%1 -> %2)")
                                .arg(initsBeforeReparent)
                                .arg(surface->glInits));
        const bool pauseRestored = waitUntil(
            [&] { return surface->pauseObserved() && !surface->isPlaying(); }, 3000);
        verdict.require(pauseRestored,
                        QStringLiteral("GL recreation restores the observed paused state"));
        pumpEvents(300);
        verdict.require(std::abs(surface->position() - pausePositionBeforeReparent) <= 0.12,
                        QStringLiteral("the paused source remains parked across GL recreation"));
    if (reparented && pauseBeforeReparent) {
        surface->setPaused(false);
        const bool resumed = waitUntil([&] { return !surface->pauseObserved(); }, 3000);
        verdict.require(resumed, QStringLiteral("playback resumes after paused GL recreation"));
    }
    if (reparented) {
        QImage reparentA = surface->grabFramebuffer();
        QImage reparentB = reparentA;
        PictureStats stats;
        PictureStats litBefore = statsOf(reparentA, QImage(), surface->videoWidth(),
                                        surface->videoHeight(), dpr);
        bool moved = false;
        for (int attempt = 0; attempt < 16 && !moved; ++attempt) {
            pumpEvents(300);
            reparentB = surface->grabFramebuffer();
            stats = statsOf(reparentB, reparentA, surface->videoWidth(), surface->videoHeight(),
                            dpr);
            const PictureStats litAfter = statsOf(reparentB, QImage(), surface->videoWidth(),
                                                 surface->videoHeight(), dpr);
            // Both frames have to carry the picture: a blank frame turning
            // coloured is a repaint, not movement.
            moved = litBefore.litFraction() >= kMinPictureLitFraction &&
                    litAfter.litFraction() >= kMinPictureLitFraction &&
                    stats.changedFraction() >= kMinMotionChangedFraction;
            reparentA = reparentB;
            litBefore = litAfter;
        }
        verdict.require(moved,
                        QStringLiteral("the reparented surface paints two lit, differing frames "
                                       "(%1% changed, both frames at least %2% lit)")
                            .arg(100.0 * stats.changedFraction(), 0, 'f', 2)
                            .arg(100.0 * kMinPictureLitFraction, 0, 'f', 0));
        verdict.require(stats.litFraction() >= kMinPictureLitFraction,
                        QStringLiteral("the reparented picture is lit (%1%)")
                            .arg(100.0 * stats.litFraction(), 0, 'f', 1));
        verdict.require(framebufferMatches(reparentB.size(), QSize(600, 340), dpr),
                        QStringLiteral("the reparented framebuffer is 600x340 at ratio %1, "
                                       "expected %2x%3 (got %4x%5)")
                            .arg(dpr, 0, 'f', 2)
                            .arg(qRound(600 * dpr))
                            .arg(qRound(340 * dpr))
                            .arg(reparentB.width())
                            .arg(reparentB.height()));
        verdict.require(saveArtifact(options.outPrefix + QStringLiteral("_reparent.png"),
                                     reparentB),
                        QStringLiteral("artifact %1_reparent.png written").arg(options.outPrefix));
    }

    // ── close, then reopen a different source ─────────────────────────────
    surface->close();
    pumpEvents(300);
    const double stoppedAt = surface->position();
    const int initsBeforeStoppedReparent = surface->glInits;
    QWidget host3;
    host3.setWindowFlag(Qt::WindowStaysOnTopHint, true);
    host3.resize(640, 420);
    surface->setParent(&host3);
    surface->setGeometry(10, 10, 600, 340);
    host3.show();
    host3.raise();
    host3.activateWindow();
    surface->show();
    const bool stoppedContextRecreated = waitUntil(
        [&] {
            return surface->isContextReady() && surface->glInits > initsBeforeStoppedReparent;
        },
        8000);
    verdict.require(stoppedContextRecreated,
                    QStringLiteral("a closed surface recreates its GL context"));
    pumpEvents(500);
    verdict.require(!surface->hasVideo() && !surface->isPlaying() &&
                        std::abs(surface->position() - stoppedAt) <= 0.12,
                    QStringLiteral("GL recreation after close stays stopped instead of reopening "
                                   "the former source"));
    const QString reopened = QStringLiteral("av://lavfi:testsrc=size=320x180:rate=30");
    const bool reopenedAccepted = surface->open(reopened);
    verdict.require(reopenedAccepted, QStringLiteral("reopening after close was accepted"));
    if (reopenedAccepted) {
        const quint64 framesAtReopen = renderedFrames();
        const bool reopenedLive =
            waitUntil(
                [&] {
                    return surface->isContextReady() && surface->hasVideo() &&
                           surface->videoWidth() == 320 && surface->videoHeight() == 180 &&
                           renderedFrames() >= framesAtReopen + 3;
                },
                8000);
        verdict.require(reopenedLive,
                        QStringLiteral("the reopened source rendered three output frames at "
                                       "320x180 within 8 s"));
        if (reopenedLive) {
            const quint64 framesNow = renderedFrames();
            const QImage reopenA = surface->grabFramebuffer();
            waitUntil([&] { return renderedFrames() >= framesNow + 4; }, 3000);
            const QImage reopenB = surface->grabFramebuffer();
            const PictureStats stats =
                statsOf(reopenB, reopenA, surface->videoWidth(), surface->videoHeight(), dpr);
            verdict.require(stats.litFraction() >= kMinPictureLitFraction,
                            QStringLiteral("the reopened source shows a lit picture (%1%)")
                                .arg(100.0 * stats.litFraction(), 0, 'f', 1));
            verdict.require(stats.changedFraction() >= kMinMotionChangedFraction,
                            QStringLiteral("the reopened source moves (%1%)")
                                .arg(100.0 * stats.changedFraction(), 0, 'f', 2));
            verdict.require(saveArtifact(options.outPrefix + QStringLiteral("_reopen_320x180.png"),
                                         reopenB),
                            QStringLiteral("artifact %1_reopen_320x180.png written")
                                .arg(options.outPrefix));
        }
    }

    QTemporaryDir missingSourceDirectory;
    verdict.require(missingSourceDirectory.isValid(),
                    QStringLiteral("the missing-source fixture has an isolated directory"));
    if (missingSourceDirectory.isValid()) {
        expectingSourceFailure = true;
        const bool accepted =
            surface->open(missingSourceDirectory.filePath(QStringLiteral("missing-video.mp4")));
        verdict.require(accepted, QStringLiteral("the asynchronous missing-source request was accepted"));
        const bool reported = accepted && waitUntil(
            [&] { return sourceFailureSeen && !surface->hasVideo(); }, 5000);
        expectingSourceFailure = false;
        verdict.require(reported,
                        QStringLiteral("a current-source failure notifies the viewer and clears video"));
        verdict.require(surface->isContextReady(),
                        QStringLiteral("a media load failure preserves the rendering context"));

        const bool recovering = surface->open(reopened);
        const bool recovered = recovering && waitUntil(
            [&] { return surface->hasVideo() && renderedFrames() >= 3; }, 5000);
        verdict.require(recovered, QStringLiteral("playback recovers after a current-source failure"));
        if (recovered) {
            const quint64 before = renderedFrames();
            const QImage first = surface->grabFramebuffer();
            waitUntil([&] { return renderedFrames() >= before + 4; }, 3000);
            const QImage second = surface->grabFramebuffer();
            const PictureStats firstStats =
                statsOf(first, QImage(), surface->videoWidth(), surface->videoHeight(), dpr);
            const PictureStats recoveredStats =
                statsOf(second, first, surface->videoWidth(), surface->videoHeight(), dpr);
            verdict.require(firstStats.litFraction() >= kMinPictureLitFraction &&
                                recoveredStats.litFraction() >= kMinPictureLitFraction &&
                                recoveredStats.changedFraction() >= kMinMotionChangedFraction,
                            QStringLiteral("the recovered picture has two lit, differing frames"));
            verdict.require(saveArtifact(options.outPrefix + QStringLiteral("_error_recovery.png"),
                                         second),
                            QStringLiteral("current-source recovery evidence was saved"));
        }
    }

    verdict.metric(QStringLiteral("media errors reported during the run: %1%2")
                       .arg(mediaErrorCount)
                       .arg(mediaError.isEmpty()
                                ? QString()
                                : QStringLiteral(" (last: %1)").arg(mediaError)));
    verdict.require(mediaErrorCount == 0,
                    QStringLiteral("the run reported no media error (last: %1)")
                        .arg(mediaError.isEmpty() ? QStringLiteral("none") : mediaError));
    std::fprintf(stderr, "render lifecycle complete; surface teardown follows\n");
    std::fflush(stdout);
    // host2 is declared after host, so it is destroyed first and takes the
    // surface with it: the app's own final teardown, after every stage.
    return verdict.report("mpv_surface_smoke:render-lifecycle");
}

// ── the historical single-clip proof ──────────────────────────────────────

// The media clock, source geometry and health are retained with the picture:
// after EOF the surface resets its own fields, and a sample without them cannot
// be banded, paired or trusted.
struct FrameSample {
    QImage image;
    double position = -1.0;
    int videoWidth = 0;
    int videoHeight = 0;
    bool contextReady = false;
    bool clean = false;
    bool valid = false;
};

int runSingleClip(QApplication& app, const QString& requested) {
    mplayer::VideoSurface surface;
    surface.resize(640, 360);
    std::optional<double> streamFps;
    auto streamFpsSource = mplayer::StreamFpsSource::Unknown;
    QString mediaError;
    QObject::connect(&surface, &mplayer::VideoSurface::errorOccurred,
                     [&](const QString& message) {
                         mediaError = message;
                         std::fprintf(stderr, "media error: %s\n", qPrintable(message));
                     });
    QObject::connect(&surface, &mplayer::VideoSurface::streamStatsChanged,
                     [&](double, double fps, mplayer::StreamFpsSource source) {
                         streamFps = fps;
                         streamFpsSource = source;
                     });
    surface.show();
    const QString target = requested.isEmpty()
                               ? QStringLiteral("av://lavfi:testsrc=size=640x360:rate=25")
                               : requested;
    std::fprintf(stderr, "target=%s\n", qPrintable(target));
    if (!surface.open(target)) {
        std::fprintf(stdout, "RESULT: FAIL (open refused)\n");
        return 1;
    }

    const auto grabSample = [&](double position) {
        FrameSample sample;
        sample.image = surface.grabFramebuffer();
        sample.position = position;
        sample.videoWidth = surface.videoWidth();
        sample.videoHeight = surface.videoHeight();
        sample.contextReady = surface.isContextReady();
        sample.clean = mediaError.isEmpty();
        sample.valid = !sample.image.isNull();
        return sample;
    };

    FrameSample early;
    FrameSample lastLive;
    bool done = false;
    QTimer timer;
    timer.setInterval(100);

    const auto finish = [&]() {
        if (done) return;
        done = true;
        timer.stop();

        const bool havePair = early.valid && lastLive.valid && lastLive.position > early.position;
        const FrameSample& shown = havePair ? lastLive : early;
        const QSize pictureSize =
            shown.videoWidth > 0 && shown.videoHeight > 0
                ? QSize(shown.videoWidth, shown.videoHeight)
                      .scaled(shown.image.size(), Qt::KeepAspectRatio)
                : QSize();
        const QRect picture(QPoint((shown.image.width() - pictureSize.width()) / 2,
                                   (shown.image.height() - pictureSize.height()) / 2),
                            pictureSize);

        int nonblack = 0;
        int total = 0;
        int changed = 0;
        int compared = 0;
        if (!shown.image.isNull() && picture.isValid()) {
            for (int y = picture.top(); y <= picture.bottom(); ++y) {
                for (int x = picture.left(); x <= picture.right(); ++x) {
                    const QColor pixel = shown.image.pixelColor(x, y);
                    ++total;
                    // 0x080808 sums to 24: legitimate near-black content stays lit.
                    if (pixel.red() + pixel.green() + pixel.blue() > 12) ++nonblack;
                    if (havePair) {
                        const QColor before = early.image.pixelColor(x, y);
                        ++compared;
                        if (std::abs(pixel.red() - before.red()) +
                                std::abs(pixel.green() - before.green()) +
                                std::abs(pixel.blue() - before.blue()) >
                            8) {
                            ++changed;
                        }
                    }
                }
            }
        }

        const bool captured =
            !shown.image.isNull() && shown.videoWidth > 0 && shown.videoHeight > 0;
        const bool clean = mediaError.isEmpty() && early.clean && lastLive.clean &&
                           early.contextReady && lastLive.contextReady;
        const bool saved =
            !shown.image.isNull() && shown.image.save(QStringLiteral("mpv_surface_smoke.png"));

        std::fprintf(stderr,
                     "hasVideo=%d playing=%d contextReady=%d video=%dx%d "
                     "position=%.3f earlierPosition=%.3f duration=%.3f\n",
                     int(surface.hasVideo()), int(surface.isPlaying()),
                     int(surface.isContextReady()), shown.videoWidth, shown.videoHeight,
                     surface.position(), early.valid ? early.position : -1.0,
                     surface.duration());
        if (streamFps.has_value()) {
            const char* provenance =
                streamFpsSource == mplayer::StreamFpsSource::MpvOutput
                    ? "mpv output"
                    : streamFpsSource == mplayer::StreamFpsSource::Container
                          ? "container source"
                          : "unknown";
            std::fprintf(stderr, "reported stream FPS=%.3f (%s)\n", *streamFps, provenance);
        } else {
            std::fprintf(stderr, "reported stream FPS=unavailable\n");
        }
        const mplayer::PlaybackDiagnostics diagnostics = surface.playbackDiagnostics();
        if (diagnostics.estimatedVfFps.has_value()) {
            std::fprintf(stderr, "raw estimated-vf-fps=%.3f\n", *diagnostics.estimatedVfFps);
        } else {
            std::fprintf(stderr, "raw estimated-vf-fps=unavailable\n");
        }
        if (diagnostics.decoderDropCount.has_value()) {
            std::fprintf(stderr, "decoder-frame-drop-count=%lld\n",
                         static_cast<long long>(*diagnostics.decoderDropCount));
        } else {
            std::fprintf(stderr, "decoder-frame-drop-count=unavailable (named core limitation, "
                                 "not a measured zero)\n");
        }
        if (diagnostics.voDropCount.has_value()) {
            std::fprintf(stderr, "frame-drop-count (VO drops)=%lld\n",
                         static_cast<long long>(*diagnostics.voDropCount));
        } else {
            std::fprintf(stderr, "frame-drop-count (VO drops)=unavailable (named core "
                                 "limitation, not a measured zero)\n");
        }
        const auto hardwareDecoder =
            surface.probeMpvProperty(QStringLiteral("hwdec-current"),
                                     mplayer::ProbePropertyKind::Text);
        const QString hardwareDecoderName =
            hardwareDecoder && hardwareDecoder->kind == mplayer::ProbePropertyValue::Kind::Text
                ? hardwareDecoder->text
                : QStringLiteral("unavailable");
        std::fprintf(stderr, "hwdec-current=%s\n", qPrintable(hardwareDecoderName));
        if (diagnostics.nativeFilterPresent.has_value()) {
            std::fprintf(stderr, "native filter in the live vf chain=%s (%d entries)\n",
                         *diagnostics.nativeFilterPresent ? "present" : "absent",
                         diagnostics.vfCount.value_or(-1));
        } else {
            std::fprintf(stderr, "native filter in the live vf chain=unknown (not read)\n");
        }
        std::fprintf(stderr,
                     "framebuffer=%dx%d picture band=%dx%d at %d,%d nonblack=%d/%d "
                     "motion=%d/%d PNG=%s\n",
                     shown.image.width(), shown.image.height(), pictureSize.width(),
                     pictureSize.height(), picture.x(), picture.y(), nonblack, total, changed,
                     compared, saved ? "written" : "FAILED");
        if (!mediaError.isEmpty()) {
            std::fprintf(stderr, "media error: %s\n", qPrintable(mediaError));
        }

        const bool motion = compared > 0 && changed * 100 >= compared;
        int exitCode = 1;
        if (!havePair) {
            std::fprintf(stdout, "RESULT: FAIL (no live picture pair)\n");
        } else if (!clean) {
            std::fprintf(stdout, "RESULT: FAIL (media or context failed while sampling)\n");
        } else if (!captured || total == 0) {
            std::fprintf(stdout, "RESULT: FAIL (no picture geometry)\n");
        } else if (!saved) {
            std::fprintf(stdout, "RESULT: FAIL (PNG not written)\n");
        } else if (nonblack * 2 <= total) {
            // Frames did arrive, so a black band is either valid black content
            // or a renderer that painted nothing; this probe cannot tell them apart.
            std::fprintf(stdout, "RESULT: SKIP (black picture: valid black content and a "
                                 "failed render are indistinguishable)\n");
            exitCode = 77;
        } else if (!motion) {
            std::fprintf(stdout, "RESULT: SKIP (no inter-frame motion: static content)\n");
            exitCode = 77;
        } else if (!diagnostics.nativeFilterPresent.has_value()) {
            std::fprintf(stdout, "RESULT: SKIP (vf chain not read: native absence not provable)\n");
            exitCode = 77;
        } else if (*diagnostics.nativeFilterPresent) {
            std::fprintf(stdout, "RESULT: FAIL (native filter present on a standard core)\n");
            exitCode = 1;
        } else {
            std::fprintf(stdout, "RESULT: PASS\n");
            exitCode = 0;
        }
        std::fflush(stdout);

        surface.hide();
        surface.close();
        app.exit(exitCode);
    };

    QObject::connect(&timer, &QTimer::timeout, &surface, [&]() {
        if (done || !surface.isContextReady()) return;
        if (!surface.hasVideo()) {
            // End of file after two live samples is not a failure; a short clip
            // arrives here instead of being read past its end on a fixed clock.
            if (early.valid && lastLive.valid && lastLive.position > early.position) finish();
            return;
        }
        const double position = surface.position();
        const double duration = surface.duration();
        if (!early.valid) {
            const double earlyAt =
                duration > 0.0 ? std::min(0.5, std::max(0.1, duration * 0.15)) : 0.5;
            if (position >= earlyAt) early = grabSample(position);
            return;
        }
        const double advance =
            duration > 0.0 ? std::min(2.0, std::max(0.3, duration * 0.5)) : 2.0;
        if (position >= early.position + advance) {
            lastLive = grabSample(position);
            finish();
            return;
        }
        // A recent live frame keeps a short clip's pre-EOF sample available.
        if (!lastLive.valid || position - lastLive.position >= 0.2) {
            lastLive = grabSample(position);
        }
    });

    QTimer::singleShot(9000, &surface, [&] { finish(); });
    timer.start();
    return app.exec();
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<char*> passthrough;
    const Options options = parseOptions(argc, argv, &passthrough);
    if (options.dpr > 0.0) {
        // Must be set before QApplication: QOpenGLWidget has no per-widget ratio.
        qputenv("QT_SCALE_FACTOR", QByteArray::number(options.dpr));
        qputenv("QT_AUTO_SCREENSCALE_FACTOR", QByteArray("0"));
    }
    int filteredArgc = static_cast<int>(passthrough.size());
    // QApplication requires argv[argc] == nullptr, which a vector of exactly
    // argc char* pointers does not provide.
    passthrough.push_back(nullptr);
    QApplication app(filteredArgc, passthrough.data());
    app.setQuitOnLastWindowClosed(false);
    if (!mplayer::probes::selectRuntime()) {
        return 2;
    }
    if (options.renderLifecycle) return runRenderLifecycle(options);
    return runSingleClip(app, options.source);
}
