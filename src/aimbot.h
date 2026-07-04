#pragma once
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <csignal>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <thread>
#include <mutex>
#include <vector>
#include <ncnn/net.h>

// ─── stb_image ───────────────────────────────────────────────────
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#include "stb_image.h"

// ─── 配置 ────────────────────────────────────────────────────────
struct AimbotConfig {
    char modelPath[256] = "/data/local/tmp/yolov8.param";
    int screenW = 1080;
    int screenH = 2400;
    int inputW = 256;
    int inputH = 256;
    int rangeRadius = 300;
    float confThresh = 0.25f;
    float nmsThresh = 0.45f;
    float kp = 0.30f;
    float ki = 0.02f;
    float kd = 0.08f;
    float smooth = 0.35f;
    float maxMovePerFrame = 1200.0f;
    bool enabled = false;
    bool fireEnabled = false;
};

struct Detection {
    float x1, y1, x2, y2, score;
    int classId;
};

struct PIDState {
    float integralX = 0, integralY = 0;
    float prevErrorX = 0, prevErrorY = 0;
    float lastMoveX = 0, lastMoveY = 0;
    float centerX = 0, centerY = 0;
    bool pointerDown = false;
    int deadzoneFrames = 0;
};

// ─── 触摸常量 ────────────────────────────────────────────────────
#define TOUCH_VIRTUAL_SLOT  8
#define TOUCH_TRIGGER_SLOT  9
#define TOUCH_VIRTUAL_ID    1000
#define TOUCH_TRIGGER_ID    2000

// ─── 全局状态 ────────────────────────────────────────────────────
extern volatile int g_running;
extern AimbotConfig g_cfg;
extern std::mutex g_detections_mutex;
extern std::vector<Detection> g_detections;
extern float g_aimFPS;
extern float g_detectFPS;
extern int g_frameCount;
extern long long g_lastFrameTime;

// ─── API ─────────────────────────────────────────────────────────
bool aimbot_init();
void aimbot_shutdown();
void aimbot_loop_iteration();  // 单帧迭代
void aimbot_toggle_enabled();
void aimbot_toggle_fire();
void aimbot_set_fire(bool down);

// 触摸注入
bool touch_init_aimbot(int screenW, int screenH);
void touch_close_aimbot();
void touch_down(int slot, int id, int x, int y);
void touch_move(int slot, int x, int y);
void touch_up(int slot);