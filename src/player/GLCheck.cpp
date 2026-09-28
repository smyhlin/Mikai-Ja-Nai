#include <windows.h>
#include <GL/gl.h>
#include <cstdio>
static LRESULT CALLBACK proc(HWND h,UINT m,WPARAM w,LPARAM l){return DefWindowProcW(h,m,w,l);}
int main(){
    HINSTANCE inst=GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof(wc),CS_OWNDC,proc,0,0,inst,nullptr,nullptr,nullptr,nullptr,L"glc",nullptr};
    RegisterClassExW(&wc);
    HWND win=CreateWindowExW(0,L"glc",L"glc",WS_OVERLAPPEDWINDOW,0,0,320,240,nullptr,nullptr,inst,nullptr);
    HDC dc=GetDC(win);
    PIXELFORMATDESCRIPTOR pfd{sizeof(pfd),1,PFD_DRAW_TO_WINDOW|PFD_SUPPORT_OPENGL|PFD_DOUBLEBUFFER,
        PFD_TYPE_RGBA,32,0,0,0,0,0,0,0,0,0,24,0,0,PFD_MAIN_PLANE,0,0,0,0};
    int pf=ChoosePixelFormat(dc,&pfd); SetPixelFormat(dc,pf,&pfd);
    HGLRC gl=wglCreateContext(dc); wglMakeCurrent(dc,gl);
    std::printf("GL_VERSION  : %s\n",(const char*)glGetString(GL_VERSION));
    std::printf("GL_RENDERER : %s\n",(const char*)glGetString(GL_RENDERER));
    std::fflush(stdout);
    return 0;
}
