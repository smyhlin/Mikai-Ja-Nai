#include "MpvRenderContext.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <QOpenGLContext>
#include <dlfcn.h>
#endif

#include <stdexcept>
#include <string>

#include "MpvHandle.hpp"

namespace mplayer {
namespace {

void* resolve(const char* name) {
#ifdef _WIN32
    // System32 only: the OpenGL driver stack is a Windows dependency, and a
    // bare name would let whatever sits in the current directory answer for it.
    static HMODULE opengl = LoadLibraryExW(L"opengl32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    using WglGetProcAddressFn = void* (*)(const char*);
    static auto wglGetProcAddress =
        opengl ? reinterpret_cast<WglGetProcAddressFn>(
                     reinterpret_cast<void*>(GetProcAddress(opengl, "wglGetProcAddress")))
               : nullptr;
    if (wglGetProcAddress != nullptr) {
        if (void* p = wglGetProcAddress(name)) return p;
    }
    if (opengl != nullptr) {
        if (void* p = reinterpret_cast<void*>(GetProcAddress(opengl, name))) return p;
    }
    return nullptr;
#else
    if (QOpenGLContext* ctx = QOpenGLContext::currentContext()) {
        if (QFunctionPointer fn = ctx->getProcAddress(name)) {
            return reinterpret_cast<void*>(fn);
        }
    }
    static void* libgl = ::dlopen("libGL.so.1", RTLD_LAZY | RTLD_LOCAL);
    if (libgl == nullptr) {
        libgl = ::dlopen("libOpenGL.so.0", RTLD_LAZY | RTLD_LOCAL);
    }
    if (libgl != nullptr) {
        if (void* p = ::dlsym(libgl, name)) return p;
    }
    return nullptr;
#endif
}

// The macro sidesteps both the parenthesisation a function type needs and the
// commas inside a signature, which would otherwise split the argument list.
#define LOAD_GL(field, sig, name) (field) = reinterpret_cast<sig>(resolve(name))

}  // namespace

GlApi loadGlApi() {
    GlApi api;
#ifdef _WIN32
    using GetProcFn = void* (*)(const char*);
    LOAD_GL(api.GetProcAddress, GetProcFn, "wglGetProcAddress");
    if (api.GetProcAddress == nullptr)
        throw MpvError("OpenGL: wglGetProcAddress unavailable - no current GL context");
#endif
    api.GetProcAddress = &resolve;

    LOAD_GL(api.genTextures, void (*)(GLsizei, GLuint*), "glGenTextures");
    LOAD_GL(api.deleteTextures, void (*)(GLsizei, const GLuint*), "glDeleteTextures");
    LOAD_GL(api.bindTexture, void (*)(GLenum, GLuint), "glBindTexture");
    LOAD_GL(api.texImage2D,
            void (*)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*),
            "glTexImage2D");
    LOAD_GL(api.texParameteri, void (*)(GLenum, GLenum, GLint), "glTexParameteri");
    LOAD_GL(api.texImage3D,
            void (*)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLint, GLenum, GLenum, const void*),
            "glTexImage3D");
    LOAD_GL(api.getIntegerv, void (*)(GLenum, GLint*), "glGetIntegerv");
    LOAD_GL(api.getTexLevelParameteriv, void (*)(GLenum, GLint, GLenum, GLint*),
            "glGetTexLevelParameteriv");
    LOAD_GL(api.bindFramebuffer, void (*)(GLenum, GLuint), "glBindFramebuffer");
    LOAD_GL(api.genFramebuffers, void (*)(GLsizei, GLuint*), "glGenFramebuffers");
    LOAD_GL(api.framebufferTexture2D, void (*)(GLenum, GLenum, GLenum, GLuint, GLint),
            "glFramebufferTexture2D");
    LOAD_GL(api.blitFramebuffer,
            void (*)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum),
            "glBlitFramebuffer");
    LOAD_GL(api.viewport, void (*)(GLint, GLint, GLsizei, GLsizei), "glViewport");
    LOAD_GL(api.clearColor, void (*)(GLfloat, GLfloat, GLfloat, GLfloat), "glClearColor");
    LOAD_GL(api.clear, void (*)(GLbitfield), "glClear");
    LOAD_GL(api.flush, void (*)(), "glFlush");
    LOAD_GL(api.readPixels, void (*)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*),
            "glReadPixels");

    if (api.genTextures == nullptr || api.bindTexture == nullptr ||
        api.getTexLevelParameteriv == nullptr || api.getIntegerv == nullptr)
        throw MpvError("OpenGL: required entry points missing (no usable GL context)");

    return api;
}

// ── MpvRenderContext ──────────────────────────────────────────────────────

MpvRenderContext::MpvRenderContext(MpvHandle& mpv, const GlApi& gl, std::function<void()> onUpdate,
                                   bool advancedControl)
    : gl_(gl), onUpdate_(std::move(onUpdate)) {
    MpvApi& api = MpvApi::instance();
    if (!api.loaded()) {
        throw MpvError("no mpv runtime is selected: a render context cannot be created before "
                       "MpvApi::load has succeeded");
    }
    api_ = &api.functions();

    mpv_opengl_init_params init{&MpvRenderContext::sGetProc, this};
    // An int, and passed per creation: mpv reads it once, here.
    int advanced = advancedControl ? 1 : 0;

    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_API_TYPE, const_cast<char*>(MPV_RENDER_API_TYPE_OPENGL)},
        {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &init},
        {MPV_RENDER_PARAM_ADVANCED_CONTROL, &advanced},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };

    const int rc = api_->renderContextCreate(&ctx_, mpv.raw(), params);
    if (rc < 0) {
        ctx_ = nullptr;
        throw MpvError(std::string("mpv_render_context_create: ") + api_->errorString(rc));
    }
    // Registered last, when the target is already in `onUpdate_`: mpv raises the
    // callback from inside this call, and a target assigned afterwards would be
    // reached - from another thread - while still empty.
    api_->renderContextSetUpdateCallback(ctx_, &MpvRenderContext::sUpdate, this);
}

MpvRenderContext::~MpvRenderContext() {
    if (ctx_ != nullptr && api_ != nullptr) {
        // Detach before the mpv handle goes: the callback would otherwise fire
        // against a freed context during teardown.
        api_->renderContextSetUpdateCallback(ctx_, nullptr, nullptr);
        api_->renderContextFree(ctx_);
        ctx_ = nullptr;
    }
}

void* MpvRenderContext::sGetProc(void* ctx, const char* name) {
    auto* self = static_cast<MpvRenderContext*>(ctx);
    return self->gl_.GetProcAddress(name);
}

void MpvRenderContext::sUpdate(void* ctx) {
    auto* self = static_cast<MpvRenderContext*>(ctx);
    if (self != nullptr && self->onUpdate_) self->onUpdate_();
}

void MpvRenderContext::setFlipY(bool flip) {
    // MPV_RENDER_PARAM_FLIP_Y is an int, not the "yes"/"no" string it reads like;
    // passing a char* made mpv read a pointer as a flag and the flip never applied.
    flipY_ = flip ? 1 : 0;
}

uint64_t MpvRenderContext::update() {
    if (ctx_ == nullptr || api_ == nullptr) return 0;
    return api_->renderContextUpdate(ctx_);
}

bool MpvRenderContext::nextFrameInfo(mpv_render_frame_info& info) const {
    if (ctx_ == nullptr || api_ == nullptr || api_->renderContextGetInfo == nullptr)
        return false;
    mpv_render_param p{MPV_RENDER_PARAM_NEXT_FRAME_INFO, &info};
    return api_->renderContextGetInfo(ctx_, p) == 0;
}

bool isRealNewFrame(const mpv_render_frame_info& info) {
    const uint64_t f = info.flags;
    return (f & MPV_RENDER_FRAME_INFO_PRESENT) != 0 &&
           (f & (MPV_RENDER_FRAME_INFO_REDRAW | MPV_RENDER_FRAME_INFO_REPEAT)) == 0;
}

void MpvRenderContext::reportSwap() {
    if (ctx_ != nullptr && api_ != nullptr) api_->renderContextReportSwap(ctx_);
}

int MpvRenderContext::skipRendering() {
    if (ctx_ == nullptr || api_ == nullptr) return 0;
    int skip = 1;
    // Never block the calling thread: mpv's own target-time wait belongs to the
    // presentation path, and a skip is exactly the case where nothing is shown.
    int blockForTargetTime = 0;
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_SKIP_RENDERING, &skip},
        {MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &blockForTargetTime},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };
    return api_->renderContextRender(ctx_, params);
}

int MpvRenderContext::render(int fbo, int width, int height, int internalFormat) {
    if (ctx_ == nullptr || api_ == nullptr || width <= 0 || height <= 0) return 0;

    mpv_opengl_fbo target{};
    target.fbo = fbo;
    target.w = width;
    target.h = height;
    target.internal_format = internalFormat;

    // The requested value, not get_info(): reading it back returns whatever mpv
    // currently holds and discards what the caller asked for.
    int blockForTargetTime = 0;
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_OPENGL_FBO, &target},
        {MPV_RENDER_PARAM_FLIP_Y, &flipY_},
        {MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &blockForTargetTime},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };
    return api_->renderContextRender(ctx_, params);
}

}  // namespace mplayer
