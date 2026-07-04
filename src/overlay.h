#pragma once
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <linux/fb.h>

struct OverlayWindow {
    int fb_fd = -1;
    void* fb_mmap = nullptr;
    size_t fb_size = 0;
    int fb_stride = 0;  // bytes per line
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLSurface eglSurface = EGL_NO_SURFACE;
    EGLContext context = EGL_NO_CONTEXT;
    int width = 0, height = 0;
};

bool overlay_init(OverlayWindow* ov, int w, int h);
void overlay_destroy(OverlayWindow* ov);
void overlay_swap(OverlayWindow* ov);
void overlay_clear(OverlayWindow* ov, float r, float g, float b, float a);