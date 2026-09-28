// Real pixel proof with diagnostics: load a test stream, watch mpv's frame-
// generation counter, render into a core FBO, read the texture back, and
// verify the frame is not uniform black.
#include "MpvHandle.hpp"
#include "MpvRenderContext.hpp"

#include <GL/glext.h>
#include <windows.h>

#include <cstdio>
#include <vector>

using namespace mplayer;

static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    return DefWindowProcW(h, m, w, l);
}
#define SAY(...)                                                             \
    do {                                                                     \
        std::printf(__VA_ARGS__);                                            \
        std::fflush(stdout);                                                 \
    } while (0)

int main() {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof(wc), CS_OWNDC, proc, 0, 0, inst, nullptr, nullptr,
                   nullptr, nullptr, L"px", nullptr};
    RegisterClassExW(&wc);
    HWND win = CreateWindowExW(0, L"px", L"px", WS_OVERLAPPEDWINDOW, 0, 0, 640, 360,
                               nullptr, nullptr, inst, nullptr);
    HDC dc = GetDC(win);
    PIXELFORMATDESCRIPTOR pfd{sizeof(pfd), 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER,
                              PFD_TYPE_RGBA, 32, 0, 0, 0, 0, 0, 0, 0, 0, 0, 24, 0, 0,
                              PFD_MAIN_PLANE, 0, 0, 0, 0};
    int pf = ChoosePixelFormat(dc, &pfd);
    SetPixelFormat(dc, pf, &pfd);
    HGLRC glc = wglCreateContext(dc);
    wglMakeCurrent(dc, glc);
    SAY("GL %s\n", (const char*)glGetString(GL_VERSION));

    GlApi api = loadGlApi();
    GLuint tex = 0;
    GLuint fbo = 0;
    api.genTextures(1, &tex);
    api.genFramebuffers(1, &fbo);
    api.bindTexture(GL_TEXTURE_2D, tex);
    api.texImage2D(GL_TEXTURE_2D, 0, GL_RGB, 320, 180, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
    api.bindFramebuffer(GL_FRAMEBUFFER, fbo);
    api.framebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    api.bindFramebuffer(GL_FRAMEBUFFER, 0);
    SAY("fbo=%u tex=%u\n", fbo, tex);

    try {
        MpvHandle mpv({{ "vo", "libmpv" }});
        MpvRenderContext rc(mpv, api);
        rc.setFlipY(false);
        rc.setUpdateCallback([] {});
        SAY("rc ok; loading stream\n");
        mpv.loadFile("av://lavfi:testsrc=size=320x180:rate=25");

        uint64_t lastGen = 0;
        mpv_render_param params[]{};
        for (int frame = 0; frame < 150; ++frame) {
            Sleep(12);
            uint64_t gen = rc.update();
            if (gen != lastGen) {
                SAY("generation -> %llu (frame %d)\n", (unsigned long long)gen, frame);
                lastGen = gen;
            }
            api.bindFramebuffer(GL_FRAMEBUFFER, fbo);
            int rr = rc.render(fbo, 320, 180, 0);
            if (frame % 20 == 0)
                SAY("render rc at %d: %d (%s)\n", frame, rr, mpv_error_string(rr));
            rc.reportSwap();
            api.bindFramebuffer(GL_FRAMEBUFFER, 0);
        }

        std::vector<unsigned char> px(320 * 180 * 3, 0);
        glFinish();
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        api.bindTexture(GL_TEXTURE_2D, tex);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGB, GL_UNSIGNED_BYTE, px.data());
        unsigned long long sum = 0;
        int nonblack = 0;
        for (size_t i = 0; i < px.size(); i += 3) {
            unsigned v = (unsigned)px[i] + px[i + 1] + px[i + 2];
            sum += v;
            if (v > 12) ++nonblack;
        }
        SAY("pixels: sum=%llu nonblack=%d of %d\n", sum, nonblack, 320 * 180);
        SAY("PASS %s\n", nonblack > 320L * 180 / 2 ? "YES" : "NO");
    } catch (const MpvError& e) {
        SAY("MpvError: %s\n", e.what());
        return 9;
    }
    return 0;
}