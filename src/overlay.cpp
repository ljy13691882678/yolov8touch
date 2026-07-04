#include "overlay.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <dlfcn.h>
#include <android/native_window.h>

// ═══════════════════════════════════════════════════════════════════
//  运行时动态加载 libgui.so / libutils.so 符号
//  避免 stub 头文件的 ABI 不匹配问题 — 使用设备自带的真实库
// ═══════════════════════════════════════════════════════════════════

// ─── 函数指针类型 ────────────────────────────────────────────────
typedef void  (*SCC_ctor_t)(void* self);
typedef void* (*SCC_createSurface_t)(void* self, void* nameObj, uint32_t w, uint32_t h, int32_t format, uint32_t flags);
typedef void* (*SC_getSurface_t)(void* sc);
typedef void  (*Transaction_ctor_t)(void* self);
typedef void* (*Transaction_setLayer_t)(void* self, void* sc, int32_t layer);
typedef void* (*Transaction_setPos_t)(void* self, void* sc, float x, float y);
typedef void* (*Transaction_show_t)(void* self, void* sc);
typedef int   (*Transaction_apply_t)(void* self);
typedef void  (*String8_ctor_t)(void* self, const char* s);

// ─── 全局函数指针 ────────────────────────────────────────────────
static void* g_libgui = nullptr;
static void* g_libutils = nullptr;

static SCC_ctor_t            p_SCC_ctor = nullptr;
static SCC_createSurface_t   p_SCC_createSurface = nullptr;
static SC_getSurface_t       p_SC_getSurface = nullptr;
static Transaction_ctor_t    p_Transaction_ctor = nullptr;
static Transaction_setLayer_t p_Transaction_setLayer = nullptr;
static Transaction_setPos_t  p_Transaction_setPosition = nullptr;
static Transaction_show_t    p_Transaction_show = nullptr;
static Transaction_apply_t   p_Transaction_apply = nullptr;
static String8_ctor_t        p_String8_ctor = nullptr;

// ─── 对象缓冲区大小 (足够大，避免溢出) ────────────────────────────
#define SCC_BUF_SIZE        1024
#define STRING8_BUF_SIZE    64
#define SURFACE_CONTROL_BUF_SIZE  512
#define TRANSACTION_BUF_SIZE      512

// ─── 尝试多个可能的 mangled name ─────────────────────────────────
static void* try_dlsym(void* lib, const char** names, int count) {
    for (int i = 0; i < count; i++) {
        void* p = dlsym(lib, names[i]);
        if (p) return p;
    }
    return nullptr;
}

static bool load_symbols() {
    // 加载库
    g_libgui = dlopen("libgui.so", RTLD_NOW);
    if (!g_libgui) {
        fprintf(stderr, "DLOPEN: libgui.so not found\n");
        return false;
    }
    g_libutils = dlopen("libutils.so", RTLD_NOW);
    if (!g_libutils) {
        fprintf(stderr, "DLOPEN: libutils.so not found\n");
        return false;
    }

    // String8 构造函数 (libutils.so)
    const char* string8_names[] = {
        "_ZN7android7String8C1EPKc",      // clang 标准
        "_ZN7android8String16C1EPKc",     // 有些版本用 String16
        nullptr
    };
    p_String8_ctor = (String8_ctor_t)try_dlsym(g_libutils, string8_names, 2);
    if (!p_String8_ctor) {
        fprintf(stderr, "DLOPEN: String8 ctor not found\n");
        return false;
    }

    // SurfaceComposerClient 构造函数
    const char* scc_names[] = {
        "_ZN7android21SurfaceComposerClientC1Ev",
        "_ZN7android21SurfaceComposerClientC2Ev",
        nullptr
    };
    p_SCC_ctor = (SCC_ctor_t)try_dlsym(g_libgui, scc_names, 2);
    if (!p_SCC_ctor) { fprintf(stderr, "DLOPEN: SCC ctor not found\n"); return false; }

    // createSurface
    const char* cs_names[] = {
        "_ZN7android21SurfaceComposerClient13createSurfaceERKNS_7String8Eiiij",
        "_ZN7android21SurfaceComposerClient13createSurfaceERKNS_7String8EiiijPNS_14SurfaceControlE",
        "_ZN7android21SurfaceComposerClient13createSurfaceERKNS_7String8EiiijPNS_14SurfaceControlERKNS_13LayerMetadataE",
        nullptr
    };
    p_SCC_createSurface = (SCC_createSurface_t)try_dlsym(g_libgui, cs_names, 3);
    if (!p_SCC_createSurface) { fprintf(stderr, "DLOPEN: createSurface not found\n"); return false; }

    // SurfaceControl::getSurface
    const char* gs_names[] = {
        "_ZN7android14SurfaceControl10getSurfaceEv",
        nullptr
    };
    p_SC_getSurface = (SC_getSurface_t)try_dlsym(g_libgui, gs_names, 1);
    if (!p_SC_getSurface) { fprintf(stderr, "DLOPEN: getSurface not found\n"); return false; }

    // Transaction 构造函数
    const char* tr_names[] = {
        "_ZN7android21SurfaceComposerClient11TransactionC1Ev",
        "_ZN7android21SurfaceComposerClient11TransactionC2Ev",
        nullptr
    };
    p_Transaction_ctor = (Transaction_ctor_t)try_dlsym(g_libgui, tr_names, 2);

    // Transaction::setLayer
    const char* sl_names[] = {
        "_ZN7android21SurfaceComposerClient11Transaction8setLayerERKNS_2spINS_14SurfaceControlEEEi",
        nullptr
    };
    p_Transaction_setLayer = (Transaction_setLayer_t)try_dlsym(g_libgui, sl_names, 1);

    // Transaction::setPosition
    const char* sp_names[] = {
        "_ZN7android21SurfaceComposerClient11Transaction11setPositionERKNS_2spINS_14SurfaceControlEEEff",
        nullptr
    };
    p_Transaction_setPosition = (Transaction_setPos_t)try_dlsym(g_libgui, sp_names, 1);

    // Transaction::show
    const char* sh_names[] = {
        "_ZN7android21SurfaceComposerClient11Transaction4showERKNS_2spINS_14SurfaceControlEEE",
        nullptr
    };
    p_Transaction_show = (Transaction_show_t)try_dlsym(g_libgui, sh_names, 1);

    // Transaction::apply
    const char* ap_names[] = {
        "_ZN7android21SurfaceComposerClient11Transaction5applyEv",
        nullptr
    };
    p_Transaction_apply = (Transaction_apply_t)try_dlsym(g_libgui, ap_names, 1);

    fprintf(stderr, "DLOPEN: all symbols loaded\n");
    return true;
}

// ═══════════════════════════════════════════════════════════════════
//  overlay_init / overlay_destroy
// ═══════════════════════════════════════════════════════════════════
bool overlay_init(OverlayWindow* ov, int w, int h) {
    memset(ov, 0, sizeof(*ov));
    ov->width = w; ov->height = h;
    ov->display = EGL_NO_DISPLAY;
    ov->eglSurface = EGL_NO_SURFACE;
    ov->context = EGL_NO_CONTEXT;

    if (!load_symbols()) return false;

    // ─── 1. 创建 SurfaceComposerClient ───────────────────────────
    ov->scc = malloc(SCC_BUF_SIZE);
    memset(ov->scc, 0, SCC_BUF_SIZE);
    p_SCC_ctor(ov->scc);
    fprintf(stderr, "SurfaceComposerClient created\n");

    // ─── 2. 构造 String8 ─────────────────────────────────────────
    char nameBuf[STRING8_BUF_SIZE];
    memset(nameBuf, 0, sizeof(nameBuf));
    p_String8_ctor(nameBuf, "yolov8touch");

    // ─── 3. createSurface ────────────────────────────────────────
    //     PIXEL_FORMAT_RGBA_8888 = 1
    //     flags: ISurfaceComposerClient::eHidden = 0x4 (hidden initially)
    //     flags: 0 = visible by default
    void* sc = p_SCC_createSurface(ov->scc, nameBuf, (uint32_t)w, (uint32_t)h, 1, 0);
    if (!sc) {
        fprintf(stderr, "ERR: createSurface returned null\n");
        return false;
    }
    ov->surfaceControl = sc;
    fprintf(stderr, "SurfaceControl created: %dx%d\n", w, h);

    // ─── 4. getSurface ───────────────────────────────────────────
    void* surface = p_SC_getSurface(ov->surfaceControl);
    if (!surface) {
        fprintf(stderr, "ERR: getSurface returned null\n");
        return false;
    }
    ov->surface = surface;
    fprintf(stderr, "Surface obtained: %p\n", surface);

    // ─── 5. 设置 layer 并显示 (Transaction) ─────────────────────
    if (p_Transaction_ctor && p_Transaction_setLayer && p_Transaction_show && p_Transaction_apply) {
        char txBuf[TRANSACTION_BUF_SIZE];
        memset(txBuf, 0, sizeof(txBuf));
        p_Transaction_ctor(txBuf);

        // sp<SurfaceControl> 参数: 传入指针的指针
        p_Transaction_setLayer(txBuf, &ov->surfaceControl, 0x7fffffff);  // INT_MAX = 最顶层
        p_Transaction_show(txBuf, &ov->surfaceControl);
        int ret = p_Transaction_apply(txBuf);
        fprintf(stderr, "Transaction applied: ret=%d\n", ret);
    }

    // ─── 6. EGL 初始化 (window surface) ──────────────────────────
    //     Surface* IS-A ANativeWindow*, 可以直接转换
    ANativeWindow* nativeWin = (ANativeWindow*)ov->surface;
    nativeWin = nativeWin;  // 抑制未使用警告

    ov->display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (ov->display == EGL_NO_DISPLAY) {
        fprintf(stderr, "ERR: eglGetDisplay\n"); return false;
    }

    EGLint major, minor;
    if (!eglInitialize(ov->display, &major, &minor)) {
        fprintf(stderr, "ERR: eglInitialize\n"); return false;
    }

    const EGLint cfgAttribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24,
        EGL_NONE
    };
    EGLConfig cfg; EGLint numCfg;
    if (!eglChooseConfig(ov->display, cfgAttribs, &cfg, 1, &numCfg) || numCfg == 0) {
        fprintf(stderr, "ERR: eglChooseConfig\n"); return false;
    }

    const EGLint ctxAttribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    ov->context = eglCreateContext(ov->display, cfg, EGL_NO_CONTEXT, ctxAttribs);
    if (ov->context == EGL_NO_CONTEXT) {
        fprintf(stderr, "ERR: eglCreateContext\n"); return false;
    }

    // 设置 ANativeWindow 缓冲区大小
    ANativeWindow_setBuffersGeometry((ANativeWindow*)ov->surface, w, h, 1);  // RGBA_8888

    ov->eglSurface = eglCreateWindowSurface(ov->display, cfg, (ANativeWindow*)ov->surface, nullptr);
    if (ov->eglSurface == EGL_NO_SURFACE) {
        fprintf(stderr, "ERR: eglCreateWindowSurface (EGL error: %d)\n", eglGetError());
        return false;
    }

    if (!eglMakeCurrent(ov->display, ov->eglSurface, ov->eglSurface, ov->context)) {
        fprintf(stderr, "ERR: eglMakeCurrent\n"); return false;
    }

    glViewport(0, 0, ov->width, ov->height);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    fprintf(stderr, "Overlay created: %dx%d, EGL %d.%d\n", ov->width, ov->height, major, minor);
    return true;
}

void overlay_destroy(OverlayWindow* ov) {
    if (ov->display != EGL_NO_DISPLAY) {
        eglMakeCurrent(ov->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (ov->eglSurface != EGL_NO_SURFACE) eglDestroySurface(ov->display, ov->eglSurface);
        if (ov->context != EGL_NO_CONTEXT) eglDestroyContext(ov->display, ov->context);
        eglTerminate(ov->display);
    }
    free(ov->scc);
    memset(ov, 0, sizeof(*ov));
}

void overlay_swap(OverlayWindow* ov) {
    if (ov->display != EGL_NO_DISPLAY) {
        eglSwapBuffers(ov->display, ov->eglSurface);
    }
}

void overlay_clear(OverlayWindow* ov, float r, float g, float b, float a) {
    (void)ov;
    glClearColor(r, g, b, a);
    glClear(GL_COLOR_BUFFER_BIT);
}