#include "overlay.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

// 尝试多个可能的 fb 路径
static const char* FB_PATHS[] = {
    "/dev/graphics/fb0",
    "/dev/fb0",
    nullptr
};

bool overlay_init(OverlayWindow* ov, int w, int h) {
    memset(ov, 0, sizeof(*ov));
    ov->width = w; ov->height = h;
    ov->display = EGL_NO_DISPLAY;
    ov->eglSurface = EGL_NO_SURFACE;
    ov->context = EGL_NO_CONTEXT;

    // ─── 1. 打开 framebuffer ────────────────────────────────────
    for (int i = 0; FB_PATHS[i]; i++) {
        ov->fb_fd = open(FB_PATHS[i], O_RDWR);
        if (ov->fb_fd >= 0) {
            fprintf(stderr, "FB: opened %s\n", FB_PATHS[i]);
            break;
        }
    }
    if (ov->fb_fd < 0) {
        fprintf(stderr, "ERR: cannot open framebuffer device\n");
        return false;
    }

    // ─── 2. 获取屏幕信息 ────────────────────────────────────────
    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
    if (ioctl(ov->fb_fd, FBIOGET_VSCREENINFO, &vinfo) < 0) {
        fprintf(stderr, "ERR: FBIOGET_VSCREENINFO failed\n");
        close(ov->fb_fd); ov->fb_fd = -1;
        return false;
    }
    if (ioctl(ov->fb_fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
        fprintf(stderr, "ERR: FBIOGET_FSCREENINFO failed\n");
        close(ov->fb_fd); ov->fb_fd = -1;
        return false;
    }

    ov->fb_stride = finfo.line_length;
    ov->fb_size = finfo.smem_len;

    fprintf(stderr, "FB: %dx%d, bpp=%d, stride=%d, size=%zu\n",
            vinfo.xres, vinfo.yres, vinfo.bits_per_pixel, ov->fb_stride, ov->fb_size);

    // 使用 framebuffer 的实际分辨率
    ov->width = vinfo.xres;
    ov->height = vinfo.yres;

    // ─── 3. mmap framebuffer ────────────────────────────────────
    ov->fb_mmap = mmap(nullptr, ov->fb_size, PROT_READ | PROT_WRITE,
                       MAP_SHARED, ov->fb_fd, 0);
    if (ov->fb_mmap == MAP_FAILED) {
        fprintf(stderr, "ERR: mmap framebuffer failed\n");
        close(ov->fb_fd); ov->fb_fd = -1;
        return false;
    }

    // ─── 4. EGL 初始化 (pbuffer 离屏渲染) ──────────────────────
    ov->display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (ov->display == EGL_NO_DISPLAY) {
        fprintf(stderr, "ERR: eglGetDisplay\n"); return false;
    }

    EGLint major, minor;
    if (!eglInitialize(ov->display, &major, &minor)) {
        fprintf(stderr, "ERR: eglInitialize\n"); return false;
    }

    const EGLint cfgAttribs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24,
        EGL_NONE
    };
    EGLConfig cfg; EGLint numCfg;
    if (!eglChooseConfig(ov->display, cfgAttribs, &cfg, 1, &numCfg) || numCfg == 0) {
        fprintf(stderr, "ERR: eglChooseConfig (pbuffer)\n"); return false;
    }

    const EGLint ctxAttribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    ov->context = eglCreateContext(ov->display, cfg, EGL_NO_CONTEXT, ctxAttribs);
    if (ov->context == EGL_NO_CONTEXT) {
        fprintf(stderr, "ERR: eglCreateContext\n"); return false;
    }

    const EGLint pbAttribs[] = {
        EGL_WIDTH, ov->width,
        EGL_HEIGHT, ov->height,
        EGL_NONE
    };
    ov->eglSurface = eglCreatePbufferSurface(ov->display, cfg, pbAttribs);
    if (ov->eglSurface == EGL_NO_SURFACE) {
        fprintf(stderr, "ERR: eglCreatePbufferSurface\n"); return false;
    }

    if (!eglMakeCurrent(ov->display, ov->eglSurface, ov->eglSurface, ov->context)) {
        fprintf(stderr, "ERR: eglMakeCurrent\n"); return false;
    }

    glViewport(0, 0, ov->width, ov->height);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    fprintf(stderr, "Overlay created: %dx%d (fb), EGL %d.%d\n", ov->width, ov->height, major, minor);
    return true;
}

void overlay_destroy(OverlayWindow* ov) {
    if (ov->display != EGL_NO_DISPLAY) {
        eglMakeCurrent(ov->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (ov->eglSurface != EGL_NO_SURFACE) eglDestroySurface(ov->display, ov->eglSurface);
        if (ov->context != EGL_NO_CONTEXT) eglDestroyContext(ov->display, ov->context);
        eglTerminate(ov->display);
    }
    if (ov->fb_mmap && ov->fb_mmap != MAP_FAILED) {
        munmap(ov->fb_mmap, ov->fb_size);
    }
    if (ov->fb_fd >= 0) {
        close(ov->fb_fd);
    }
    memset(ov, 0, sizeof(*ov));
}

void overlay_swap(OverlayWindow* ov) {
    // 1. 完成 GL 渲染到 pbuffer
    glFlush();
    glFinish();

    // 2. 从 pbuffer 读回像素
    //    分配临时缓冲区 (RGBA → 目标格式)
    size_t rowBytes = ov->width * 4;
    uint8_t* pixels = (uint8_t*)malloc(rowBytes * ov->height);
    if (!pixels) return;

    glReadPixels(0, 0, ov->width, ov->height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    // 3. 复制到 framebuffer (行翻转：GL 原点在左下，fb 在左上)
    uint8_t* fb = (uint8_t*)ov->fb_mmap;
    for (int y = 0; y < ov->height; y++) {
        int srcY = ov->height - 1 - y;
        uint8_t* srcRow = pixels + srcY * rowBytes;
        uint8_t* dstRow = fb + y * ov->fb_stride;

        // 逐像素复制，处理 32bpp (BGRA/RGBA)
        for (int x = 0; x < ov->width; x++) {
            int sx = x * 4;
            int dx = x * 4;
            if (dx + 4 > ov->fb_stride) break;

            // 大多数 Android framebuffer 是 BGRA 格式
            // 将 RGBA 转为 BGRA
            dstRow[dx + 0] = srcRow[sx + 2];  // B
            dstRow[dx + 1] = srcRow[sx + 1];  // G
            dstRow[dx + 2] = srcRow[sx + 0];  // R
            dstRow[dx + 3] = srcRow[sx + 3];  // A
        }
    }

    free(pixels);
}

void overlay_clear(OverlayWindow* ov, float r, float g, float b, float a) {
    (void)ov;
    glClearColor(r, g, b, a);
    glClear(GL_COLOR_BUFFER_BIT);
}