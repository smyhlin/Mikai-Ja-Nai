#include "MpvRenderContext.hpp"

#include <windows.h>

#include <stdexcept>
#include <string>

#include "MpvHandle.hpp"

namespace mplayer {
namespace {

// wglGetProcAddress returns NULL for anything that lives inside opengl32.dll
// itself, which is most of the 1.x entry points, so those need GetProcAddress.
// Getting this wrong yields a NULL function pointer that only crashes once a
// frame actually needs it, so every load is checked.
void* resolve(const char* name) {
    static HMODULE opengl = LoadLibraryA("opengl32.dll");
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
}

// A function-pointer type used as a template argument needs the (*): spelled
// `void (int)` it is a function type, and the compiler then refuses to return
// it by value. The macro sidesteps both the parenthesisation and the fact that
// a comma inside the signature would otherwise split the argument list.
#define LOAD_GL(field, sig, name) (field) = reinterpret_cast<sig>(resolve(name))

}  // namespace

GlApi loadGlApi() {
    GlApi api;
    // The context must be current before these resolve, which is why loading
    // is a separate step the Qt surface performs from initializeGL/paintGL.
    using GetProcFn = void* (*)(const char*);
    LOAD_GL(api.GetProcAddress, GetProcFn, "wglGetProcAddress");
    if (api.GetProcAddress == nullptr)
        throw MpvError("OpenGL: wglGetProcAddress unavailable - no current GL context");
    // Hand mpv the FULL resolver, not the raw wgl entry point. wglGetProcAddress
    // returns NULL for core GL 1.1 symbols (glGetTexLevelParameteriv and
    // friends) because they live inside opengl32.dll; mpv then fails the whole
    // context as "not supported" instead of naming the missing symbol.
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

MpvRenderContext::MpvRenderContext(MpvHandle& mpv, const GlApi& gl) : gl_(&gl), mpv_(&mpv) {
    auto getProc = [](void* ctx, const char* name) -> void* {
        auto* self = static_cast<MpvRenderContext*>(ctx);
        return self->gl_->GetProcAddress(name);
    };
    mpv_opengl_init_params init{getProc, this};

    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_API_TYPE, const_cast<char*>(MPV_RENDER_API_TYPE_OPENGL)},
        {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &init},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };

    int rc = mpv_render_context_create(&ctx_, mpv.get(), params);
    if (rc < 0) {
        ctx_ = nullptr;
        throw MpvError(std::string("mpv_render_context_create: ") + mpv_error_string(rc));
    }
    // A frame is always wanted first; after that the update callback drives us.
    mpv_render_context_set_update_callback(ctx_, &MpvRenderContext::sUpdate, this);
}

MpvRenderContext::~MpvRenderContext() {
    if (ctx_ != nullptr) {
        // Detach before the mpv handle goes: the callback would otherwise fire
        // against a freed context during teardown.
        mpv_render_context_set_update_callback(ctx_, nullptr, nullptr);
        mpv_render_context_free(ctx_);
    }
}

void* MpvRenderContext::sGetProc(void* ctx, const char* name) {
    auto* self = static_cast<MpvRenderContext*>(ctx);
    return self->gl_->GetProcAddress(name);
}

void MpvRenderContext::sUpdate(void* ctx) {
    auto* self = static_cast<MpvRenderContext*>(ctx);
    if (self != nullptr && self->onUpdate_) self->onUpdate_();
}

void MpvRenderContext::setUpdateCallback(std::function<void()> fn) {
    onUpdate_ = std::move(fn);
    // Fire once now so a surface that is already showing a frame gets painted
    // without waiting for the next one mpv decides to produce.
    if (onUpdate_) onUpdate_();
}

void MpvRenderContext::setFlipY(bool flip) {
    // MPV_RENDER_PARAM_FLIP_Y is an int, not the "yes"/"no" string it reads like.
    // Passing a char* here left mpv reading a pointer as a flag, and the flip
    // silently never applied.
    flipY_ = flip ? 1 : 0;
}

uint64_t MpvRenderContext::update() {
    if (ctx_ == nullptr) return 0;
    return mpv_render_context_update(ctx_);
}

void MpvRenderContext::reportSwap() {
    if (ctx_ != nullptr) mpv_render_context_report_swap(ctx_);
}

int MpvRenderContext::render(int fbo, int width, int height, int internalFormat) {
    if (ctx_ == nullptr || width <= 0 || height <= 0) return 0;

    mpv_opengl_fbo target{};
    target.fbo = fbo;
    target.w = width;
    target.h = height;
    target.internal_format = internalFormat;

    mpv_opengl_fbo* fbos[2] = {&target, nullptr};
    // The requested value, not get_info(): reading it back returns whatever mpv
    // currently holds and discards what the caller asked for.
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_OPENGL_FBO, fbos[0]},
        {MPV_RENDER_PARAM_FLIP_Y, &flipY_},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };
    return mpv_render_context_render(ctx_, params);
}

}  // namespace mplayer
