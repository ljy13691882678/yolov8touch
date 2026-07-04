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
typedef void* (*SCC_createSurface_t)(void* self, void* nameObj, uint32_t w, uint32_t h, int32_t format, uint32_t flags);
typedef void* (*SC_getSurface_t)(void* sc);
typedef void  (*Transaction_ctor_t)(void* self);
typedef void* (*Transaction_setLayer_t)(void* self, void* sc, int32_t layer);
typedef void* (*Transaction_show_t)(void* self, void* sc);
typedef int   (*Transaction_apply_t)(void* self);
typedef void  (*String8_ctor_t)(void* self, const char* s);

static void* g_libgui = nullptr;
static void* g_libutils = nullptr;

static SCC_ctor_t            p_SCC_ctor = nullptr;
static SCC_createSurface_t   p_SCC_createSurface = nullptr;
static SC_getSurface_t       p_SC_getSurface = nullptr;
static Transaction_ctor_t    p_Transaction_ctor = nullptr;
static Transaction_setLayer_t p_Transaction_setLayer = nullptr;
static Transaction_show_t    p_Transaction_show = nullptr;
static Transaction_apply_t   p_Transaction_apply = nullptr;
static String8_ctor_t        p_String8_ctor = nullptr;

#define SCC_BUF_SIZE        1024
#define STRING8_BUF_SIZE    64
#define TRANSACTION_BUF_SIZE 512

// ═══════════════════════════════════════════════════════════════════
//  ELF 符号表扫描 — 通过 demangled name 子串匹配
// ═══════════════════════════════════════════════════════════════════

// 遍历 .dynsym 表，用 demangled name 子串匹配找符号
// lib: dlopen 返回的句柄
// demangled_substr: 要匹配的 demangled 名子串 (如 "createSurface")
// 返回: 找到的符号地址，未找到返回 nullptr
static void* find_symbol_by_demangled(void* lib, const char* demangled_substr) {
    struct link_map* map = nullptr;
    if (dlinfo(lib, RTLD_DI_LINKMAP, &map) != 0 || !map) {
        fprintf(stderr, "  ELF: dlinfo(RTLD_DI_LINKMAP) failed\n");
        return nullptr;
    }

    uintptr_t base = map->l_addr;
    Elf64_Ehdr* ehdr = (Elf64_Ehdr*)base;
    if (ehdr->e_phnum == 0 || ehdr->e_phoff == 0) return nullptr;

    // 找 PT_DYNAMIC 段
    Elf64_Phdr* phdr = (Elf64_Phdr*)(base + ehdr->e_phoff);
    Elf64_Dyn* dynamic = nullptr;
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type == PT_DYNAMIC) {
            dynamic = (Elf64_Dyn*)(base + phdr[i].p_vaddr);
            break;
        }
    }
    if (!dynamic) return nullptr;

    // 遍历 .dynamic 找 SYMTAB / STRTAB
    Elf64_Sym* symtab = nullptr;
    const char* strtab = nullptr;
    size_t symcount = 0;
    for (Elf64_Dyn* d = dynamic; d->d_tag != DT_NULL; d++) {
        if (d->d_tag == DT_SYMTAB) symtab = (Elf64_Sym*)(base + d->d_un.d_ptr);
        if (d->d_tag == DT_STRTAB) strtab = (const char*)(base + d->d_un.d_ptr);
        if (d->d_tag == DT_GNU_HASH) {
            // 粗略估算符号数量 (不求精确，遍历时遇到边界就停)
            symcount = 50000;
        }
    }
    if (!symtab || !strtab) return nullptr;

    // 没有精确的 symcount 就用一个安全的非精确上限
    // 遍历到第一个 st_name 越界或遇到空值反复出现为止
    size_t strtab_size = 0;
    // 粗略获取 strtab 大小 (通过 DT_STRSZ)
    for (Elf64_Dyn* d = dynamic; d->d_tag != DT_NULL; d++) {
        if (d->d_tag == DT_STRSZ) { strtab_size = d->d_un.d_val; break; }
    }

    // 遍历符号表
    for (size_t i = 0; i < 65536; i++) {
        if (symtab[i].st_name >= strtab_size) break;  // 越界
        const char* name = strtab + symtab[i].st_name;
        if (name[0] == '\0') continue;
        if (ELF64_ST_TYPE(symtab[i].st_info) != STT_FUNC) continue;
        if (symtab[i].st_value == 0) continue;

        int status;
        char* demangled = abi::__cxa_demangle(name, nullptr, nullptr, &status);
        if (status == 0 && demangled) {
            if (strstr(demangled, demangled_substr)) {
                void* addr = (void*)(base + symtab[i].st_value);
                fprintf(stderr, "  ELF: found '%s' -> %s @ %p\n", demangled_substr, demangled, addr);
                free(demangled);
                return addr;
            }
            free(demangled);
        }
    }
    return nullptr;
}

// ═══════════════════════════════════════════════════════════════════
//  加载所有符号
// ═══════════════════════════════════════════════════════════════════
static bool load_symbols() {
    g_libgui = dlopen("libgui.so", RTLD_NOW);
    if (!g_libgui) { fprintf(stderr, "DLOPEN: libgui.so not found\n"); return false; }

    g_libutils = dlopen("libutils.so", RTLD_NOW);
    if (!g_libutils) { fprintf(stderr, "DLOPEN: libutils.so not found\n"); return false; }

    // String8 构造函数
    p_String8_ctor = (String8_ctor_t)find_symbol_by_demangled(g_libutils, "String8::String8");
    if (!p_String8_ctor) { fprintf(stderr, "DLOPEN: String8 ctor not found\n"); return false; }

    // SurfaceComposerClient 构造函数
    p_SCC_ctor = (SCC_ctor_t)find_symbol_by_demangled(g_libgui, "SurfaceComposerClient::SurfaceComposerClient");
    if (!p_SCC_ctor) { fprintf(stderr, "DLOPEN: SCC ctor not found\n"); return false; }

    // createSurface (可能有多个重载，取第一个匹配)
    p_SCC_createSurface = (SCC_createSurface_t)find_symbol_by_demangled(g_libgui, "SurfaceComposerClient::createSurface");
    if (!p_SCC_createSurface) {
        // 某些 Android 版本改名了
        p_SCC_createSurface = (SCC_createSurface_t)find_symbol_by_demangled(g_libgui, "SurfaceComposerClient::createWithSurfaceParent");
    }
    if (!p_SCC_createSurface) { fprintf(stderr, "DLOPEN: createSurface not found\n"); return false; }

    // SurfaceControl::getSurface
    p_SC_getSurface = (SC_getSurface_t)find_symbol_by_demangled(g_libgui, "SurfaceControl::getSurface");
    if (!p_SC_getSurface) { fprintf(stderr, "DLOPEN: getSurface not found\n"); return false; }

    // Transaction (optional, no error)
    p_Transaction_ctor = (Transaction_ctor_t)find_symbol_by_demangled(g_libgui, "SurfaceComposerClient::Transaction::Transaction");
    p_Transaction_setLayer = (Transaction_setLayer_t)find_symbol_by_demangled(g_libgui, "Transaction::setLayer");
    p_Transaction_show = (Transaction_show_t)find_symbol_by_demangled(g_libgui, "Transaction::show");
    p_Transaction_apply = (Transaction_apply_t)find_symbol_by_demangled(g_libgui, "Transaction::apply");

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

    // ─── 1. SurfaceComposerClient ────────────────────────────────
    ov->scc = malloc(SCC_BUF_SIZE);
    memset(ov->scc, 0, SCC_BUF_SIZE);
    p_SCC_ctor(ov->scc);
    fprintf(stderr, "SurfaceComposerClient created\n");

    // ─── 2. String8 ──────────────────────────────────────────────
    char nameBuf[STRING8_BUF_SIZE];
    memset(nameBuf, 0, sizeof(nameBuf));
    p_String8_ctor(nameBuf, "yolov8touch");

    // ─── 3. createSurface ────────────────────────────────────────
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