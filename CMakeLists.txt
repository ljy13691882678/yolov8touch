cmake_minimum_required(VERSION 3.22.1)
project("yolov8touch")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ─── TFLite ───────────────────────────────────────────────────────
set(TFLITE_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/tflite/include)

# ─── ImGui ────────────────────────────────────────────────────────
set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/imgui)

# ─── 主程序 ──────────────────────────────────────────────────────
add_executable(yolov8touch
    src/main.cpp
    src/aimbot.cpp
    src/gui.cpp
    src/overlay.cpp
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_impl_opengl3.cpp
)

target_include_directories(yolov8touch PRIVATE
    ${TFLITE_INCLUDE_DIR}
    ${IMGUI_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# ─── TFLite shared library (stub for CI, real .so on device) ─────
if(DEFINED STUB_LIBS_DIR)
    add_library(tensorflowlite_jni SHARED IMPORTED)
    set_target_properties(tensorflowlite_jni PROPERTIES
        IMPORTED_LOCATION ${STUB_LIBS_DIR}/libtensorflowlite_jni.so)
endif()

target_link_libraries(yolov8touch
    tensorflowlite_jni
    EGL GLESv3
    android log
)

set_target_properties(yolov8touch PROPERTIES OUTPUT_NAME "yolov8touch")

# ─── RPATH: 让二进制在同目录找 .so 文件 ───────────────────────────
target_link_options(yolov8touch PRIVATE "-Wl,-rpath,$ORIGIN")