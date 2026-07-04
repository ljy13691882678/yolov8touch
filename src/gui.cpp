#include "gui.h"
#include "aimbot.h"
#include "imgui/imgui_impl_opengl3.h"
#include <cstdio>
#include <cmath>

static ImVec4 g_clearColor = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
static bool g_showSettings = true;
static char g_modelPathBuf[256] = "";
static char g_reloadStatus[128] = "";

bool gui_init(OverlayWindow* ov) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)ov->width, (float)ov->height);
    io.IniFilename = nullptr;
    // 深色主题
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 8.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.WindowBorderSize = 0;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.08f, 0.08f, 0.10f, 0.92f);
    style.Colors[ImGuiCol_TitleBg] = ImVec4(0.12f, 0.12f, 0.16f, 0.95f);
    style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.16f, 0.16f, 0.22f, 0.95f);
    style.Colors[ImGuiCol_Button] = ImVec4(0.20f, 0.22f, 0.28f, 0.85f);
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.28f, 0.30f, 0.38f, 0.90f);
    style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.18f, 0.50f, 0.30f, 0.90f);
    style.Colors[ImGuiCol_CheckMark] = ImVec4(0.26f, 0.76f, 0.45f, 1.0f);
    style.Colors[ImGuiCol_SliderGrab] = ImVec4(0.26f, 0.76f, 0.45f, 0.80f);
    style.Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.26f, 0.86f, 0.45f, 1.0f);
    style.Colors[ImGuiCol_FrameBg] = ImVec4(0.16f, 0.18f, 0.22f, 0.80f);
    style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.22f, 0.24f, 0.30f, 0.85f);
    style.Colors[ImGuiCol_Header] = ImVec4(0.20f, 0.22f, 0.28f, 0.85f);
    style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.26f, 0.28f, 0.36f, 0.90f);

    ImGui_ImplOpenGL3_Init("#version 300 es");
    fprintf(stderr, "ImGui initialized\n");
    return true;
}

void gui_new_frame() {
    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();
}

void gui_render() {
    // ─── 检测框叠加 ─────────────────────────────────────────────
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddText(ImVec2(10, 10), IM_COL32(255, 255, 255, 180), "YOLOv8 Touch Aimbot");

    // FPS
    char fpsText[64];
    snprintf(fpsText, sizeof(fpsText), "FPS: %.1f | Detections: %zu | %s",
        g_aimFPS, g_detections.size(), g_cfg.enabled ? "AIM ON" : "AIM OFF");
    dl->AddText(ImVec2(10, 30), IM_COL32(128, 255, 128, 200), fpsText);

    // 绘制检测框
    {
        std::lock_guard<std::mutex> lk(g_detections_mutex);
        for (auto& d : g_detections) {
            dl->AddRect(ImVec2(d.x1, d.y1), ImVec2(d.x2, d.y2),
                IM_COL32(0, 255, 0, 180), 0.0f, 0, 2.0f);
            // 中心十字
            float cx = (d.x1 + d.x2) * 0.5f, cy = (d.y1 + d.y2) * 0.5f;
            dl->AddLine(ImVec2(cx - 8, cy), ImVec2(cx + 8, cy), IM_COL32(0, 255, 0, 120));
            dl->AddLine(ImVec2(cx, cy - 8), ImVec2(cx, cy + 8), IM_COL32(0, 255, 0, 120));
            // 置信度
            char buf[32]; snprintf(buf, sizeof(buf), "%.0f%%", d.score * 100);
            dl->AddText(ImVec2(d.x1, d.y1 - 16), IM_COL32(0, 255, 0, 200), buf);
        }
    }

    // ─── 设置面板 ────────────────────────────────────────────────
    if (g_showSettings) {
        ImGui::SetNextWindowPos(ImVec2(10, 50), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(270, 500), ImGuiCond_FirstUseEver);
        ImGui::Begin("Control Panel", &g_showSettings,
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize);

        // 状态
        ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "Status: %s",
            g_cfg.enabled ? "AIMING" : "IDLE");
        ImGui::SameLine();
        if (ImGui::Button(g_cfg.enabled ? "STOP" : "START", ImVec2(60, 0))) {
            aimbot_toggle_enabled();
        }
        ImGui::SameLine();
        if (ImGui::Button(g_cfg.fireEnabled ? "FIRE OFF" : "FIRE ON", ImVec2(60, 0))) {
            aimbot_toggle_fire();
        }
        ImGui::Separator();

        // 模型信息
        if (ImGui::CollapsingHeader("Model", ImGuiTreeNodeFlags_DefaultOpen)) {
            // 模型路径输入
            ImGui::Text("Model Path:");
            ImGui::PushItemWidth(-1);
            if (g_modelPathBuf[0] == '\0') {
                strncpy(g_modelPathBuf, g_cfg.modelPath, sizeof(g_modelPathBuf) - 1);
            }
            ImGui::InputText("##modelPath", g_modelPathBuf, sizeof(g_modelPathBuf));
            ImGui::PopItemWidth();

            if (ImGui::Button("Load Model", ImVec2(-1, 0))) {
                if (g_modelPathBuf[0] != '\0') {
                    bool ok = aimbot_reload_model(g_modelPathBuf);
                    snprintf(g_reloadStatus, sizeof(g_reloadStatus), "%s",
                             ok ? "Model loaded OK" : "Failed to load model!");
                }
            }

            // 状态提示
            if (g_reloadStatus[0]) {
                ImVec4 col = strstr(g_reloadStatus, "OK") ? ImVec4(0.3f, 1.0f, 0.3f, 1.0f)
                                                          : ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
                ImGui::TextColored(col, "%s", g_reloadStatus);
            }

            ImGui::Text("Current: %s", g_cfg.modelPath);
            ImGui::Text("Input: %dx%d", g_cfg.inputW, g_cfg.inputH);
            ImGui::SliderInt("Range Radius", &g_cfg.rangeRadius, 100, 800);
            ImGui::SliderFloat("Conf Threshold", &g_cfg.confThresh, 0.05f, 0.95f);
            ImGui::SliderFloat("NMS Threshold", &g_cfg.nmsThresh, 0.1f, 0.9f);
        }

        // PID 参数
        if (ImGui::CollapsingHeader("PID Control")) {
            ImGui::SliderFloat("Kp", &g_cfg.kp, 0.01f, 2.0f, "%.2f");
            ImGui::SliderFloat("Ki", &g_cfg.ki, 0.0f, 0.5f, "%.3f");
            ImGui::SliderFloat("Kd", &g_cfg.kd, 0.0f, 0.5f, "%.3f");
            ImGui::SliderFloat("Smooth", &g_cfg.smooth, 0.0f, 0.95f, "%.2f");
            ImGui::SliderFloat("Max Move/frame", &g_cfg.maxMovePerFrame, 100.0f, 3000.0f, "%.0f");
            if (ImGui::Button("Reset PID")) {
                g_cfg.kp = 0.30f; g_cfg.ki = 0.02f; g_cfg.kd = 0.08f;
                g_cfg.smooth = 0.35f; g_cfg.maxMovePerFrame = 1200.0f;
            }
        }

        // 屏幕
        if (ImGui::CollapsingHeader("Screen")) {
            ImGui::InputInt("Width", &g_cfg.screenW);
            ImGui::InputInt("Height", &g_cfg.screenH);
        }

        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
            "FPS: %.1f | Dets: %zu", g_aimFPS, g_detections.size());
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
            "Ctrl+C to exit");

        ImGui::End();
    }

    // 渲染
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

void gui_shutdown() {
    ImGui_ImplOpenGL3_Shutdown();
    ImGui::DestroyContext();
}