#include "overlay.h"
#include <cstdio>

bool overlay_init(OverlayWindow* ov, int w, int h) {
    ov->width = w; ov->height = h;
    // 创建 SurfaceComposerClient
    ov->client = new android::SurfaceComposerClient();
    if (ov->client->initCheck() != android::NO_ERROR) {
        fprintf(stderr, "ERR: SurfaceComposerClient init failed\n");
        return false;
    }
    // 创建透明 Surface
    ov->ctrl = ov->client->createSurface(
        android::String8("yolov8touch"),
        w, h,
        android::PIXEL_FORMAT_RGBA_8888,
        0  // flags
    );
    if (!ov->ctrl.get()) {
        fprintf(stderr, "ERR: createSurface failed\n");
        return false;
    }
    // 设置透明 + 顶层
    android::SurfaceComposerClient::Transaction t;
    t.setLayer(ov->ctrl, 0x7fffffff);  // INT_MAX, topmost
    t.setAlpha(ov->ctrl, 0.0f);  // 透明背景
    t.show(ov->ctrl);
    t.apply();
    ov->surface = ov->ctrl->getSurface();
    ov->window = ov->surface.get();
    // EGL
    ov->display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (ov->display == EGL_NO_DISPLAY) { fprintf(stderr, "ERR: eglGetDisplay\n"); return false; }
    EGLint major, minor;
    if (!eglInitialize(ov->display, &major, &minor)) { fprintf(stderr, "ERR: eglInitialize\n"); return false; }
    const EGLint cfgAttribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24,
        EGL_NONE
    };
    EGLConfig cfg; EGLint numCfg;
    eglChooseConfig(ov->display, cfgAttribs, &cfg, 1, &numCfg);
    const EGLint ctxAttribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    ov->context = eglCreateContext(ov->display, cfg, EGL_NO_CONTEXT, ctxAttribs);
    const EGLint surfAttribs[] = { EGL_NONE };
    ov->eglSurface = eglCreateWindowSurface(ov->display, cfg, ov->window, surfAttribs);
    eglMakeCurrent(ov->display, ov->eglSurface, ov->eglSurface, ov->context);
    glViewport(0, 0, w, h);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    fprintf(stderr, "Overlay created: %dx%d, EGL %d.%d\n", w, h, major, minor);
    return true;
}

void overlay_destroy(OverlayWindow* ov) {
    if (ov->display != EGL_NO_DISPLAY) {
        eglMakeCurrent(ov->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (ov->eglSurface != EGL_NO_SURFACE) eglDestroySurface(ov->display, ov->eglSurface);
        if (ov->context != EGL_NO_CONTEXT) eglDestroyContext(ov->display, ov->context);
        eglTerminate(ov->display);
    }
    if (ov->ctrl.get()) {
        android::SurfaceComposerClient::Transaction t;
        t.hide(ov->ctrl); t.apply();
        ov->ctrl.clear();
    }
    ov->client.clear();
}

void overlay_swap(OverlayWindow* ov) {
    eglSwapBuffers(ov->display, ov->eglSurface);
}

void overlay_clear(OverlayWindow* ov, float r, float g, float b, float a) {
    glClearColor(r, g, b, a);
    glClear(GL_COLOR_BUFFER_BIT);
}