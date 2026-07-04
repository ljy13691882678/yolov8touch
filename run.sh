#!/bin/bash
# yolov8touch — 一键构建 + 推送 + 运行脚本
# 用法: ./run.sh [--build] [--push] [--run] [--width 1080] [--height 2400]
# 默认: 构建 + 推送 + 运行

set -e

# ─── 配置 ────────────────────────────────────────────────────────
NDK_PATH="${ANDROID_NDK_HOME:-$HOME/Android/Sdk/ndk/27.0.12077973}"
TOOLCHAIN="$NDK_PATH/toolchains/llvm/prebuilt/linux-x86_64"
TARGET="aarch64-linux-android"
API_LEVEL=31
BINARY_NAME="yolov8touch"
DEVICE_PATH="/data/local/tmp/$BINARY_NAME"
MODEL_PATH="/data/local/tmp/yolov8.param"
SCREEN_W=1080
SCREEN_H=2400

# ─── 参数解析 ────────────────────────────────────────────────────
DO_BUILD=true; DO_PUSH=true; DO_RUN=true
while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-only) DO_PUSH=false; DO_RUN=false ;;
        --push-only)  DO_BUILD=false ;;
        --run-only)   DO_BUILD=false; DO_PUSH=false ;;
        --width)      SCREEN_W="$2"; shift ;;
        --height)     SCREEN_H="$2"; shift ;;
        --model)      MODEL_PATH="$2"; shift ;;
        --help|-h)    echo "Usage: $0 [--build-only|--push-only|--run-only] [--width W] [--height H] [--model PATH]"; exit 0 ;;
    esac
    shift
done

# ─── 构建 ────────────────────────────────────────────────────────
if $DO_BUILD; then
    echo "=== Building $BINARY_NAME ==="
    # 检查 NCNN (从 YoloTouchHelp 复制)
    if [ ! -f "ncnn/arm64-v8a/lib/libncnn.a" ]; then
        echo "Copying NCNN from YoloTouchHelp..."
        NCNN_SRC="../YoloTouchHelp/app/src/main/cpp/ncnn"
        if [ -d "$NCNN_SRC" ]; then
            cp -r "$NCNN_SRC" ncnn/
        else
            echo "ERROR: ncnn/ not found. Please copy from YoloTouchHelp first."
            exit 1
        fi
    fi

    mkdir -p build && cd build
    cmake .. \
        -DCMAKE_TOOLCHAIN_FILE="$NDK_PATH/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI=arm64-v8a \
        -DANDROID_PLATFORM=android-$API_LEVEL \
        -DCMAKE_BUILD_TYPE=Release
    make -j$(nproc)
    cd ..
    echo "Build complete: build/$BINARY_NAME ($(ls -lh build/$BINARY_NAME | awk '{print $5}'))"
fi

# ─── 推送 ────────────────────────────────────────────────────────
if $DO_PUSH; then
    echo "=== Pushing to device ==="
    adb push build/$BINARY_NAME $DEVICE_PATH
    adb shell "chmod 755 $DEVICE_PATH"
    echo "Pushed: $DEVICE_PATH"
fi

# ─── 运行 ────────────────────────────────────────────────────────
if $DO_RUN; then
    echo "=== Running ==="
    echo "Args: --model $MODEL_PATH --width $SCREEN_W --height $SCREEN_H"
    echo "Press Ctrl+C to stop."
    adb shell "su -c '$DEVICE_PATH --model $MODEL_PATH --width $SCREEN_W --height $SCREEN_H'"
fi