#pragma once
#include <EGL/egl.h>
#include <GLES3/gl3.h>

struct OverlayWindow {
    // SurfaceComposer 对象 (opaque, 运行时通过 dlopen 分配)
    void* scc = nullptr;       // android::SurfaceComposerClient*
    void* surfaceControl = nullptr;  // android::sp<SurfaceControl>
    void* surface = nullptr;   // android::Surface* (IS-A ANativeWindow*)

    // EGL
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLSurface eglSurface = EGL_NO_SURFACE;
    EGLContext context = EGL_NO_CONTEXT;

    int width = 0, height = 0;
};

bool overlay_init(OverlayWindow* ov, int w, int h);
void overlay_destroy(OverlayWindow* ov);
void overlay_swap(OverlayWindow* ov);
void overlay_clear(OverlayWindow* ov, float r, float g, float b, float a);