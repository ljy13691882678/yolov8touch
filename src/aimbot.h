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
#include <memory>
#include <algorithm>

// ─── TFLite C API ────────────────────────────────────────────────
#include "tensorflow/lite/c/c_api.h"
#include "tensorflow/lite/c/common.h"

// ─── stb_image ───────────────────────────────────────────────────
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#include "stb_image.h"

// ─── 配置 ────────────────────────────────────────────────────────
struct AimbotConfig {
    char modelPath[256] = "/data/local/tmp/yolov8n_float_256.tflite";
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

// ─── 工具 ─────────────────────────────────────────────────────────
inline long long getTimeUs() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return tv.tv_sec * 1000000LL + tv.tv_usec;
}

inline float clampValue(float value, float lo, float hi) {
    return std::max(lo, std::min(value, hi));
}

inline std::vector<Detection> nms(std::vector<Detection>& boxes, float iouThreshold) {
    if (boxes.empty()) return {};
    std::sort(boxes.begin(), boxes.end(),
        [](const Detection& a, const Detection& b) { return a.score > b.score; });
    auto suppressed = std::make_unique<uint8_t[]>(boxes.size());
    memset(suppressed.get(), 0, boxes.size());
    std::vector<Detection> result;
    result.reserve(boxes.size());
    for (size_t i = 0; i < boxes.size(); ++i) {
        if (suppressed[i]) continue;
        result.push_back(boxes[i]);
        for (size_t j = i + 1; j < boxes.size(); ++j) {
            if (suppressed[j]) continue;
            float x1a = boxes[i].x1, y1a = boxes[i].y1;
            float x2a = boxes[i].x2, y2a = boxes[i].y2;
            float x1b = boxes[j].x1, y1b = boxes[j].y1;
            float x2b = boxes[j].x2, y2b = boxes[j].y2;
            float interX1 = std::max(x1a, x1b);
            float interY1 = std::max(y1a, y1b);
            float interX2 = std::min(x2a, x2b);
            float interY2 = std::min(y2a, y2b);
            float interW = interX2 - interX1;
            float interH = interY2 - interY1;
            if (interW <= 0 || interH <= 0) continue;
            float interArea = interW * interH;
            float areaA = (x2a - x1a) * (y2a - y1a);
            float areaB = (x2b - x1b) * (y2b - y1b);
            float iou = interArea / (areaA + areaB - interArea);
            if (iou > iouThreshold) suppressed[j] = 1;
        }
    }
    return result;
}