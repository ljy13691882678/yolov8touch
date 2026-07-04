#define STB_IMAGE_IMPLEMENTATION
#include "aimbot.h"
#include <linux/uinput.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <errno.h>

volatile int g_running = 1;
AimbotConfig g_cfg;
std::mutex g_detections_mutex;
std::vector<Detection> g_detections;
float g_aimFPS = 0, g_detectFPS = 0;
int g_frameCount = 0;
long long g_lastFrameTime = 0;

static ncnn::Net* g_net = nullptr;
static PIDState g_pid;
static int g_uinput_fd = -1;
static int g_screenW = 0, g_screenH = 0;

// ─── 工具函数 ───────────────────────────────────────────────────
static inline long long now_us() {
    struct timeval tv; gettimeofday(&tv, nullptr);
    return tv.tv_sec * 1000000LL + tv.tv_usec;
}
static inline float sigmoid(float x) { return 1.0f / (1.0f + expf(-x)); }

// ─── 屏幕捕获 ───────────────────────────────────────────────────
static uint8_t* capture_screen_png(size_t* out_len) {
    int pipefd[2];
    if (pipe(pipefd) < 0) return nullptr;
    pid_t pid = fork();
    if (pid == 0) {
        close(pipefd[0]); dup2(pipefd[1], STDOUT_FILENO); close(pipefd[1]);
        execlp("screencap", "screencap", "-p", (char*)nullptr);
        _exit(1);
    }
    close(pipefd[1]);
    size_t cap = 4 * 1024 * 1024;
    uint8_t* buf = (uint8_t*)malloc(cap);
    size_t total = 0;
    ssize_t n;
    while ((n = read(pipefd[0], buf + total, cap - total)) > 0) {
        total += n;
        if (total >= cap) { cap *= 2; buf = (uint8_t*)realloc(buf, cap); }
    }
    close(pipefd[0]); waitpid(pid, nullptr, 0);
    if (total == 0) { free(buf); return nullptr; }
    *out_len = total; return buf;
}

// ─── NCNN 推理 ──────────────────────────────────────────────────
static bool init_ncnn(const char* path) {
    g_net = new ncnn::Net();
    g_net->opt.use_vulkan_compute = false;
    g_net->opt.num_threads = 4;
    char bin[256]; strncpy(bin, path, 250); bin[250] = 0;
    char* dot = strrchr(bin, '.'); if (dot) strcpy(dot, ".bin");
    if (g_net->load_param(path) != 0 || g_net->load_model(bin) != 0) {
        fprintf(stderr, "ERR: model load failed\n"); return false;
    }
    return true;
}

static int detect_objects(uint8_t* rgba, int w, int h, int ox, int oy, int rw, int rh,
                          Detection* dets, int maxDets) {
    if (!g_net) return 0;
    int iw = g_cfg.inputW, ih = g_cfg.inputH;
    ncnn::Mat in = ncnn::Mat::from_pixels(rgba + (oy * w + ox) * 4, ncnn::Mat::PIXEL_RGBA, rw, rh);
    ncnn::Mat rs; ncnn::resize_bilinear(in, rs, iw, ih);
    const float m[3] = {0,0,0}, n[3] = {1/255.0f, 1/255.0f, 1/255.0f};
    rs.substract_mean_normalize(m, n);
    ncnn::Extractor ex = g_net->create_extractor();
    ex.input("in0", rs);
    ncnn::Mat out; ex.extract("out0", out);
    int na = out.h, fl = out.w, nc = fl - 16;
    if (nc <= 0) nc = 1;
    float iiw = 1.0f/iw, iih = 1.0f/ih;
    int count = 0;
    for (int a = 0; a < na && count < maxDets; a++) {
        const float* row = out.row(a);
        int bc = 0; float bs = 0;
        for (int c = 0; c < nc; c++) { float s = sigmoid(row[16+c]); if (s > bs) { bs = s; bc = c; } }
        if (bs < g_cfg.confThresh) continue;
        float box[4] = {0,0,0,0};
        for (int b = 0; b < 4; b++) {
            float se = 0, w = 0; const float dw[4] = {0,1,2,3};
            for (int r = 0; r < 4; r++) { float e = expf(row[b*4+r]); se += e; w += e * dw[r]; }
            box[b] = w / se;
        }
        int stride = na > 8400 ? 8 : na > 2100 ? 16 : 32;
        int grid = (int)sqrtf((float)na);
        int gy = a / grid, gx = a % grid;
        float cx = (gx + 0.5f) * stride * iiw, cy = (gy + 0.5f) * stride * iih;
        float x1 = (cx - box[0]*iiw - box[2]*iiw*0.5f) * rw + ox;
        float y1 = (cy - box[1]*iih - box[3]*iih*0.5f) * rh + oy;
        float x2 = x1 + box[2]*iiw * rw, y2 = y1 + box[3]*iih * rh;
        if (x1 < 0) x1 = 0; if (y1 < 0) y1 = 0;
        if (x2 > w) x2 = w; if (y2 > h) y2 = h;
        dets[count++] = {x1, y1, x2, y2, bs, (int)bc};
    }
    // NMS
    if (count > 1) {
        for (int i = 0; i < count-1; i++) for (int j = i+1; j < count; j++)
            if (dets[j].score > dets[i].score) { Detection t = dets[i]; dets[i] = dets[j]; dets[j] = t; }
        bool* sup = (bool*)calloc(count, 1); int k = 0;
        for (int i = 0; i < count; i++) {
            if (sup[i]) continue; dets[k++] = dets[i];
            for (int j = i+1; j < count; j++) {
                if (sup[j] || dets[i].classId != dets[j].classId) continue;
                float ix1 = fmaxf(dets[i].x1, dets[j].x1), iy1 = fmaxf(dets[i].y1, dets[j].y1);
                float ix2 = fminf(dets[i].x2, dets[j].x2), iy2 = fminf(dets[i].y2, dets[j].y2);
                float iw = ix2-ix1, ih = iy2-iy1;
                if (iw <= 0 || ih <= 0) continue;
                float ia = iw*ih, aa = (dets[i].x2-dets[i].x1)*(dets[i].y2-dets[i].y1);
                float ba = (dets[j].x2-dets[j].x1)*(dets[j].y2-dets[j].y1);
                if (ia / (aa+ba-ia) > g_cfg.nmsThresh) sup[j] = true;
            }
        }
        count = k; free(sup);
    }
    return count;
}

// ─── 目标选择 ───────────────────────────────────────────────────
static Detection* select_target(Detection* dets, int n, int cx, int cy) {
    float best = 1e9f; Detection* b = nullptr;
    for (int i = 0; i < n; i++) {
        float tx = (dets[i].x1+dets[i].x2)*0.5f, ty = (dets[i].y1+dets[i].y2)*0.5f;
        float d = (tx-cx)*(tx-cx) + (ty-cy)*(ty-cy);
        if (d < best) { best = d; b = &dets[i]; }
    }
    return b;
}

// ─── PID 控制 ───────────────────────────────────────────────────
static void pid_aim(float tx, float ty) {
    float cx = g_pid.centerX, cy = g_pid.centerY;
    float ex = tx - cx, ey = ty - cy;
    const float ct = 5.0f;
    if (fabsf(ex) < ct && fabsf(ey) < ct) {
        if (g_pid.pointerDown && ++g_pid.deadzoneFrames >= 3) {
            touch_up(TOUCH_VIRTUAL_SLOT); g_pid.pointerDown = false; g_pid.deadzoneFrames = 0;
        }
        return;
    }
    g_pid.deadzoneFrames = 0;
    if (!g_pid.pointerDown) {
        touch_down(TOUCH_VIRTUAL_SLOT, TOUCH_VIRTUAL_ID, (int)cx, (int)cy);
        g_pid.pointerDown = true;
    }
    if (ex * g_pid.prevErrorX <= 0) g_pid.integralX = 0;
    if (ey * g_pid.prevErrorY <= 0) g_pid.integralY = 0;
    float dt = 0.008f;
    g_pid.integralX += ex * dt; g_pid.integralY += ey * dt;
    float il = 100; if (g_pid.integralX > il) g_pid.integralX = il; if (g_pid.integralX < -il) g_pid.integralX = -il;
    if (g_pid.integralY > il) g_pid.integralY = il; if (g_pid.integralY < -il) g_pid.integralY = -il;
    float dx = (ex - g_pid.prevErrorX) / dt, dy = (ey - g_pid.prevErrorY) / dt;
    float mx = ex * g_cfg.kp + g_pid.integralX * g_cfg.ki + dx * g_cfg.kd;
    float my = ey * g_cfg.kp + g_pid.integralY * g_cfg.ki + dy * g_cfg.kd;
    mx = g_pid.lastMoveX * g_cfg.smooth + mx * (1 - g_cfg.smooth);
    my = g_pid.lastMoveY * g_cfg.smooth + my * (1 - g_cfg.smooth);
    g_pid.lastMoveX = mx; g_pid.lastMoveY = my;
    g_pid.prevErrorX = ex; g_pid.prevErrorY = ey;
    float d = sqrtf(mx*mx + my*my);
    if (d > g_cfg.maxMovePerFrame) { mx = mx/d * g_cfg.maxMovePerFrame; my = my/d * g_cfg.maxMovePerFrame; }
    g_pid.centerX += mx; g_pid.centerY += my;
    if (g_pid.centerX < 0) g_pid.centerX = 0; if (g_pid.centerY < 0) g_pid.centerY = 0;
    if (g_pid.centerX > g_screenW) g_pid.centerX = g_screenW;
    if (g_pid.centerY > g_screenH) g_pid.centerY = g_screenH;
    touch_move(TOUCH_VIRTUAL_SLOT, (int)g_pid.centerX, (int)g_pid.centerY);
}

static void pid_reset(int cx, int cy) {
    memset(&g_pid, 0, sizeof(g_pid));
    g_pid.centerX = cx; g_pid.centerY = cy;
}

// ─── uinput 触摸注入 ────────────────────────────────────────────
bool touch_init_aimbot(int sw, int sh) {
    g_screenW = sw; g_screenH = sh;
    g_uinput_fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (g_uinput_fd < 0) { g_uinput_fd = open("/dev/input/uinput", O_WRONLY | O_NONBLOCK); }
    if (g_uinput_fd < 0) { fprintf(stderr, "ERR: open uinput failed\n"); return false; }
    ioctl(g_uinput_fd, UI_SET_EVBIT, EV_KEY);
    ioctl(g_uinput_fd, UI_SET_EVBIT, EV_SYN);
    ioctl(g_uinput_fd, UI_SET_EVBIT, EV_ABS);
    ioctl(g_uinput_fd, UI_SET_KEYBIT, BTN_TOUCH);
    ioctl(g_uinput_fd, UI_SET_ABSBIT, ABS_MT_SLOT);
    ioctl(g_uinput_fd, UI_SET_ABSBIT, ABS_MT_TRACKING_ID);
    ioctl(g_uinput_fd, UI_SET_ABSBIT, ABS_MT_POSITION_X);
    ioctl(g_uinput_fd, UI_SET_ABSBIT, ABS_MT_POSITION_Y);
    ioctl(g_uinput_fd, UI_SET_ABSBIT, ABS_MT_TOUCH_MAJOR);
    ioctl(g_uinput_fd, UI_SET_ABSBIT, ABS_MT_PRESSURE);
    struct uinput_user_dev udev = {};
    snprintf(udev.name, UINPUT_MAX_NAME_SIZE, "yolov8touch");
    udev.id.bustype = BUS_VIRTUAL; udev.id.vendor = 0x1; udev.id.product = 0x1;
    udev.absmax[ABS_MT_SLOT] = 15;
    udev.absmax[ABS_MT_POSITION_X] = sw; udev.absmax[ABS_MT_POSITION_Y] = sh;
    udev.absmax[ABS_MT_TOUCH_MAJOR] = 255; udev.absmax[ABS_MT_PRESSURE] = 255;
    write(g_uinput_fd, &udev, sizeof(udev));
    if (ioctl(g_uinput_fd, UI_DEV_CREATE) < 0) { fprintf(stderr, "ERR: UI_DEV_CREATE\n"); return false; }
    return true;
}

void touch_close_aimbot() {
    if (g_uinput_fd >= 0) { ioctl(g_uinput_fd, UI_DEV_DESTROY); close(g_uinput_fd); g_uinput_fd = -1; }
}

void touch_down(int slot, int id, int x, int y) {
    if (g_uinput_fd < 0) return;
    struct input_event ev[6];
    memset(ev, 0, sizeof(ev));
    int i = 0;
    ev[i].type = EV_ABS; ev[i].code = ABS_MT_SLOT; ev[i].value = slot; i++;
    ev[i].type = EV_ABS; ev[i].code = ABS_MT_TRACKING_ID; ev[i].value = id; i++;
    ev[i].type = EV_ABS; ev[i].code = ABS_MT_POSITION_X; ev[i].value = x; i++;
    ev[i].type = EV_ABS; ev[i].code = ABS_MT_POSITION_Y; ev[i].value = y; i++;
    ev[i].type = EV_ABS; ev[i].code = ABS_MT_TOUCH_MAJOR; ev[i].value = 10; i++;
    ev[i].type = EV_SYN; ev[i].code = SYN_REPORT; ev[i].value = 0; i++;
    write(g_uinput_fd, ev, sizeof(ev));
}

void touch_move(int slot, int x, int y) {
    if (g_uinput_fd < 0) return;
    struct input_event ev[4];
    memset(ev, 0, sizeof(ev));
    int i = 0;
    ev[i].type = EV_ABS; ev[i].code = ABS_MT_SLOT; ev[i].value = slot; i++;
    ev[i].type = EV_ABS; ev[i].code = ABS_MT_POSITION_X; ev[i].value = x; i++;
    ev[i].type = EV_ABS; ev[i].code = ABS_MT_POSITION_Y; ev[i].value = y; i++;
    ev[i].type = EV_SYN; ev[i].code = SYN_REPORT; ev[i].value = 0; i++;
    write(g_uinput_fd, ev, sizeof(ev));
}

void touch_up(int slot) {
    if (g_uinput_fd < 0) return;
    struct input_event ev[3];
    memset(ev, 0, sizeof(ev));
    int i = 0;
    ev[i].type = EV_ABS; ev[i].code = ABS_MT_SLOT; ev[i].value = slot; i++;
    ev[i].type = EV_ABS; ev[i].code = ABS_MT_TRACKING_ID; ev[i].value = -1; i++;
    ev[i].type = EV_SYN; ev[i].code = SYN_REPORT; ev[i].value = 0; i++;
    write(g_uinput_fd, ev, sizeof(ev));
}

// ─── 生命周期 ───────────────────────────────────────────────────
bool aimbot_init() {
    if (!init_ncnn(g_cfg.modelPath)) return false;
    if (!touch_init_aimbot(g_cfg.screenW, g_cfg.screenH)) return false;
    pid_reset(g_cfg.screenW / 2, g_cfg.screenH / 2);
    fprintf(stderr, "Aimbot: model loaded, uinput ready\n");
    return true;
}

void aimbot_shutdown() {
    if (g_pid.pointerDown) touch_up(TOUCH_VIRTUAL_SLOT);
    touch_close_aimbot();
    if (g_net) { delete g_net; g_net = nullptr; }
}

void aimbot_toggle_enabled() { g_cfg.enabled = !g_cfg.enabled; }
void aimbot_toggle_fire() { g_cfg.fireEnabled = !g_cfg.fireEnabled; }
void aimbot_set_fire(bool down) {
    if (down) touch_down(TOUCH_TRIGGER_SLOT, TOUCH_TRIGGER_ID, g_cfg.screenW / 2, g_cfg.screenH / 2);
    else touch_up(TOUCH_TRIGGER_SLOT);
}

void aimbot_loop_iteration() {
    static Detection dets[64];
    // 截屏
    size_t pngLen = 0;
    uint8_t* png = capture_screen_png(&pngLen);
    if (!png || pngLen == 0) return;
    int iw, ih, ch;
    uint8_t* rgba = stbi_load_from_memory(png, (int)pngLen, &iw, &ih, &ch, 4);
    free(png);
    if (!rgba) return;
    // 裁剪中心区域
    int rw = g_cfg.rangeRadius * 2, rh = g_cfg.rangeRadius * 2;
    int ox = (iw - rw) / 2, oy = (ih - rh) / 2;
    if (ox < 0) { ox = 0; rw = iw; }
    if (oy < 0) { oy = 0; rh = ih; }
    // 推理
    int n = g_cfg.enabled ? detect_objects(rgba, iw, ih, ox, oy, rw, rh, dets, 64) : 0;
    stbi_image_free(rgba);
    // 更新检测结果
    {
        std::lock_guard<std::mutex> lk(g_detections_mutex);
        g_detections.clear();
        for (int i = 0; i < n; i++) g_detections.push_back(dets[i]);
    }
    // PID 自瞄
    if (g_cfg.enabled && n > 0) {
        int scx = g_cfg.screenW / 2, scy = g_cfg.screenH / 2;
        Detection* t = select_target(dets, n, scx, scy);
        if (t) pid_aim((t->x1+t->x2)*0.5f, (t->y1+t->y2)*0.5f);
    } else if (!g_cfg.enabled && g_pid.pointerDown) {
        touch_up(TOUCH_VIRTUAL_SLOT);
        g_pid.pointerDown = false;
    }
    // 帧率统计
    g_frameCount++;
    long long now = now_us();
    if (g_lastFrameTime == 0) g_lastFrameTime = now;
    if (now - g_lastFrameTime >= 1000000LL) {
        g_aimFPS = g_frameCount * 1000000.0f / (float)(now - g_lastFrameTime);
        g_frameCount = 0; g_lastFrameTime = now;
    }
}