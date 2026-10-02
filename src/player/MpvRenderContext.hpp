// mpv render-API bridge: mpv renders into an FBO we blit inside a normal Qt
// widget, so there is no second HWND and native z-order stops mattering.
//
// The update callback fires on an arbitrary mpv thread and is forwarded as a
// plain std::function; the Qt layer marshals it onto the GUI thread.

#pragma once

// gl.h must come first: GlApi is spelled in plain GL scalar types, and glext.h
// is not shipped by the Windows SDK so it is not included.
#include <windows.h>

#include <GL/gl.h>

#include "MpvApi.hpp"

#include <cstdint>
#include <functional>

namespace mplayer {

class MpvHandle;

// mpv calls back through get_proc_address, so it needs more than the surface.
struct GlApi {
    // Must be the full resolver (wglGetProcAddress plus GetProcAddress on
    // opengl32.dll): raw wglGetProcAddress returns NULL for core GL 1.1 entry
    // points, and mpv then rejects the context as "not supported".
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

// wglGetProcAddress with a GetProcAddress fallback, as core-profile drivers need.
GlApi loadGlApi();

class MpvRenderContext {
public:
    // Throws MpvError if the context cannot be created.
    MpvRenderContext(MpvHandle& mpv, const GlApi& gl);
    ~MpvRenderContext();

    MpvRenderContext(const MpvRenderContext&) = delete;
    MpvRenderContext& operator=(const MpvRenderContext&) = delete;

    mpv_render_context* get() const noexcept { return ctx_; }
    explicit operator bool() const noexcept { return ctx_ != nullptr; }

    // Caller must have the FBO bound and mpv's update() called since the last
    // swap. Returns mpv's render error code (0 on success).
    int render(int fbo, int width, int height, int internalFormat);

    // Tells mpv a frame was presented, so it can schedule the next.
    void reportSwap();

    // Cheap, call once per paint. Returns mpv's frame-generation counter, zero
    // if no frame yet.
    uint64_t update();

    // Fires (on an mpv thread) when a new frame may be available.
    void setUpdateCallback(std::function<void()> fn);

    // Set once, right after construction, before the first render.
    void setFlipY(bool flip);

private:
    // 1 to flip, 0 not. Always passed per-render; never read back from mpv.
    int flipY_ = 1;

    static void* sGetProc(void* ctx, const char* name);
    static void sUpdate(void* ctx);

    mpv_render_context* ctx_ = nullptr;
    // Held by value: mpv calls get_proc_address for the whole life of the
    // context, so this cannot be a caller's local.
    GlApi gl_;
    // Cached from the process-wide selection so render never looks the module up.
    const MpvApiInstance* api_ = nullptr;
    std::function<void()> onUpdate_;
};

}  // namespace mplayer
