// mpv render-API bridge.
//
// This is the whole point of the C++ player. The Python player embedded mpv by
// handing it a native HWND (mpv calls winId()), and a native child window on
// Windows ALWAYS composites above non-native siblings regardless of raise().
// That is the entire reason the dock kept disappearing behind the video, the
// chrome had to be "promoted native" and re-raised in three places, and the
// float needed a native overlay hack.
//
// The render API removes that class of bug outright: mpv renders into a
// framebuffer object and we blit that texture ourselves inside a normal Qt
// widget. There is no second HWND, so z-order stops being a concern.
//
// Threading: mpv_render_context_set_update_callback fires on an arbitrary mpv
// thread. The callback is forwarded as a plain std::function; the Qt layer is
// responsible for marshalling it onto the GUI thread. Nothing here touches Qt,
// so this file stays buildable and testable without a window system.

#pragma once

// mpv/render_gl.h uses the GL scalar types but does not include the GL
// headers itself, so they have to come first.
#include <windows.h>

#include <GL/gl.h>
#include <GL/glext.h>

#include <mpv/client.h>
#include <mpv/render_gl.h>

#include <cstdint>
#include <functional>

namespace mplayer {

class MpvHandle;

// GL entry points mpv needs. mpv calls back through get_proc_address, so the
// loader must resolve more than the surface itself needs.
struct GlApi {
    // Must be the full resolver (wglGetProcAddress PLUS a GetProcAddress on
    // opengl32.dll), not raw wglGetProcAddress: the latter returns NULL for every
    // core GL 1.1 entry point, and mpv then rejects the context as
    // "not supported" rather than telling us which symbol went missing.
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

// Resolves the subset above through wglGetProcAddress with a GetProcAddress
// fallback, which is what core-profile drivers need on Windows.
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

    // Renders one frame into `fbo` sized width x height. Caller must have the
    // FBO bound and mpv's update() called since the last swap.
    int render(int fbo, int width, int height, int internalFormat);
    // Returns mpv's render error code (0 on success).

    // Tells mpv a frame was presented, so it can schedule the next.
    void reportSwap();

    // Asks mpv whether a new frame is ready; cheap, call once per paint.
    // Returns mpv's frame-generation counter; zero means no frame yet.
    uint64_t update();

    // Fires (on an mpv thread) when a new frame may be available.
    void setUpdateCallback(std::function<void()> fn);

    // Set once, right after construction, before the first render.
    void setFlipY(bool flip);

private:
    static void* sGetProc(void* ctx, const char* name);
    static void sUpdate(void* ctx);

    mpv_render_context* ctx_ = nullptr;
    const GlApi* gl_ = nullptr;
    MpvHandle* mpv_ = nullptr;
    std::function<void()> onUpdate_;
};

}  // namespace mplayer
