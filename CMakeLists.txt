cmake_minimum_required(VERSION 3.22.1)
project("yolov8touch")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ─── TFLite ───────────────────────────────────────────────────────
# TFLite C API headers are bundled in src/tflite/include/
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

# ─── Stub headers for Android private APIs (gui/ui/binder) ────────
# NDK doesn't ship libgui/libui headers; these stubs let CI compile.
# At runtime, the real system libs are used on the device.
target_include_directories(yolov8touch BEFORE PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/stubs
)

# ─── TFLite shared library ────────────────────────────────────────
# CI provides stub .so; real libtensorflowlite_jni.so is on device
if(DEFINED STUB_LIBS_DIR)
    add_library(tensorflowlite_jni SHARED IMPORTED)
    set_target_properties(tensorflowlite_jni PROPERTIES
        IMPORTED_LOCATION ${STUB_LIBS_DIR}/libtensorflowlite_jni.so)
endif()

# Private Android system libs (gui/ui/binder/utils) are not in NDK.
if(DEFINED STUB_LIBS_DIR)
    macro(add_stub_lib name)
        add_library(${name} SHARED IMPORTED)
        set_target_properties(${name} PROPERTIES IMPORTED_LOCATION ${STUB_LIBS_DIR}/lib${name}.so)
    endmacro()
    add_stub_lib(gui)
    add_stub_lib(ui)
    add_stub_lib(binder)
    add_stub_lib(utils)
endif()

target_link_libraries(yolov8touch
    tensorflowlite_jni
    EGL GLESv3
    gui ui binder utils
    android log
)

set_target_properties(yolov8touch PROPERTIES OUTPUT_NAME "yolov8touch")

# ─── RPATH: 让二进制在同目录找 .so 文件 ───────────────────────────
# 无需 LD_LIBRARY_PATH，只需把 libtensorflowlite_jni.so 放在同目录
# Android NDK 不支持 CMake BUILD_RPATH，直接传 linker flag
target_link_options(yolov8touch PRIVATE "-Wl,-rpath,$ORIGIN")