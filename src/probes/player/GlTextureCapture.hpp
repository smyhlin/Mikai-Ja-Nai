#pragma once
#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QOpenGLWidget>
#include <QSize>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

inline bool savePlayerGlTextures(QOpenGLWidget* surface, QSize outputSize, const QString& folder) {
    const auto fail = [](const QString& reason) {
        std::fprintf(stderr, "[gl-textures] FAIL %s\n", qPrintable(reason));
        return false;
    };
    if (!surface || outputSize.isEmpty()) return fail(QStringLiteral("invalid capture surface/size"));
    QOpenGLContext* context = surface->context();
    if (!context || context != QOpenGLContext::currentContext())
        return fail(QStringLiteral("surface context is not current"));
#ifdef Q_OS_WIN
    using GetLevel = void(__stdcall*)(GLuint, GLint, GLenum, GLint*);
    using GetImage = void(__stdcall*)(GLuint, GLint, GLenum, GLenum, GLsizei, void*);
#else
    using GetLevel = void(*)(GLuint, GLint, GLenum, GLint*);
    using GetImage = void(*)(GLuint, GLint, GLenum, GLenum, GLsizei, void*);
#endif
    const auto getLevel = reinterpret_cast<GetLevel>(context->getProcAddress("glGetTextureLevelParameteriv"));
    const auto getImage = reinterpret_cast<GetImage>(context->getProcAddress("glGetTextureImage"));
    if (!getLevel || !getImage) return fail(QStringLiteral("OpenGL 4.5 texture readback unavailable"));
    QOpenGLFunctions* gl = context->functions();
    const auto error = [&]() {
        const GLenum value = gl->glGetError();
        return value == GL_NO_ERROR ? QString() : QStringLiteral("GL error 0x%1").arg(value, 0, 16);
    };
    GLint buffer = -1, alignment = -1, rowLength = -1, skipRows = -1, skipPixels = -1;
    gl->glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &buffer);
    gl->glGetIntegerv(GL_PACK_ALIGNMENT, &alignment);
    gl->glGetIntegerv(GL_PACK_ROW_LENGTH, &rowLength);
    gl->glGetIntegerv(GL_PACK_SKIP_ROWS, &skipRows);
    gl->glGetIntegerv(GL_PACK_SKIP_PIXELS, &skipPixels);
    const QString priorError = error();
    if (!priorError.isEmpty()) return fail(priorError);
    if (buffer != 0) return fail(QStringLiteral("pixel-pack buffer %1 is bound").arg(buffer));
    const QDir directory(folder);
    if (!directory.exists() && !directory.mkpath(QStringLiteral(".")))
        return fail(QStringLiteral("cannot create capture directory"));
    const auto saveBytes = [&](const QString& name, const QByteArray& bytes) {
        QFile file(directory.filePath(name));
        return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() &&
               file.flush() && file.error() == QFile::NoError;
    };
    gl->glPixelStorei(GL_PACK_ALIGNMENT, 1);
    gl->glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    gl->glPixelStorei(GL_PACK_SKIP_ROWS, 0);
    gl->glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    const bool captured = [&]() {
        int live = 0, lumaCount = 0, chromaCount = 0;
        bool activePlanesFinite = true;
        QByteArray luma, chroma;
        for (GLuint id = 1; id <= 512; ++id) {
            if (!gl->glIsTexture(id)) continue;
            ++live;
            GLint width = 0, height = 0, depth = 0, format = 0;
            getLevel(id, 0, GL_TEXTURE_WIDTH, &width);
            getLevel(id, 0, GL_TEXTURE_HEIGHT, &height);
            getLevel(id, 0, GL_TEXTURE_DEPTH, &depth);
            getLevel(id, 0, GL_TEXTURE_INTERNAL_FORMAT, &format);
            const QString queryError = error();
            if (!queryError.isEmpty()) return fail(QStringLiteral("texture %1: %2").arg(id).arg(queryError));
            std::fprintf(stderr, "[gl-textures] id=%u format=0x%x size=%dx%dx%d\n", id, format, width, height, depth);
            const bool isLuma = format == GL_R8 && width == outputSize.width() && height == outputSize.height();
            const bool isChroma = format == GL_RG8 && width == (outputSize.width() + 1) / 2 &&
                                  height == (outputSize.height() + 1) / 2;
            const bool isColor = (format == GL_RGBA8 || format == GL_RGBA16F || format == GL_RGBA32F ||
                                  format == GL_RGB10_A2) && width > 0 && height > 0;
            if ((!isLuma && !isChroma && !isColor) || depth > 1) continue;
            const int bytesPerPixel = isColor ? 4 * sizeof(GLfloat) : isLuma ? 1 : 2;
            const qint64 byteCount = qint64(width) * height * bytesPerPixel;
            if (byteCount <= 0 || byteCount > std::numeric_limits<GLsizei>::max())
                return fail(QStringLiteral("texture %1 capture size out of range").arg(id));
            QByteArray bytes(byteCount, Qt::Uninitialized);
            getImage(id, 0, isColor ? GL_RGBA : isLuma ? GL_RED : GL_RG,
                     isColor ? GL_FLOAT : GL_UNSIGNED_BYTE, static_cast<GLsizei>(byteCount), bytes.data());
            const QString readError = error();
            if (!readError.isEmpty()) return fail(QStringLiteral("texture %1: %2").arg(id).arg(readError));
            const QString stem = QStringLiteral("%1-t%2-%3x%4")
                                     .arg(isColor ? QStringLiteral("rgba-f32") : isLuma ? QStringLiteral("nv12-luma") : QStringLiteral("nv12-chroma"))
                                     .arg(id).arg(width).arg(height);
            if (!saveBytes(stem + QStringLiteral(".bin"), bytes)) return fail(QStringLiteral("cannot save %1").arg(stem));
            if (isColor) {
                QImage preview(width, height, QImage::Format_RGB888);
                if (preview.isNull()) return fail(QStringLiteral("cannot allocate color texture preview"));
                const GLfloat* values = reinterpret_cast<const GLfloat*>(bytes.constData());
                qsizetype nonfinite[4]{};
                for (int y = 0; y < height; ++y) {
                    uchar* row = preview.scanLine(y);
                    for (int x = 0; x < width; ++x) {
                        const qsizetype base = (qsizetype(y) * width + x) * 4;
                        for (int channel = 0; channel < 4; ++channel) {
                            const float value = values[base + channel];
                            if (!std::isfinite(value)) ++nonfinite[channel];
                            if (channel < 3) row[x * 3 + channel] = std::isfinite(value)
                                ? static_cast<uchar>(std::clamp(value, 0.0f, 1.0f) * 255.0f) : 0;
                        }
                    }
                }
                if (!preview.save(directory.filePath(stem + QStringLiteral(".png")), "PNG"))
                    return fail(QStringLiteral("cannot save color texture preview"));
                std::fprintf(stderr, "[gl-textures] id=%u nonfinite=%lld/%lld/%lld/%lld file=%s\n", id,
                             static_cast<long long>(nonfinite[0]), static_cast<long long>(nonfinite[1]),
                             static_cast<long long>(nonfinite[2]), static_cast<long long>(nonfinite[3]), qPrintable(stem));
                if (height == outputSize.height() &&
                    (width == outputSize.width() || width == (outputSize.width() + 1) / 2)) {
                    activePlanesFinite = activePlanesFinite &&
                        std::all_of(nonfinite, nonfinite + 4,
                                    [](qsizetype count) { return count == 0; });
                }
            } else if (isLuma) {
                const QImage preview(reinterpret_cast<const uchar*>(bytes.constData()), width, height,
                                     width, QImage::Format_Grayscale8);
                if (!preview.save(directory.filePath(stem + QStringLiteral(".png")), "PNG"))
                    return fail(QStringLiteral("cannot save luma preview"));
                luma = std::move(bytes);
                ++lumaCount;
            } else {
                chroma = std::move(bytes);
                ++chromaCount;
            }
        }
        std::fprintf(stderr, "[gl-textures] ids=1..512 live=%d luma=%d chroma=%d; bounded scan only\n",
                     live, lumaCount, chromaCount);
        if (lumaCount != 1 || chromaCount != 1) return fail(QStringLiteral("no unambiguous NV12 texture pair"));
        const QString path = directory.filePath(QStringLiteral("nv12-combined-%1x%2.bin")
                                                    .arg(outputSize.width()).arg(outputSize.height()));
        QFile combined(path);
        if (!combined.open(QIODevice::WriteOnly) || combined.write(luma) != luma.size() ||
            combined.write(chroma) != chroma.size() || !combined.flush() || combined.error() != QFile::NoError)
            return fail(QStringLiteral("cannot save combined NV12 planes"));
        std::fprintf(stderr, "[gl-textures] combined=%s\n", qPrintable(path));
        if (!activePlanesFinite)
            return fail(QStringLiteral("non-finite values in active output-sized color/chroma planes"));
        return true;
    }();
    gl->glPixelStorei(GL_PACK_ALIGNMENT, alignment);
    gl->glPixelStorei(GL_PACK_ROW_LENGTH, rowLength);
    gl->glPixelStorei(GL_PACK_SKIP_ROWS, skipRows);
    gl->glPixelStorei(GL_PACK_SKIP_PIXELS, skipPixels);
    const QString restoreError = error();
    if (!restoreError.isEmpty()) return fail(restoreError);
    return captured;
}
