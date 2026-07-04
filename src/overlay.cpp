#include "overlay.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <dlfcn.h>
#include <link.h>
#include <elf.h>
#include <cxxabi.h>
#include <android/native_window.h>

// ═══════════════════════════════════════════════════════════════════
//  运行时 ELF 符号扫描 — 自动适配不同 Android 版本的 ABI
// ═══════════════════════════════════════════════════════════════════

// ─── 函数指针类型 ────────────────────────────────────────────────
typedef void  (*SCC_ctor_t)(void* self);
typedef void* (*SC_getSurface_t)(void* sc);
typedef void  (*Transaction_ctor_t)(void* self);
typedef void* (*Transaction_setLayer_t)(void* self, void* sc, int32_t layer);
typedef void* (*Transaction_show_t)(void* self, void* sc);
typedef int   (*Transaction_apply_t)(void* self);
typedef void  (*String8_ctor_t)(void* self, const char* s);

// createSurface — 有两套 ABI:
// v6 (Android <14): 6 params  (self, name, w, h, format, flags)
// v8 (Android 14+): 9 params  (self, name, w, h, format, flags, parent, metadata, outId)
typedef void* (*createSurface_v6_t)(void* self, void* name, uint32_t w, uint32_t h, int32_t format, uint32_t flags);
typedef void* (*createSurface_v8_t)(void* self, void* name, uint32_t w, uint32_t h, int32_t format, int32_t flags,
                                     void* parent, void* metadata, uint32_t* outId);

static void* g_libgui = nullptr;
static void* g_libutils = nullptr;

static SCC_ctor_t            p_SCC_ctor = nullptr;
static void*                p_SCC_createSurface_raw = nullptr;  // 原始地址
static int                  g_createSurface_version = 0;        // 6 or 8
static SC_getSurface_t       p_SC_getSurface = nullptr;
static Transaction_ctor_t    p_Transaction_ctor = nullptr;
static Transaction_setLayer_t p_Transaction_setLayer = nullptr;
static Transaction_show_t    p_Transaction_show = nullptr;
static Transaction_apply_t   p_Transaction_apply = nullptr;
static String8_ctor_t        p_String8_ctor = nullptr;

#define SCC_BUF_SIZE        1024
#define STRING8_BUF_SIZE    64
#define TRANSACTION_BUF_SIZE 512
#define LAYER_METADATA_SIZE  1024  // 足够大的零缓冲区

// ═══════════════════════════════════════════════════════════════════
//  ELF 符号表扫描
// ═══════════════════════════════════════════════════════════════════

struct FindCtx {
    const char* libname;
    const char* substr;
    void*       result;
    char*       demangled_name;  // 输出: demangled 全名 (用于检测 ABI 版本)
};

static int phdr_callback(struct dl_phdr_info* info, size_t size, void* data) {
    (void)size;
    FindCtx* ctx = (FindCtx*)data;
    if (ctx->result) return 1;

    const char* name = info->dlpi_name;
    if (!name || !strstr(name, ctx->libname)) return 0;

    uintptr_t base = info->dlpi_addr;
    Elf64_Ehdr* ehdr = (Elf64_Ehdr*)base;
    if (ehdr->e_phnum == 0) return 0;

    Elf64_Phdr* phdr = (Elf64_Phdr*)(base + ehdr->e_phoff);
    Elf64_Dyn* dynamic = nullptr;
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type == PT_DYNAMIC) {
            dynamic = (Elf64_Dyn*)(base + phdr[i].p_vaddr);
            break;
        }
    }
    if (!dynamic) return 0;

    Elf64_Sym* symtab = nullptr;
    const char* strtab = nullptr;
    size_t strtab_size = 0;
    for (Elf64_Dyn* d = dynamic; d->d_tag != DT_NULL; d++) {
        if (d->d_tag == DT_SYMTAB) symtab = (Elf64_Sym*)(base + d->d_un.d_ptr);
        if (d->d_tag == DT_STRTAB) strtab = (const char*)(base + d->d_un.d_ptr);
        if (d->d_tag == DT_STRSZ)  strtab_size = d->d_un.d_val;
    }
    if (!symtab || !strtab || !strtab_size) return 0;

    for (size_t i = 0; i < 65536; i++) {
        if (symtab[i].st_name >= strtab_size) break;
        const char* sname = strtab + symtab[i].st_name;
        if (sname[0] == '\0') continue;
        if (ELF64_ST_TYPE(symtab[i].st_info) != STT_FUNC) continue;
        if (symtab[i].st_value == 0) continue;

        int status;
        char* demangled = abi::__cxa_demangle(sname, nullptr, nullptr, &status);
        if (status == 0 && demangled) {
            if (strstr(demangled, ctx->substr)) {
                ctx->result = (void*)(base + symtab[i].st_value);
                if (ctx->demangled_name) {
                    ctx->demangled_name = strdup(demangled);
                }
                fprintf(stderr, "  ELF: found '%s' -> %s @ %p\n", ctx->substr, demangled, ctx->result);
                free(demangled);
                return 1;
            }
            free(demangled);
        }
    }
    return 0;
}

static void* find_symbol_by_demangled(const char* libname, const char* demangled_substr,
                                       char** out_demangled) {
    FindCtx ctx = { libname, demangled_substr, nullptr, nullptr };
    if (out_demangled) ctx.demangled_name = nullptr;
    dl_iterate_phdr(phdr_callback, &ctx);
    if (out_demangled) *out_demangled = ctx.demangled_name;
    return ctx.result;
}

// ═══════════════════════════════════════════════════════════════════
//  加载所有符号 (自动检测 ABI 版本)
// ═══════════════════════════════════════════════════════════════════
static bool load_symbols() {
    g_libgui = dlopen("libgui.so", RTLD_NOW);
    if (!g_libgui) { fprintf(stderr, "DLOPEN: libgui.so not found\n"); return false; }

    g_libutils = dlopen("libutils.so", RTLD_NOW);
    if (!g_libutils) { fprintf(stderr, "DLOPEN: libutils.so not found\n"); return false; }

    p_String8_ctor = (String8_ctor_t)find_symbol_by_demangled("libutils.so", "String8::String8", nullptr);
    if (!p_String8_ctor) { fprintf(stderr, "DLOPEN: String8 ctor not found\n"); return false; }

    p_SCC_ctor = (SCC_ctor_t)find_symbol_by_demangled("libgui.so", "SurfaceComposerClient::SurfaceComposerClient", nullptr);
    if (!p_SCC_ctor) { fprintf(stderr, "DLOPEN: SCC ctor not found\n"); return false; }

    // createSurface — 检测 ABI 版本
    char* demangled_cs = nullptr;
    p_SCC_createSurface_raw = find_symbol_by_demangled("libgui.so", "SurfaceComposerClient::createSurface", &demangled_cs);
    if (!p_SCC_createSurface_raw) {
        p_SCC_createSurface_raw = find_symbol_by_demangled("libgui.so", "createWithSurfaceParent", &demangled_cs);
    }
    if (!p_SCC_createSurface_raw) { fprintf(stderr, "DLOPEN: createSurface not found\n"); return false; }

    // 根据 demangled 名判断是 v6 (旧) 还是 v8 (新, 含 LayerMetadata/IBinder)
    if (demangled_cs) {
        if (strstr(demangled_cs, "LayerMetadata") || strstr(demangled_cs, "IBinder")) {
            g_createSurface_version = 8;
            fprintf(stderr, "DLOPEN: createSurface API v8 (Android 14+)\n");
        } else {
            g_createSurface_version = 6;
            fprintf(stderr, "DLOPEN: createSurface API v6 (Android <14)\n");
        }
        free(demangled_cs);
    } else {
        g_createSurface_version = 6;
    }

    p_SC_getSurface = (SC_getSurface_t)find_symbol_by_demangled("libgui.so", "SurfaceControl::getSurface", nullptr);
    if (!p_SC_getSurface) { fprintf(stderr, "DLOPEN: getSurface not found\n"); return false; }

    p_Transaction_ctor = (Transaction_ctor_t)find_symbol_by_demangled("libgui.so", "Transaction::Transaction", nullptr);
    p_Transaction_setLayer = (Transaction_setLayer_t)find_symbol_by_demangled("libgui.so", "Transaction::setLayer", nullptr);
    p_Transaction_show = (Transaction_show_t)find_symbol_by_demangled("libgui.so", "Transaction::show", nullptr);
    p_Transaction_apply = (Transaction_apply_t)find_symbol_by_demangled("libgui.so", "Transaction::apply", nullptr);

    fprintf(stderr, "DLOPEN: all symbols loaded (createSurface v%d)\n", g_createSurface_version);
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

    // ─── 1. SurfaceComposerClient ────────────────────────────────
    ov->scc = malloc(SCC_BUF_SIZE);
    memset(ov->scc, 0, SCC_BUF_SIZE);
    p_SCC_ctor(ov->scc);
    fprintf(stderr, "SurfaceComposerClient created\n");

    // ─── 2. String8 ──────────────────────────────────────────────
    char nameBuf[STRING8_BUF_SIZE];
    memset(nameBuf, 0, sizeof(nameBuf));
    p_String8_ctor(nameBuf, "yolov8touch");

    // ─── 3. createSurface (ABI 自适应) ────────────────────────────
    void* sc = nullptr;
    if (g_createSurface_version == 8) {
        // Android 14+: 需要 LayerMetadata 缓冲区 + parentHandle + outId
        void* metadataBuf = calloc(1, LAYER_METADATA_SIZE);
        uint32_t outId = 0;
        createSurface_v8_t fn = (createSurface_v8_t)p_SCC_createSurface_raw;
        sc = fn(ov->scc, nameBuf, (uint32_t)w, (uint32_t)h, 1, 0, nullptr, metadataBuf, &outId);
        free(metadataBuf);
        fprintf(stderr, "createSurface(v8) -> %p, outId=%u\n", sc, outId);
    } else {
        // Android <14: 旧 API
        createSurface_v6_t fn = (createSurface_v6_t)p_SCC_createSurface_raw;
        sc = fn(ov->scc, nameBuf, (uint32_t)w, (uint32_t)h, 1, 0);
        fprintf(stderr, "createSurface(v6) -> %p\n", sc);
    }

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

    // ─── 5. Transaction ──────────────────────────────────────────
    if (p_Transaction_ctor && p_Transaction_setLayer && p_Transaction_show && p_Transaction_apply) {
        char txBuf[TRANSACTION_BUF_SIZE];
        memset(txBuf, 0, sizeof(txBuf));
        p_Transaction_ctor(txBuf);
        p_Transaction_setLayer(txBuf, &ov->surfaceControl, 0x7fffffff);
        p_Transaction_show(txBuf, &ov->surfaceControl);
        int ret = p_Transaction_apply(txBuf);
        fprintf(stderr, "Transaction applied: ret=%d\n", ret);
    }

    // ─── 6. EGL 初始化 ───────────────────────────────────────────
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
        EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 24, EGL_NONE
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

    ANativeWindow_setBuffersGeometry((ANativeWindow*)ov->surface, w, h, 1);
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