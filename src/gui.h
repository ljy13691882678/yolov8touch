#pragma once
#include "imgui/imgui.h"
#include "overlay.h"

bool gui_init(OverlayWindow* ov);
void gui_render();
void gui_shutdown();
void gui_new_frame();