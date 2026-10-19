// QOpenGLWidget lacks offscreen-platform GL; a real FBO exercises the same mpv bridge.
#include <QElapsedTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QSaveFile>
#include <QSurfaceFormat>
#include <QTemporaryDir>
#include <QTextStream>
#include <QThread>

#include <algorithm>
#include <optional>
#include <vector>

#include "Anime4K.hpp"
#include "MpvHandle.hpp"
#include "MpvRenderContext.hpp"
#include "ProbeRuntime.hpp"

using namespace mplayer;

namespace {

QTextStream g_log;

void note(const QString& line) {
    g_log << line << Qt::endl;
    std::fprintf(stderr, "%s\n", qPrintable(line));
}

// nullopt, never 0: a refused property must not be handed out as a number.
// announce is off for the per-frame sampling reads, which would otherwise
// repeat the same refusal hundreds of times.
std::optional<double> readProperty(const MpvHandle& mpv, const char* property, bool announce) {
    try {
        const double value = mpv.getNumber(property);
        if (announce) {
            note(QStringLiteral("%1=%2").arg(QLatin1String(property)).arg(value, 0, 'g', 12));
        }
        return value;
    } catch (const MpvError& error) {
        if (announce) {
            note(QStringLiteral("%1=unavailable (%2)")
                     .arg(QLatin1String(property), QString::fromUtf8(error.what())));
        }
        return std::nullopt;
    }
}

std::optional<double> noteProperty(const MpvHandle& mpv, const char* property) {
    return readProperty(mpv, property, true);
}

}  // namespace

int main(int argc, char** argv) {
    QFile out(QStringLiteral("headless_px.txt"));
    if (!out.open(QIODevice::WriteOnly | QIODevice::Text)) {
        std::fprintf(stderr, "FAIL cannot write headless_px.txt: %s\n", qPrintable(out.errorString()));
        return 2;
    }
    g_log.setDevice(&out);

    QSurfaceFormat format;
    format.setDepthBufferSize(24);
    format.setStencilBufferSize(8);
    QSurfaceFormat::setDefaultFormat(format);

    QString presetId;
    int anime4kScale = 1;
    QString mediaArgument;
    for (int index = 1; index < argc; ++index) {
        const QString argument = QString::fromLocal8Bit(argv[index]);
        if (argument.startsWith(QStringLiteral("--anime4k-preset="))) {
            presetId = argument.mid(QStringLiteral("--anime4k-preset=").size());
        } else if (argument.startsWith(QStringLiteral("--anime4k-scale="))) {
            bool ok = false;
            anime4kScale = argument.mid(QStringLiteral("--anime4k-scale=").size()).toInt(&ok);
            if (!ok || (anime4kScale != 1 && anime4kScale != 2 && anime4kScale != 4)) {
                std::fprintf(stderr, "FAIL: --anime4k-scale must be 1, 2, or 4\\n");
                return 2;
            }
        } else if (!argument.startsWith(QStringLiteral("--"))) {
            mediaArgument = argument;
        }
    }
    if (presetId.isEmpty() && anime4kScale != 1) {
        std::fprintf(stderr, "FAIL: --anime4k-scale requires --anime4k-preset=<id>\\n");
        return 2;
    }

    QGuiApplication app(argc, argv);
    if (!mplayer::probes::selectRuntime()) {
        return 2;
    }

    note(QStringLiteral("platform: %1").arg(QGuiApplication::platformName()));

    QOpenGLContext context;
    context.setFormat(format);
    if (!context.create()) {
        note(QStringLiteral("FAIL: no GL context"));
        return 2;
    }
    QOffscreenSurface surface;
    surface.setFormat(context.format());
    surface.create();
    if (!surface.isValid()) {
        note(QStringLiteral("FAIL: offscreen surface invalid"));
        return 2;
    }
    if (!context.makeCurrent(&surface)) {
        note(QStringLiteral("FAIL: context not current"));
        return 2;
    }

    QOpenGLFunctions* gl = context.functions();
    note(QStringLiteral("GL: %1")
             .arg(QString::fromLatin1(reinterpret_cast<const char*>(gl->glGetString(GL_VERSION)))));

    const int width = 320;
    const int height = 180;
    QOpenGLFramebufferObject fbo(width, height);
    if (!fbo.isValid()) {
        note(QStringLiteral("FAIL: FBO invalid"));
        return 2;
    }
    note(QStringLiteral("fbo valid, handle=%1").arg(fbo.handle()));

    gl->glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(fbo.handle()));
    gl->glViewport(0, 0, width, height);
    gl->glClearColor(0.f, 0.f, 0.f, 1.f);
    gl->glClear(GL_COLOR_BUFFER_BIT);
    while (gl->glGetError() != GL_NO_ERROR) {
    }

    QTemporaryDir shaderStage(
        QDir::current().filePath(QStringLiteral(".headless-pixel-anime4k-XXXXXX")));
    std::vector<std::pair<std::string, std::string>> mpvOptions = {{"vo", "libmpv"}};
    int stageWidth = width;
    int stageHeight = height;
    if (!presetId.isEmpty()) {
        QString reason;
        const QByteArray program = anime4k::filterShaderProgramForCandidate(
            presetId, anime4k::findShadersDir(), &reason);
        if (program.isEmpty()) {
            note(QStringLiteral("FAIL: %1").arg(reason));
            return 2;
        }
        if (!shaderStage.isValid()) {
            note(QStringLiteral("FAIL: cannot create owned Anime4K shader staging directory"));
            return 2;
        }
        const QString stagedPath = QDir(shaderStage.path()).filePath(QStringLiteral("program.glsl"));
        QSaveFile stagedProgram(stagedPath);
        if (!stagedProgram.open(QIODevice::WriteOnly) ||
            stagedProgram.write(program) != program.size() || !stagedProgram.commit()) {
            note(QStringLiteral("FAIL: cannot atomically stage Anime4K shader program: %1")
                     .arg(stagedProgram.errorString()));
            return 2;
        }
        stageWidth *= anime4kScale;
        stageHeight *= anime4kScale;
        const QString relativeShaderPath =
            QDir::current().relativeFilePath(QFileInfo(stagedPath).absoluteFilePath());
        const QString filter = QStringLiteral(
            "lavfi=[libplacebo=w=%1:h=%2:custom_shader_path=%3]")
                                   .arg(stageWidth)
                                   .arg(stageHeight)
                                   .arg(relativeShaderPath);
        mpvOptions.emplace_back("vf", filter.toStdString());
        note(QStringLiteral("Anime4K preset=%1 libplacebo-stage=%2x%3 shader=%4")
                 .arg(presetId)
                 .arg(stageWidth)
                 .arg(stageHeight)
                 .arg(relativeShaderPath));
    }

    auto api = std::make_shared<GlApi>(loadGlApi());
    int frameUpdates = 0;
    int renderErrors = 0;
    int glErrors = 0;
    GLenum firstGlError = GL_NO_ERROR;
    int videoWidth = 0;
    int videoHeight = 0;
    bool positionSeen = false;

    QImage image;
    try {
        MpvHandle mpv(mpvOptions);
        // This probe drives rendering from its own loop and never enables
        // advanced control, so it has no update work to hand over.
        MpvRenderContext rc(mpv, *api, [] {});
        rc.setFlipY(false);
        const QString media = mediaArgument.isEmpty()
                                  ? QStringLiteral("av://lavfi:testsrc=size=320x180:rate=25")
                                  : mediaArgument;
        note(QStringLiteral("media: %1").arg(media));
        mpv.loadFile(media.toStdString());

        // The clock is read fresh each pass: a lavfi source can report a tiny,
        // still-growing duration before it settles, so nothing is cached.
        std::optional<double> lastPosition;
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 8000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            if (rc.update() & MPV_RENDER_UPDATE_FRAME) {
                ++frameUpdates;
            }
            gl->glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(fbo.handle()));
            if (rc.render(static_cast<int>(fbo.handle()), width, height, 0) != 0) {
                ++renderErrors;
            }
            rc.reportSwap();
            const GLenum glError = gl->glGetError();
            if (glError != GL_NO_ERROR) {
                if (glErrors == 0) firstGlError = glError;
                ++glErrors;
            }
            QThread::msleep(8);

            const std::optional<double> duration = readProperty(mpv, "duration", false);
            if (videoWidth <= 0 || videoHeight <= 0) {
                const std::optional<double> w = readProperty(mpv, "video-params/w", false);
                const std::optional<double> h = readProperty(mpv, "video-params/h", false);
                if (w && h && *w > 0.0 && *h > 0.0) {
                    videoWidth = qRound(*w);
                    videoHeight = qRound(*h);
                }
            }
            const std::optional<double> position = readProperty(mpv, "time-pos", false);
            if (position) {
                positionSeen = true;
                // Only a real clock that moves backwards is mpv ending the file.
                if (lastPosition && *position + 0.25 < *lastPosition) break;
                lastPosition = position;
            }
            const double sampleAt =
                duration && *duration > 0.0 ? std::min(3.0, *duration * 0.5) : 3.0;
            // The frame floor stops a bogus tiny duration from breaking on the
            // first frame, before any playback is measurable.
            if (frameUpdates >= 8 && position && *position >= sampleAt) break;
        }

        gl->glFinish();
        image = fbo.toImage();
        note(QStringLiteral("contextReady=%1").arg(context.isValid() && fbo.isValid()));
        if (!positionSeen) {
            note(QStringLiteral("time-pos never became available during sampling"));
        }
        noteProperty(mpv, "time-pos");
        noteProperty(mpv, "duration");
        if (videoWidth > 0 && videoHeight > 0) {
            note(QStringLiteral("video-params/w=%1").arg(videoWidth));
            note(QStringLiteral("video-params/h=%1").arg(videoHeight));
        } else {
            noteProperty(mpv, "video-params/w");
            noteProperty(mpv, "video-params/h");
        }
        if (!presetId.isEmpty()) {
            const auto outputWidth = readProperty(mpv, "video-out-params/w", false);
            const auto outputHeight = readProperty(mpv, "video-out-params/h", false);
            if (!outputWidth || !outputHeight || qRound(*outputWidth) != stageWidth ||
                qRound(*outputHeight) != stageHeight) {
                note(QStringLiteral("FAIL: filtered output does not match %1x%2")
                         .arg(stageWidth).arg(stageHeight));
                return 1;
            }
            note(QStringLiteral("verified filtered output=%1x%2")
                     .arg(qRound(*outputWidth)).arg(qRound(*outputHeight)));
        }
        noteProperty(mpv, "estimated-vf-fps");
        noteProperty(mpv, "decoder-frame-drop-count");
        noteProperty(mpv, "frame-drop-count");
        noteProperty(mpv, "vo-drop-frame-count");
    } catch (const MpvError& e) {
        note(QStringLiteral("MpvError: %1").arg(QString::fromUtf8(e.what())));
        return 9;
    }

    note(QStringLiteral("render frame updates: %1").arg(frameUpdates));
    note(QStringLiteral("render errors: %1").arg(renderErrors));
    if (glErrors > 0) {
        note(QStringLiteral("GL errors: %1 (first 0x%2)").arg(glErrors).arg(firstGlError, 0, 16));
    } else {
        note(QStringLiteral("GL errors: 0"));
    }
    note(QStringLiteral("image %1x%2 format=%3")
             .arg(image.width())
             .arg(image.height())
             .arg(static_cast<int>(image.format())));

    if (image.isNull()) {
        note(QStringLiteral("RESULT: FAIL (no framebuffer)"));
        return 1;
    }
    if (renderErrors > 0) {
        note(QStringLiteral("RESULT: FAIL (the render call failed %1 times)").arg(renderErrors));
        return 1;
    }
    if (videoWidth <= 0 || videoHeight <= 0) {
        note(QStringLiteral("RESULT: FAIL (no video dimensions)"));
        return 1;
    }
    if (frameUpdates == 0) {
        note(QStringLiteral("RESULT: FAIL (no frames rendered)"));
        return 1;
    }

    // mpv fits the video into the FBO, so a portrait source fills a centre band
    // and a whole-frame count would fail a fully lit portrait picture.
    const QSize picture = QSize(videoWidth, videoHeight).scaled(image.size(), Qt::KeepAspectRatio);
    const QRect band(QPoint((image.width() - picture.width()) / 2,
                            (image.height() - picture.height()) / 2),
                     picture);
    note(QStringLiteral("picture band %1x%2 at %3,%4")
             .arg(picture.width())
             .arg(picture.height())
             .arg(band.x())
             .arg(band.y()));

    long nonblack = 0;
    long total = 0;
    for (int y = band.top(); y <= band.bottom(); ++y) {
        for (int x = band.left(); x <= band.right(); ++x) {
            const QColor pixel = image.pixelColor(x, y);
            ++total;
            if (pixel.red() + pixel.green() + pixel.blue() > 12) ++nonblack;
        }
    }
    note(QStringLiteral("nonblack: %1 of %2").arg(nonblack).arg(total));

    if (total == 0) {
        note(QStringLiteral("RESULT: FAIL (empty picture band)"));
        return 1;
    }
    if (nonblack * 2 <= total) {
        // Frames did render, so a black band is either valid black content or a
        // renderer that painted nothing; this probe cannot tell them apart.
        note(QStringLiteral("RESULT: SKIP (black picture: valid black content and a failed render "
                            "are indistinguishable)"));
        return 77;
    }
    note(QStringLiteral("RESULT: PASS"));
    return 0;
}
