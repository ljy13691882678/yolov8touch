#define STB_IMAGE_IMPLEMENTATION
#include "aimbot.h"
#include <linux/uinput.h>
#include <array>
#include <chrono>
#include <cstdarg>

// ─── 全局变量 ────────────────────────────────────────────────────
volatile int g_running = 0;
AimbotConfig g_cfg;
std::mutex g_detections_mutex;
std::vector<Detection> g_detections;
float g_aimFPS = 0;
float g_detectFPS = 0;
int g_frameCount = 0;
long long g_lastFrameTime = 0;

// ─── TFLite 推理状态 ─────────────────────────────────────────────
static TfLiteModel*      g_model = nullptr;
static TfLiteInterpreter* g_interpreter = nullptr;
static int g_model_input_w = 256;
static int g_model_input_h = 256;
static TfLiteType g_input_type = kTfLiteFloat32;
static float g_input_scale = 1.0f;
static int g_input_zero_point = 0;
static int g_num_classes = 1;

// ─── uinput ──────────────────────────────────────────────────────
static int g_uinput_fd = -1;

// ─── PID 状态 ────────────────────────────────────────────────────
static PIDState g_pid;

// ─── 日志 ─────────────────────────────────────────────────────────
#define LOG_TAG "yolov8touch"
static void logd(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "[%s] ", LOG_TAG);
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
}
static void loge(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "[%s] ERROR: ", LOG_TAG);
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
}

// ═══════════════════════════════════════════════════════════════════
//  TFLite 模型加载
// ═══════════════════════════════════════════════════════════════════
bool aimbot_init() {
    logd("Loading TFLite model: %s", g_cfg.modelPath);

    g_model = TfLiteModelCreateFromFile(g_cfg.modelPath);
    if (!g_model) {
        loge("Failed to load model: %s", g_cfg.modelPath);
        return false;
    }

    TfLiteInterpreterOptions* opts = TfLiteInterpreterOptionsCreate();
    TfLiteInterpreterOptionsSetNumThreads(opts, 4);
    g_interpreter = TfLiteInterpreterCreate(g_model, opts);
    TfLiteInterpreterOptionsDelete(opts);

    if (!g_interpreter) {
        loge("Failed to create interpreter");
        TfLiteModelDelete(g_model);
        g_model = nullptr;
        return false;
    }

    if (TfLiteInterpreterAllocateTensors(g_interpreter) != kTfLiteOk) {
        loge("Failed to allocate tensors");
        TfLiteInterpreterDelete(g_interpreter);
        TfLiteModelDelete(g_model);
        g_interpreter = nullptr;
        g_model = nullptr;
        return false;
    }

    // 读取输入 tensor 信息
    const TfLiteTensor* input = TfLiteInterpreterGetInputTensor(g_interpreter, 0);
    if (input) {
        int ndim = TfLiteTensorNumDims(input);
        g_model_input_h = TfLiteTensorDim(input, 1);
        g_model_input_w = TfLiteTensorDim(input, 2);
        g_input_type = TfLiteTensorType(input);
        TfLiteQuantizationParams q = TfLiteTensorQuantizationParams(input);
        g_input_scale = q.scale;
        g_input_zero_point = q.zero_point;
        g_cfg.inputW = g_model_input_w;
        g_cfg.inputH = g_model_input_h;
        logd("Model input: %dx%d, type=%d, scale=%.4f, zp=%d",
             g_model_input_w, g_model_input_h, (int)g_input_type, g_input_scale, g_input_zero_point);
    }

    // 读取输出 tensor 信息
    int outCount = TfLiteInterpreterGetOutputTensorCount(g_interpreter);
    logd("Model outputs: %d", outCount);
    if (outCount > 0) {
        const TfLiteTensor* out = TfLiteInterpreterGetOutputTensor(g_interpreter, 0);
        if (out) {
            int ndim = TfLiteTensorNumDims(out);
            int channels = ndim >= 2 ? TfLiteTensorDim(out, ndim - 1) : 0;
            g_num_classes = channels - 4;
            if (g_num_classes < 1) g_num_classes = 1;
            logd("Output: dims=%d, channels=%d, classes=%d", ndim, channels, g_num_classes);
        }
    }

    logd("TFLite model loaded successfully");
    return true;
}

void aimbot_shutdown() {
    if (g_interpreter) { TfLiteInterpreterDelete(g_interpreter); g_interpreter = nullptr; }
    if (g_model) { TfLiteModelDelete(g_model); g_model = nullptr; }
}

// ═══════════════════════════════════════════════════════════════════
//  屏幕截图 (screencap PNG → stb_image 解码)
// ═══════════════════════════════════════════════════════════════════
static bool capture_screen_png(uint8_t*& outBuf, int& outW, int& outH, int& outCh) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "screencap -p /data/local/tmp/_aimcap.png 2>/dev/null");

    int ret = system(cmd);
    if (ret != 0) {
        loge("screencap failed");
        return false;
    }

    FILE* f = fopen("/data/local/tmp/_aimcap.png", "rb");
    if (!f) { loge("open png failed"); return false; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> pngBuf(sz);
    fread(pngBuf.data(), 1, sz, f);
    fclose(f);

    int w, h, c;
    unsigned char* img = stbi_load_from_memory(pngBuf.data(), (int)sz, &w, &h, &c, 4);
    if (!img) { loge("stbi_load failed"); return false; }

    outBuf = img;
    outW = w;
    outH = h;
    outCh = 4;
    return true;
}

// ═══════════════════════════════════════════════════════════════════
//  TFLite 推理 + YOLO 解码
// ═══════════════════════════════════════════════════════════════════

// 安全 sigmoid
static float safeSigmoid(float x) {
    if (x >= 0.0f && x <= 1.0f) return x;
    if (x > -30.0f && x < 30.0f) return 1.0f / (1.0f + std::exp(-x));
    return x > 0.0f ? 1.0f : 0.0f;
}

static bool isValidNumber(float v) {
    return std::isfinite(v) && std::fabs(v) < 100000.0f;
}

// 读取输出 tensor 数据
struct OutputTensorData {
    std::vector<int> shape;
    std::vector<float> values;
};

static bool readOutput(const TfLiteTensor* t, OutputTensorData& out) {
    if (!t) return false;
    int dims = TfLiteTensorNumDims(t);
    out.shape.reserve(dims);
    for (int i = 0; i < dims; ++i) out.shape.push_back(TfLiteTensorDim(t, i));
    while (out.shape.size() > 1 && out.shape[0] == 1) out.shape.erase(out.shape.begin());

    size_t n = TfLiteTensorByteSize(t);
    TfLiteType type = TfLiteTensorType(t);
    if (type == kTfLiteFloat32) {
        const float* data = static_cast<const float*>(TfLiteTensorData(t));
        out.values.assign(data, data + n / sizeof(float));
        return true;
    }

    TfLiteQuantizationParams q = TfLiteTensorQuantizationParams(t);
    if ((type == kTfLiteInt8 || type == kTfLiteUInt8) && q.scale > 0.0f) {
        const uint8_t* data = static_cast<const uint8_t*>(TfLiteTensorData(t));
        out.values.resize(n);
        for (size_t i = 0; i < n; ++i) {
            int raw = type == kTfLiteUInt8 ? data[i] : static_cast<int>(static_cast<int8_t>(data[i]));
            out.values[i] = (raw - q.zero_point) * q.scale;
        }
        return true;
    }
    return false;
}

// 构建 Detection（归一化坐标）
static Detection makeDet(float x1, float y1, float x2, float y2,
                         float score, int classId, int inW, int inH) {
    float invW = 1.0f / (float)g_cfg.screenW;
    float invH = 1.0f / (float)g_cfg.screenH;
    return { x1 * invW, y1 * invH, x2 * invW, y2 * invH, score, classId };
}

// 解码 NMS 格式输出 [boxCount, 6]
static bool decodeNmsOutput(const float* out, int dim0, int dim1,
                            int inW, int inH, std::vector<Detection>& dets) {
    int boxCount = (dim1 >= 6 && dim1 <= 8) ? dim0 : (dim0 >= 6 && dim0 <= 8 ? dim1 : 0);
    if (boxCount <= 0) return false;

    dets.clear();
    dets.reserve(std::min(boxCount, 128));
    for (int i = 0; i < boxCount; ++i) {
        float a0, a1, a2, a3, score, label;
        if (dim1 >= 6 && dim1 <= 8) {
            const float* p = out + (size_t)i * dim1;
            a0 = p[0]; a1 = p[1]; a2 = p[2]; a3 = p[3]; score = p[4]; label = p[5];
            if (dim1 >= 7 && p[6] >= 0.0f && p[6] <= 1.0f && (score < 0.0f || score > 1.0f)) score = p[6];
        } else {
            a0 = out[i]; a1 = out[dim1 + i]; a2 = out[2*dim1 + i]; a3 = out[3*dim1 + i];
            score = out[4*dim1 + i]; label = out[5*dim1 + i];
        }
        if (!isValidNumber(a0) || !isValidNumber(a1) || !isValidNumber(a2) || !isValidNumber(a3) || !isValidNumber(score)) continue;

        float s = score;
        if (s > 1.0f && s <= 100.0f) s *= 0.01f;
        else s = safeSigmoid(s);
        if (s < g_cfg.confThresh || s > 1.0f) continue;

        // 尝试两种 box 格式
        float boxes[2][4] = {
            {a0, a1, a2, a3},
            {a0 - a2*0.5f, a1 - a3*0.5f, a0 + a2*0.5f, a1 + a3*0.5f}
        };
        for (auto& b : boxes) {
            float x1 = clampValue(b[0], 0.0f, (float)(inW - 1));
            float y1 = clampValue(b[1], 0.0f, (float)(inH - 1));
            float x2 = clampValue(b[2], 0.0f, (float)(inW - 1));
            float y2 = clampValue(b[3], 0.0f, (float)(inH - 1));
            if (x2 - x1 >= 2.0f && y2 - y1 >= 2.0f) {
                int cid = std::max(0, std::min(g_num_classes - 1, (int)std::round(label)));
                dets.push_back(makeDet(x1, y1, x2, y2, s, cid, inW, inH));
                break;
            }
        }
    }
    return !dets.empty();
}

// 解码 raw YOLO 输出 [channels, anchors]
static bool decodeRawYolo(const float* out, int dim0, int dim1,
                          int inW, int inH, std::vector<Detection>& dets) {
    bool chFirst = dim0 >= 5 && dim0 <= 512 && dim1 > dim0;
    bool chLast  = dim1 >= 5 && dim1 <= 512 && dim0 > dim1;
    if (!chFirst && !chLast) return false;

    int ch = chFirst ? dim0 : dim1;
    int anchors = chFirst ? dim1 : dim0;
    int cls = ch - 4;
    if (cls <= 0) return false;

    dets.clear();
    dets.reserve(128);
    for (int a = 0; a < anchors; ++a) {
        float cx = chFirst ? out[a] : out[a * ch + 0];
        float cy = chFirst ? out[anchors + a] : out[a * ch + 1];
        float bw = chFirst ? out[2*anchors + a] : out[a * ch + 2];
        float bh = chFirst ? out[3*anchors + a] : out[a * ch + 3];
        if (!isValidNumber(cx) || !isValidNumber(cy) || !isValidNumber(bw) || !isValidNumber(bh)) continue;

        int bestCls = -1;
        float bestScore = 0;
        for (int c = 0; c < cls; ++c) {
            float raw = chFirst ? out[(4+c)*anchors + a] : out[a * ch + (4+c)];
            if (!isValidNumber(raw)) continue;
            float s = safeSigmoid(raw);
            if (s > bestScore) { bestScore = s; bestCls = c; }
        }
        if (bestScore < g_cfg.confThresh || bestScore > 1.0f || bestCls < 0) continue;

        float boxes[2][4] = {
            {cx, cy, bw, bh},
            {cx - bw*0.5f, cy - bh*0.5f, cx + bw*0.5f, cy + bh*0.5f}
        };
        for (auto& b : boxes) {
            float x1 = clampValue(b[0], 0.0f, (float)(inW - 1));
            float y1 = clampValue(b[1], 0.0f, (float)(inH - 1));
            float x2 = clampValue(b[2], 0.0f, (float)(inW - 1));
            float y2 = clampValue(b[3], 0.0f, (float)(inH - 1));
            if (x2 - x1 >= 2.0f && y2 - y1 >= 2.0f) {
                dets.push_back(makeDet(x1, y1, x2, y2, bestScore, bestCls, inW, inH));
                break;
            }
        }
    }
    return !dets.empty();
}

// 自动检测输出格式并解码
static std::vector<Detection> decodeYoloOutput(const OutputTensorData& output) {
    std::vector<Detection> dets;
    if (output.shape.size() < 2 || output.shape.size() > 3) return dets;

    int dim0 = output.shape[0];
    int dim1 = output.shape[1];

    // 尝试 NMS 格式
    if (decodeNmsOutput(output.values.data(), dim0, dim1, g_model_input_w, g_model_input_h, dets))
        return dets;

    // 尝试 raw YOLO 格式
    if (decodeRawYolo(output.values.data(), dim0, dim1, g_model_input_w, g_model_input_h, dets))
        return dets;

    return dets;
}

// ═══════════════════════════════════════════════════════════════════
//  PID 自瞄
// ═══════════════════════════════════════════════════════════════════
static void pid_aim(const std::vector<Detection>& dets, long long frameTime) {
    if (dets.empty() || !g_cfg.enabled) {
        if (g_pid.pointerDown) {
            touch_up(TOUCH_VIRTUAL_SLOT);
            g_pid.pointerDown = false;
        }
        if (g_pid.deadzoneFrames > 0) g_pid.deadzoneFrames--;
        return;
    }

    // 选最高分检测
    const Detection* best = &dets[0];
    for (auto& d : dets) {
        if (d.score > best->score) best = &d;
    }

    float cx = (best->x1 + best->x2) * 0.5f;
    float cy = (best->y1 + best->y2) * 0.5f;
    float targetX = cx * g_cfg.screenW;
    float targetY = cy * g_cfg.screenH;
    float centerX = g_cfg.screenW * 0.5f;
    float centerY = g_cfg.screenH * 0.5f;

    float errorX = targetX - centerX;
    float errorY = targetY - centerY;
    float dist = sqrtf(errorX * errorX + errorY * errorY);

    if (dist > g_cfg.rangeRadius) {
        if (g_pid.pointerDown) {
            touch_up(TOUCH_VIRTUAL_SLOT);
            g_pid.pointerDown = false;
        }
        return;
    }

    // Deadzone
    if (dist < 5.0f) {
        g_pid.deadzoneFrames++;
        if (g_pid.deadzoneFrames > 3 && g_pid.pointerDown) {
            touch_up(TOUCH_VIRTUAL_SLOT);
            g_pid.pointerDown = false;
        }
        return;
    }
    g_pid.deadzoneFrames = 0;

    // PID
    float dt = frameTime / 1000000.0f;
    if (dt <= 0.001f) dt = 0.016f;

    g_pid.integralX += errorX * dt;
    g_pid.integralY += errorY * dt;
    float derivX = (errorX - g_pid.prevErrorX) / dt;
    float derivY = (errorY - g_pid.prevErrorY) / dt;
    g_pid.prevErrorX = errorX;
    g_pid.prevErrorY = errorY;

    float moveX = g_cfg.kp * errorX + g_cfg.ki * g_pid.integralX + g_cfg.kd * derivX;
    float moveY = g_cfg.kp * errorY + g_cfg.ki * g_pid.integralY + g_cfg.kd * derivY;

    // 平滑
    moveX = g_cfg.smooth * moveX + (1.0f - g_cfg.smooth) * g_pid.lastMoveX;
    moveY = g_cfg.smooth * moveY + (1.0f - g_cfg.smooth) * g_pid.lastMoveY;
    g_pid.lastMoveX = moveX;
    g_pid.lastMoveY = moveY;

    // 限制单帧最大移动
    float moveMag = sqrtf(moveX * moveX + moveY * moveY);
    if (moveMag > g_cfg.maxMovePerFrame * dt) {
        float scale = g_cfg.maxMovePerFrame * dt / moveMag;
        moveX *= scale;
        moveY *= scale;
    }

    int screenX = (int)(centerX + moveX);
    int screenY = (int)(centerY + moveY);
    screenX = std::max(0, std::min(g_cfg.screenW - 1, screenX));
    screenY = std::max(0, std::min(g_cfg.screenH - 1, screenY));

    if (!g_pid.pointerDown) {
        touch_down(TOUCH_VIRTUAL_SLOT, TOUCH_VIRTUAL_ID, screenX, screenY);
        g_pid.pointerDown = true;
    } else {
        touch_move(TOUCH_VIRTUAL_SLOT, screenX, screenY);
    }

    g_pid.centerX = (float)screenX;
    g_pid.centerY = (float)screenY;
}

// ═══════════════════════════════════════════════════════════════════
//  单帧迭代
// ═══════════════════════════════════════════════════════════════════
void aimbot_loop_iteration() {
    if (!g_interpreter || !g_running) return;
    long long t0 = getTimeUs();

    // 1. 截屏
    uint8_t* img = nullptr;
    int imgW, imgH, imgCh;
    if (!capture_screen_png(img, imgW, imgH, imgCh)) return;
    g_cfg.screenW = imgW;
    g_cfg.screenH = imgH;
    long long t1 = getTimeUs();

    // 2. 预处理 + 填充输入 tensor
    TfLiteTensor* input = TfLiteInterpreterGetInputTensor(g_interpreter, 0);
    if (!input) { stbi_image_free(img); return; }

    int inW = g_model_input_w;
    int inH = g_model_input_h;
    void* inputData = TfLiteTensorData(input);

    if (g_input_type == kTfLiteFloat32) {
        float* data = static_cast<float*>(inputData);
        for (int y = 0; y < inH; ++y) {
            int srcY = y * imgH / inH;
            for (int x = 0; x < inW; ++x) {
                int srcX = x * imgW / inW;
                int srcIdx = (srcY * imgW + srcX) * 4;
                int idx = (y * inW + x) * 3;
                data[idx + 0] = img[srcIdx + 0] / 255.0f;
                data[idx + 1] = img[srcIdx + 1] / 255.0f;
                data[idx + 2] = img[srcIdx + 2] / 255.0f;
            }
        }
    } else if (g_input_type == kTfLiteInt8) {
        int8_t* data = static_cast<int8_t*>(inputData);
        float invScale = 1.0f / g_input_scale;
        for (int y = 0; y < inH; ++y) {
            int srcY = y * imgH / inH;
            for (int x = 0; x < inW; ++x) {
                int srcX = x * imgW / inW;
                int srcIdx = (srcY * imgW + srcX) * 4;
                int idx = (y * inW + x) * 3;
                data[idx + 0] = (int8_t)std::round(img[srcIdx + 0] / 255.0f * invScale + g_input_zero_point);
                data[idx + 1] = (int8_t)std::round(img[srcIdx + 1] / 255.0f * invScale + g_input_zero_point);
                data[idx + 2] = (int8_t)std::round(img[srcIdx + 2] / 255.0f * invScale + g_input_zero_point);
            }
        }
    } else if (g_input_type == kTfLiteUInt8) {
        uint8_t* data = static_cast<uint8_t*>(inputData);
        float invScale = 1.0f / g_input_scale;
        for (int y = 0; y < inH; ++y) {
            int srcY = y * imgH / inH;
            for (int x = 0; x < inW; ++x) {
                int srcX = x * imgW / inW;
                int srcIdx = (srcY * imgW + srcX) * 4;
                int idx = (y * inW + x) * 3;
                data[idx + 0] = (uint8_t)std::round(img[srcIdx + 0] / 255.0f * invScale + g_input_zero_point);
                data[idx + 1] = (uint8_t)std::round(img[srcIdx + 1] / 255.0f * invScale + g_input_zero_point);
                data[idx + 2] = (uint8_t)std::round(img[srcIdx + 2] / 255.0f * invScale + g_input_zero_point);
            }
        }
    }
    stbi_image_free(img);
    long long t2 = getTimeUs();

    // 3. 推理
    if (TfLiteInterpreterInvoke(g_interpreter) != kTfLiteOk) {
        loge("Inference failed");
        return;
    }
    long long t3 = getTimeUs();

    // 4. 解码输出
    std::vector<Detection> dets;
    int outCount = TfLiteInterpreterGetOutputTensorCount(g_interpreter);
    for (int i = 0; i < outCount; ++i) {
        const TfLiteTensor* out = TfLiteInterpreterGetOutputTensor(g_interpreter, i);
        OutputTensorData od;
        if (readOutput(out, od)) {
            auto decoded = decodeYoloOutput(od);
            if (!decoded.empty()) {
                dets = nms(decoded, g_cfg.nmsThresh);
                break;
            }
        }
    }

    // 5. PID 自瞄
    long long frameTime = t3 - t0;
    pid_aim(dets, frameTime);

    // 6. 更新全局检测结果
    {
        std::lock_guard<std::mutex> lock(g_detections_mutex);
        g_detections = dets;
    }

    g_frameCount++;
    g_lastFrameTime = frameTime;
    float fps = 1000000.0f / (float)frameTime;
    g_detectFPS = fps;
    g_aimFPS = fps;

    long long t4 = getTimeUs();
    if (g_frameCount % 60 == 0) {
        logd("Frame #%d: cap=%.1fms pre=%.1fms inf=%.1fms dets=%zu fps=%.1f",
             g_frameCount,
             (t1-t0)/1000.0, (t2-t1)/1000.0, (t3-t2)/1000.0,
             dets.size(), fps);
    }
}

// ═══════════════════════════════════════════════════════════════════
//  触摸注入 (uinput)
// ═══════════════════════════════════════════════════════════════════
bool touch_init_aimbot(int screenW, int screenH) {
    (void)screenW; (void)screenH;

    g_uinput_fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (g_uinput_fd < 0) {
        loge("open /dev/uinput failed");
        return false;
    }

    ioctl(g_uinput_fd, UI_SET_EVBIT, EV_KEY);
    ioctl(g_uinput_fd, UI_SET_KEYBIT, BTN_TOUCH);
    ioctl(g_uinput_fd, UI_SET_EVBIT, EV_ABS);
    ioctl(g_uinput_fd, UI_SET_ABSBIT, ABS_MT_SLOT);
    ioctl(g_uinput_fd, UI_SET_ABSBIT, ABS_MT_POSITION_X);
    ioctl(g_uinput_fd, UI_SET_ABSBIT, ABS_MT_POSITION_Y);
    ioctl(g_uinput_fd, UI_SET_ABSBIT, ABS_MT_TRACKING_ID);
    ioctl(g_uinput_fd, UI_SET_ABSBIT, ABS_MT_TOUCH_MAJOR);

    struct uinput_setup usetup = {};
    usetup.id.bustype = BUS_VIRTUAL;
    usetup.id.vendor = 0x1234;
    usetup.id.product = 0x5678;
    strcpy(usetup.name, "yolov8touch");
    ioctl(g_uinput_fd, UI_DEV_SETUP, &usetup);
    ioctl(g_uinput_fd, UI_DEV_CREATE);

    logd("uinput device created");
    return true;
}

void touch_close_aimbot() {
    if (g_uinput_fd >= 0) {
        ioctl(g_uinput_fd, UI_DEV_DESTROY);
        close(g_uinput_fd);
        g_uinput_fd = -1;
    }
}

static void write_uinput(int type, int code, int value) {
    struct input_event ev = {};
    ev.type = type;
    ev.code = code;
    ev.value = value;
    gettimeofday(&ev.time, nullptr);
    write(g_uinput_fd, &ev, sizeof(ev));
}

void touch_down(int slot, int id, int x, int y) {
    if (g_uinput_fd < 0) return;
    write_uinput(EV_ABS, ABS_MT_SLOT, slot);
    write_uinput(EV_ABS, ABS_MT_TRACKING_ID, id);
    write_uinput(EV_ABS, ABS_MT_POSITION_X, x);
    write_uinput(EV_ABS, ABS_MT_POSITION_Y, y);
    write_uinput(EV_ABS, ABS_MT_TOUCH_MAJOR, 50);
    write_uinput(EV_KEY, BTN_TOUCH, 1);
    write_uinput(EV_SYN, SYN_REPORT, 0);
}

void touch_move(int slot, int x, int y) {
    if (g_uinput_fd < 0) return;
    write_uinput(EV_ABS, ABS_MT_SLOT, slot);
    write_uinput(EV_ABS, ABS_MT_POSITION_X, x);
    write_uinput(EV_ABS, ABS_MT_POSITION_Y, y);
    write_uinput(EV_SYN, SYN_REPORT, 0);
}

void touch_up(int slot) {
    if (g_uinput_fd < 0) return;
    write_uinput(EV_ABS, ABS_MT_SLOT, slot);
    write_uinput(EV_ABS, ABS_MT_TRACKING_ID, -1);
    write_uinput(EV_KEY, BTN_TOUCH, 0);
    write_uinput(EV_SYN, SYN_REPORT, 0);
}

// ═══════════════════════════════════════════════════════════════════
//  开关
// ═══════════════════════════════════════════════════════════════════
void aimbot_toggle_enabled() {
    g_cfg.enabled = !g_cfg.enabled;
    logd("Aimbot %s", g_cfg.enabled ? "ON" : "OFF");
    if (!g_cfg.enabled && g_pid.pointerDown) {
        touch_up(TOUCH_VIRTUAL_SLOT);
        g_pid.pointerDown = false;
    }
}

void aimbot_toggle_fire() {
    g_cfg.fireEnabled = !g_cfg.fireEnabled;
    logd("Fire %s", g_cfg.fireEnabled ? "ON" : "OFF");
}

void aimbot_set_fire(bool down) {
    if (g_cfg.fireEnabled) {
        if (down) touch_down(TOUCH_TRIGGER_SLOT, TOUCH_TRIGGER_ID,
                             g_cfg.screenW / 2, g_cfg.screenH / 2);
        else touch_up(TOUCH_TRIGGER_SLOT);
    }
}