#pragma once
#include <gui/SurfaceComposerClient.h>
#include <gui/Surface.h>
#include <gui/SurfaceControl.h>
#include <ui/DisplayInfo.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>

struct OverlayWindow {
    android::sp<android::SurfaceComposerClient> client;
    android::sp<android::SurfaceControl> ctrl;
    android::sp<android::Surface> surface;
    ANativeWindow* window = nullptr;
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLSurface eglSurface = EGL_NO_SURFACE;
    EGLContext context = EGL_NO_CONTEXT;
    int width = 0, height = 0;
};

bool overlay_init(OverlayWindow* ov, int w, int h);
void overlay_destroy(OverlayWindow* ov);
void overlay_swap(OverlayWindow* ov);
void overlay_clear(OverlayWindow* ov, float r, float g, float b, float a);