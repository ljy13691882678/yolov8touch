/*
 * yolov8touch — Root shell AI 自瞄 + ImGui 可视化 UI
 *
 * 运行方式:
 *   su -c /data/adb/yolov8touch
 *   (无需 LD_LIBRARY_PATH，libtensorflowlite_jni.so 放在同目录即可)
 *   (无需 --model，自动搜索同目录和常用路径的 .tflite 模型)
 *
 * 原理:
 *   1. screencap 截屏 → PNG 解码 → RGBA
 *   2. TFLite YOLOv8 推理 → 检测目标框
 *   3. 选最近目标 → PID 控制 → uinput 触摸注入
 *   4. ImGui 渲染透明悬浮窗 → 显示检测框 + 设置面板
 *   5. 60fps 主循环，Ctrl+C 退出
 *
 * 依赖:
 *   - libtensorflowlite_jni.so (放同目录，RPATH=$ORIGIN 自动加载)
 *   - libgui, libui, libbinder (SurfaceComposer 悬浮窗)
 *   - libEGL, libGLESv3 (OpenGL ES 3.0)
 *   - libandroid, liblog (Android 系统库)
 */

#include "aimbot.h"
#include "overlay.h"
#include "gui.h"
#include <thread>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <setjmp.h>
#include <unistd.h>

static OverlayWindow g_overlay;
static std::thread* g_aimbot_thread = nullptr;

// ─── SIGBUS 恢复：overlay_init 可能因 SurfaceFlinger 权限崩溃 ─────
static sigjmp_buf g_sigbus_jmp;
static volatile sig_atomic_t g_sigbus_caught = 0;

static void sigbus_handler(int sig) {
    g_sigbus_caught = 1;
    siglongjmp(g_sigbus_jmp, 1);
}

// ─── 信号处理 ───────────────────────────────────────────────────
static void sig_handler(int) {
    g_running = 0;
}

// 安全的 overlay 初始化，捕获 SIGBUS/SEGV
static bool safe_overlay_init(int w, int h) {
    struct sigaction oldBus, oldSegv;
    struct sigaction sa = {};
    sa.sa_handler = sigbus_handler;
    sa.sa_flags = SA_RESETHAND;  // 只捕获一次
    sigaction(SIGBUS, &sa, &oldBus);
    sigaction(SIGSEGV, &sa, &oldSegv);

    bool ok = false;
    if (sigsetjmp(g_sigbus_jmp, 1) == 0) {
        ok = overlay_init(&g_overlay, w, h);
    } else {
        fprintf(stderr, "WARN: overlay_init crashed (signal %d) — running headless\n",
                g_sigbus_caught ? SIGBUS : SIGSEGV);
        ok = false;
    }

    // 恢复原始信号处理器
    sigaction(SIGBUS, &oldBus, nullptr);
    sigaction(SIGSEGV, &oldSegv, nullptr);
    return ok;
}

// ─── 帮助信息 ───────────────────────────────────────────────────
static void print_usage(const char* prog) {
    fprintf(stderr,
        "YOLOv8 Touch Aimbot with ImGui Overlay\n"
        "Usage: %s [OPTIONS]\n\n"
        "Options:\n"
        "  --model PATH      TFLite model path (auto-detect if not set)\n"
        "  --width W          Screen width (auto-detect from screencap)\n"
        "  --height H         Screen height (auto-detect from screencap)\n"
        "  --range R          Detection range radius px (default: 300)\n"
        "  --conf C           Confidence threshold (default: 0.25)\n"
        "  --help             Show this help\n\n"
        "Example:\n"
        "  su -c %s\n\n"
        "Deploy:\n"
        "  Put yolov8touch + libtensorflowlite_jni.so together in /data/adb/\n"
        "  Put .tflite model in same dir or /data/local/tmp/\n"
        "  Just run, no extra env vars needed!\n\n"
        "Controls:\n"
        "  START/STOP button  Toggle AI aiming\n"
        "  FIRE ON/OFF button Toggle auto fire\n"
        "  Control Panel      Adjust PID, range, confidence\n"
        "  Ctrl+C             Exit\n",
        prog, prog);
}

// ─── 解析参数 ───────────────────────────────────────────────────
static bool g_modelArgProvided = false;

static void parse_args(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            print_usage(argv[0]); exit(0);
        } else if (!strcmp(argv[i], "--model") && i + 1 < argc) {
            strncpy(g_cfg.modelPath, argv[++i], sizeof(g_cfg.modelPath) - 1);
            g_modelArgProvided = true;
        } else if (!strcmp(argv[i], "--width") && i + 1 < argc) {
            g_cfg.screenW = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--height") && i + 1 < argc) {
            g_cfg.screenH = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--range") && i + 1 < argc) {
            g_cfg.rangeRadius = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--conf") && i + 1 < argc) {
            g_cfg.confThresh = atof(argv[++i]);
        }
    }
}

// ─── 主函数 ─────────────────────────────────────────────────────
int main(int argc, char** argv) {
    parse_args(argc, argv);
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    fprintf(stderr, "========================================\n");
    fprintf(stderr, "  YOLOv8 Touch Aimbot v1.0\n");
    fprintf(stderr, "  Screen: %dx%d | Range: %dpx\n", g_cfg.screenW, g_cfg.screenH, g_cfg.rangeRadius);

    // 如果没有通过 --model 指定，自动搜索同目录/常用路径的 .tflite
    if (!g_modelArgProvided) {
        fprintf(stderr, "  Model: auto-detecting...\n");
        aimbot_auto_detect_model();
    }
    fprintf(stderr, "  Model: %s\n", g_cfg.modelPath);
    fprintf(stderr, "========================================\n");

    // 1. 初始化 AI 自瞄
    if (!aimbot_init()) {
        fprintf(stderr, "FATAL: aimbot init failed\n");
        return 1;
    }

    // 2. 创建透明悬浮窗 (ImGui 渲染) — 带 SIGBUS 保护
    if (!safe_overlay_init(g_cfg.screenW, g_cfg.screenH)) {
        fprintf(stderr, "WARN: overlay init failed — running headless\n");
    }

    // 3. 初始化 ImGui
    if (g_overlay.display != EGL_NO_DISPLAY) {
        gui_init(&g_overlay);
    }

    // 4. 启动 AI 自瞄后台线程
    std::chrono::steady_clock::time_point ns = std::chrono::steady_clock::now();
    g_aimbot_thread = new std::thread([ns]() mutable {
        while (g_running) {
            aimbot_loop_iteration();
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - ns).count();
            long long target = 16667; // 60fps
            if (elapsed < target) {
                std::this_thread::sleep_for(std::chrono::microseconds(target - elapsed));
            }
            ns = std::chrono::steady_clock::now();
        }
    });

    // 5. 主循环 (渲染 UI)
    while (g_running) {
        if (g_overlay.display != EGL_NO_DISPLAY) {
            overlay_clear(&g_overlay, 0.0f, 0.0f, 0.0f, 0.0f);
            gui_new_frame();
            gui_render();
            overlay_swap(&g_overlay);
        } else {
            // 无 UI 模式，等待退出
            usleep(100000);
        }
    }

    // 6. 清理
    fprintf(stderr, "\nShutting down...\n");
    if (g_aimbot_thread) {
        g_aimbot_thread->join();
        delete g_aimbot_thread;
    }
    gui_shutdown();
    overlay_destroy(&g_overlay);
    aimbot_shutdown();
    fprintf(stderr, "Done.\n");
    return 0;
}