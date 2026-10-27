// Native calls need the creating GL context current; free precedes core destruction.
// Update callbacks run on mpv threads and only hand work to the owner.

#pragma once

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <GL/gl.h>

#include "MpvApi.hpp"

#include <cstdint>
#include <functional>

namespace mplayer {

class MpvHandle;

// Sized internal formats for mpv_opengl_fbo.internal_format. The GL 1.1 headers
// this file includes (notably Windows GL/gl.h) do not define the sized forms,
// and 0 only asks mpv to assume a default 8-bit target: an explicit value lets
// mpv dither and quantize for the FBO the surface really renders into (T-105).
constexpr int kGlInternalFormatRgba8 = 0x8058;
constexpr int kGlInternalFormatRgb10A2 = 0x8059;
constexpr int kGlInternalFormatRgba16 = 0x805B;
constexpr int kGlInternalFormatRgba16F = 0x881A;

// mpv calls back through get_proc_address, so it needs more than the surface.
struct GlApi {
    // Must be the full resolver (wglGetProcAddress plus GetProcAddress on
    // opengl32.dll on Windows, or QOpenGLContext/glX/egl on Linux): raw
    // wglGetProcAddress returns NULL for core GL 1.1 entry points, and mpv then
    // rejects the context as "not supported".
    void* (*GetProcAddress)(const char* name) = nullptr;
    void (*genTextures)(GLsizei n, GLuint* t) = nullptr;
    void (*deleteTextures)(GLsizei n, const GLuint* t) = nullptr;
    void (*bindTexture)(GLenum target, GLuint texture) = nullptr;
    void (*texImage2D)(GLenum t, GLint l, GLint i, GLsizei w, GLsizei h, GLint b,
                       GLenum f, GLenum t2, const void* p) = nullptr;
    void (*texParameteri)(GLenum t, GLenum pname, GLint param) = nullptr;
    void (*texImage3D)(GLenum t, GLint l, GLint i, GLsizei w, GLsizei h, GLint d,
                       GLint b, GLenum f, GLenum t2, const void* p) = nullptr;
    void (*getIntegerv)(GLenum pname, GLint* data) = nullptr;
    void (*getTexLevelParameteriv)(GLenum t, GLint l, GLenum pname, GLint* data) = nullptr;
    void (*bindFramebuffer)(GLenum target, GLuint fb) = nullptr;
    void (*genFramebuffers)(GLsizei n, GLuint* fb) = nullptr;
    void (*framebufferTexture2D)(GLenum t, GLenum a, GLenum texT, GLuint tex, GLint level) = nullptr;
    void (*blitFramebuffer)(GLint sx0, GLint sy0, GLint sx1, GLint sy1,
                            GLint dx0, GLint dy0, GLint dx1, GLint dy1,
                            GLbitfield mask, GLenum filter) = nullptr;
    void (*viewport)(GLint x, GLint y, GLsizei w, GLsizei h) = nullptr;
    void (*clearColor)(GLfloat r, GLfloat g, GLfloat b, GLfloat a) = nullptr;
    void (*clear)(GLbitfield mask) = nullptr;
    void (*flush)() = nullptr;
    void (*readPixels)(GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum t,
                       void* pixels) = nullptr;
};

// Platform GL proc resolver with core-profile fallback.
GlApi loadGlApi();

class MpvRenderContext {
public:
    // Throws MpvError if the context cannot be created.
    //
    // `onUpdate` is held from construction on, so it is already in place when
    // mpv's own callback is registered and a callback mpv raises immediately
    // cannot find an empty target. It fires on an mpv thread: the target has to
    // marshal onto its own thread and must touch no GL state.
    //
    // `advancedControl` makes mpv hand its render loop to the caller: every
    // update callback then has to be answered with update(), every consumed
    // frame with report_swap(), and all of update/render/report_swap/free need
    // the context that created this one current in the calling thread. It is
    // left off for the offscreen users, which do not drive that loop.
    MpvRenderContext(MpvHandle& mpv, const GlApi& gl, std::function<void()> onUpdate,
                     bool advancedControl = false);
    ~MpvRenderContext();

    MpvRenderContext(const MpvRenderContext&) = delete;
    MpvRenderContext& operator=(const MpvRenderContext&) = delete;

    mpv_render_context* get() const noexcept { return ctx_; }
    explicit operator bool() const noexcept { return ctx_ != nullptr; }

    // Caller binds the FBO and calls update() since the last swap. A zero
    // depthBitsPerChannel uses mpv's documented 8-bit default for unknown targets.
    int render(int fbo, int width, int height, int internalFormat, int depthBitsPerChannel);

    // Tells mpv a frame was presented, so it can schedule the next. Required for
    // every frame mpv handed out under advanced control, including one that was
    // consumed with skipRendering(): mpv waits for that acknowledgement before
    // it releases the next frame.
    void reportSwap();

    // Consumes the next frame without rendering it, so a surface that cannot
    // paint still keeps mpv's timing and frame supply alive. Takes the same
    // current GL context as render(), and returns mpv's render error code.
    int skipRendering();

    // Returns mpv's MPV_RENDER_UPDATE_* flags describing new render output.
    uint64_t update();

    bool nextFrameInfo(mpv_render_frame_info& info) const;
    int64_t timeUs() const;

    // Set once, right after construction, before the first render.
    void setFlipY(bool flip);

private:
    // 1 to flip, 0 not. Always passed per-render; never read back from mpv.
    int flipY_ = 1;

    static void* sGetProc(void* ctx, const char* name);
    static void sUpdate(void* ctx);

    mpv_render_context* ctx_ = nullptr;
    mpv_handle* mpv_ = nullptr;
    // Held by value: mpv calls get_proc_address for the whole life of the
    // context, so this cannot be a caller's local.
    GlApi gl_;
    // Cached from the process-wide selection so render never looks the module up.
    const MpvApiInstance* api_ = nullptr;
    // In place before mpv's callback is registered; never reassigned afterwards,
    // so the callback mpv raises during registration always reaches a target.
    std::function<void()> onUpdate_;
};

bool isRealNewFrame(const mpv_render_frame_info& info);

}  // namespace mplayer
