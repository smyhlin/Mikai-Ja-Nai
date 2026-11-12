#pragma once

#include <QByteArray>
#include <QImage>
#include <QSize>
#include <QString>
#include <QtGlobal>

#include <array>
#include <cstdint>
#include <vector>

namespace mplayer {

struct RenderDiagnosticsRequest {
    bool captureGlTextures = false;
    quint64 contextGeneration = 0;
    quintptr coreIdentity = 0;
    uint64_t sourceEpoch = 0;
    double position = 0.0;
};

struct RenderDiagnosticsGlState {
    bool stencilTest = false;
    bool depthTest = false;
    bool blend = false;
    bool scissorTest = false;
    bool dither = false;
    std::array<bool, 4> colorWriteMask{};
    int pixelUnpackBufferBinding = 0;
    int unpackAlignment = 0;
    int unpackRowLength = 0;
};

struct RenderDiagnosticsTexture {
    enum class Plane { Luma, Chroma, Color } plane = Plane::Color;
    unsigned int textureName = 0;
    int width = 0;
    int height = 0;
    int internalFormat = 0;
    int externalFormat = 0;
    int externalType = 0;
    QByteArray bytes;
    QImage preview;
    std::array<quint64, 4> nonFiniteCounts{};
};

struct RenderDiagnosticsResult {
    bool success = false;
    QString error;
    quint64 contextGeneration = 0;
    uint64_t sourceEpoch = 0;
    double position = 0.0;
    QSize outputSize;
    int liveTextureCount = 0;
    QString glVendor;
    QString glRenderer;
    QImage framebuffer;
    RenderDiagnosticsGlState glState;
    std::vector<RenderDiagnosticsTexture> textures;
};

}  // namespace mplayer
