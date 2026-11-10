#include "MpvRenderContext.hpp"

#include <QOpenGLContext>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <stdexcept>
#include <string>

#include "MpvHandle.hpp"

namespace mplayer {
namespace {

void* resolve(const char* name) {
    if (QOpenGLContext* ctx = QOpenGLContext::currentContext()) {
        if (QFunctionPointer fn = ctx->getProcAddress(name)) {
            return reinterpret_cast<void*>(fn);
        }
    }
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
        void* p = wglGetProcAddress(name);
        if (p != nullptr && p != reinterpret_cast<void*>(1) &&
            p != reinterpret_cast<void*>(2) && p != reinterpret_cast<void*>(3) &&
            p != reinterpret_cast<void*>(-1)) {
            return p;
        }
    }
    if (opengl != nullptr) {
        if (void* p = reinterpret_cast<void*>(GetProcAddress(opengl, name))) return p;
    }
    return nullptr;
#else
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

}  // namespace

GlApi loadGlApi() {
    GlApi api;
    // resolve() already implements the full fallback chain
    // (QOpenGLContext::getProcAddress, then wglGetProcAddress with 1/2/3/-1
    // rejection, then GetProcAddress(opengl32.dll); dlopen libGL/libOpenGL
    // on non-Windows), so no separate wgl load is needed here. The
    // readiness probe goes through that resolver only - never a raw
    // wglGetProcAddress("glGetString").
    api.GetProcAddress = &resolve;
    if (resolve("glGetString") == nullptr || resolve("glGetIntegerv") == nullptr)
        throw MpvError("OpenGL: required entry points missing (no usable GL context)");

    return api;
}

// ── MpvRenderContext ──────────────────────────────────────────────────────

MpvRenderContext::MpvRenderContext(MpvHandle& mpv, const GlApi& gl, std::function<void()> onUpdate,
                                   bool advancedControl)
    : mpv_(mpv.raw()), gl_(gl), onUpdate_(std::move(onUpdate)) {
    MpvApi& api = MpvApi::instance();
    if (!api.loaded()) {
        throw MpvError("no mpv runtime is selected: a render context cannot be created before "
                       "MpvApi::load has succeeded");
    }
    api_ = &api.functions();

    // Advanced control hands mpv's loop to the caller: every callback must be
    // answered with update(), which is exactly what onUpdate_ is for. A caller
    // that asks for the mode without a callback leaves mpv waiting on frames
    // nobody draws, so the request is refused here instead of failing later as
    // a stalled video output.
    if (advancedControl && !onUpdate_) {
        throw MpvError("advanced control requested but no update callback was provided");
    }
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
    if (self == nullptr || name == nullptr || self->gl_.GetProcAddress == nullptr)
        return nullptr;
    return self->gl_.GetProcAddress(name);
}

void MpvRenderContext::sUpdate(void* ctx) {
    auto* self = static_cast<MpvRenderContext*>(ctx);
    if (self != nullptr && self->onUpdate_) self->onUpdate_();
}

void MpvRenderContext::setFlipY(bool flip) {
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

int64_t MpvRenderContext::timeUs() const {
    return mpv_ != nullptr && api_ != nullptr && api_->getTimeUs != nullptr
               ? api_->getTimeUs(mpv_)
               : 0;
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

int MpvRenderContext::render(int fbo, int width, int height, int internalFormat,
                             int depthBitsPerChannel) {
    if (ctx_ == nullptr || api_ == nullptr || width <= 0 || height <= 0) return 0;

    mpv_opengl_fbo target{};
    target.fbo = fbo;
    target.w = width;
    target.h = height;
    target.internal_format = internalFormat;

    int blockForTargetTime = 0;
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_OPENGL_FBO, &target},
        {MPV_RENDER_PARAM_FLIP_Y, &flipY_},
        {MPV_RENDER_PARAM_DEPTH, &depthBitsPerChannel},
        {MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &blockForTargetTime},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };
    return api_->renderContextRender(ctx_, params);
}

}  // namespace mplayer
