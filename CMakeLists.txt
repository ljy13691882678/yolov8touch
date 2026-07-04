cmake_minimum_required(VERSION 3.22.1)
project("yolov8touch")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ─── NCNN ────────────────────────────────────────────────────────
set(NCNN_DIR ${CMAKE_CURRENT_SOURCE_DIR}/ncnn/arm64-v8a)
set(NCNN_INCLUDE_DIR ${NCNN_DIR}/include)
set(NCNN_LIB_DIR ${NCNN_DIR}/lib)

if(NOT EXISTS ${NCNN_LIB_DIR}/libncnn.a)
    message(FATAL_ERROR "NCNN lib not found! Copy ncnn/ from YoloTouchHelp/app/src/main/cpp/ncnn/")
endif()

add_library(ncnn STATIC IMPORTED)
set_target_properties(ncnn PROPERTIES IMPORTED_LOCATION ${NCNN_LIB_DIR}/libncnn.a)

macro(add_vulkan_lib name)
    add_library(${name} STATIC IMPORTED)
    set_target_properties(${name} PROPERTIES IMPORTED_LOCATION ${NCNN_LIB_DIR}/lib${name}.a)
endmacro()
add_vulkan_lib(glslang)
add_vulkan_lib(SPIRV)
add_vulkan_lib(glslang-default-resource-limits)
add_vulkan_lib(MachineIndependent)
add_vulkan_lib(OSDependent)
add_vulkan_lib(OGLCompiler)
add_vulkan_lib(GenericCodeGen)

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
    ${NCNN_INCLUDE_DIR}
    ${IMGUI_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# OpenMP support for NCNN
find_package(OpenMP REQUIRED)

target_link_libraries(yolov8touch
    ncnn glslang SPIRV glslang-default-resource-limits
    MachineIndependent OSDependent OGLCompiler GenericCodeGen
    OpenMP::OpenMP_CXX
    EGL GLESv3
    gui ui binder utils
    android log
)

set_target_properties(yolov8touch PROPERTIES OUTPUT_NAME "yolov8touch")