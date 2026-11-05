#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -e

PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$PROJECT_DIR/build"
BIN_DIR="$PROJECT_DIR/bin"
BIN_NAME="autonomous-driving-car"
BIN_PATH="$BIN_DIR/$BIN_NAME"
CMAKE_DIR="$PROJECT_DIR/cmake"
LOGS_DIR="$PROJECT_DIR/logs"

usage() {
    echo "Usage: $0 [-D|--debug] [-R|--release] [-C|--clean]"
    echo "  -D, --debug     build with debug output to terminal"
    echo "  -R, --release   build for production (default)"
    echo "  -C, --clean     remove build directory, bin directory, cmake directory, generated CMake artifacts, and clear logs"
    echo "  平台固定为 rk3576（ROCK 4D），不再提供 -P。"
    echo "                  默认从 /proc/device-tree/compatible 自动辨识当前板卡芯片"
    echo "Prepare a board-compatible config/yolo26.rknn before building."
    exit 1
}

# 平台固定为 RK3576（本工程唯一目标）
PLATFORM=rk3576

# Reconcile uploaded models before validating the single runtime RKNN path.
# Model preparation is external to this C/C++ build.
sync_model() {
    local plat="$1"
    local runtime_model="$PROJECT_DIR/config/yolo26.rknn"
    bash "$PROJECT_DIR/scripts/model_sync.sh" reconcile \
        --project "$PROJECT_DIR" --deployment "${CAR_MODEL_DEPLOYMENT:-/opt/$BIN_NAME}"
    if [ ! -f "$runtime_model" ]; then
        echo "[MODEL] missing config/yolo26.rknn; provide a compatible $PLATFORM RKNN model before building"
        return 1
    fi
    bash "$PROJECT_DIR/scripts/model_sync.sh" validate --model "$runtime_model"
    echo "[MODEL] using config/yolo26.rknn ($PLATFORM)"
}

print_version_info() {
    local version_header="$PROJECT_DIR/include/core/version.h"

    if [ -f "$version_header" ]; then
        local app_version app_build_type
        app_version=$(grep '^#define APP_VERSION ' "$version_header" | head -n 1 | cut -d'"' -f2)
        app_build_type=$(grep '^#define APP_BUILD_TYPE ' "$version_header" | head -n 1 | cut -d'"' -f2)
        echo "[BUILD] version=${app_version:-unknown} build_type=${app_build_type:-unknown}"
    else
        echo "[BUILD] version header not generated yet"
    fi
}

clean_build() {
    echo "[CLEAN] removing $BUILD_DIR"
    rm -rf "$BUILD_DIR"

    if [ -d "$BIN_DIR" ]; then
        echo "[CLEAN] removing $BIN_DIR"
        rm -rf "$BIN_DIR"
    else
        echo "[CLEAN] no $BIN_DIR to remove"
    fi

    for artifact in CMakeCache.txt CMakeFiles cmake_install.cmake Makefile; do
        if [ -e "$PROJECT_DIR/$artifact" ]; then
            echo "[CLEAN] removing $PROJECT_DIR/$artifact"
            rm -rf "$PROJECT_DIR/$artifact"
        else
            echo "[CLEAN] no $PROJECT_DIR/$artifact to remove"
        fi
    done

    if [ -d "$CMAKE_DIR" ]; then
        echo "[CLEAN] removing $CMAKE_DIR"
        rm -rf "$CMAKE_DIR"
    else
        echo "[CLEAN] no $CMAKE_DIR to remove"
    fi

    if [ -f "$PROJECT_DIR/include/core/version.h" ]; then
        echo "[CLEAN] removing $PROJECT_DIR/include/core/version.h"
        rm -f "$PROJECT_DIR/include/core/version.h"
    else
        echo "[CLEAN] no version header to remove"
    fi

    echo "[CLEAN] removing $LOGS_DIR"
    rm -rf "$LOGS_DIR"
    mkdir -p "$LOGS_DIR"

    echo "[CLEAN] done"
}

# 构建依赖检查：构建前校验工具链与开发库，缺失时给出安装提示并中止，
# 避免在 cmake / make 阶段才暴露晦涩的报错。
check_deps() {
    echo "[DEPS] checking build dependencies..."
    local missing_cmds=() missing_pkgs=()

    # 工具链
    local cmd
    for cmd in cmake make gcc g++ tar zip; do
        command -v "$cmd" >/dev/null 2>&1 || missing_cmds+=("$cmd")
    done

    # 开发库：每个条目给出若干候选包名，任一已安装即视为满足
    local desc
    check_pkg() {
        desc="$1"; shift
        local p ok=0
        for p in "$@"; do
            if dpkg -s "$p" >/dev/null 2>&1; then ok=1; break; fi
        done
        [ "$ok" = 1 ] || missing_pkgs+=("$desc")
    }

    check_pkg "libsystemd-dev" libsystemd-dev
    check_pkg "libnl-3-dev" libnl-3-dev
    check_pkg "libnl-route-3-dev" libnl-route-3-dev
    check_pkg "OpenCV 开发库 (libopencv-dev)" libopencv-dev

    # RKNN NPU 运行时（板载 Rockchip SDK 安装，非 apt 包）
    local rknn_ok=0
    for r in /lib/librknnrt.so /usr/lib/librknnrt.so /usr/lib/aarch64-linux-gnu/librknnrt.so; do
        [ -f "$r" ] && { rknn_ok=1; break; }
    done
    [ "$rknn_ok" = 1 ] || missing_pkgs+=("librknnrt.so (RKNN NPU 运行时)")

    # MPP 是硬件 H.264 编码的必需依赖，可由独立内核仓库安装。
    local mpp_headers_ok=0 mpp_library_ok=0 path
    for path in /usr/local/include/rockchip/rk_mpi.h /usr/include/rockchip/rk_mpi.h; do
        [ -f "$path" ] && { mpp_headers_ok=1; break; }
    done
    for path in /usr/local/lib/librockchip_mpp.so /usr/lib/librockchip_mpp.so \
                /usr/lib/aarch64-linux-gnu/librockchip_mpp.so; do
        [ -f "$path" ] && { mpp_library_ok=1; break; }
    done
    [ "$mpp_headers_ok" = 1 ] || missing_pkgs+=("Rockchip MPP 开发头文件 (rockchip/rk_mpi.h)")
    [ "$mpp_library_ok" = 1 ] || missing_pkgs+=("Rockchip MPP 共享库 (librockchip_mpp.so)")

    if [ ${#missing_cmds[@]} -gt 0 ] || [ ${#missing_pkgs[@]} -gt 0 ]; then
        echo "[DEPS] 缺少以下依赖："
        for c in "${missing_cmds[@]}"; do echo "  - 命令: $c"; done
        for p in "${missing_pkgs[@]}"; do echo "  - 包: $p"; done
        echo "[DEPS] 安装命令（Debian/Ubuntu）："
        echo "  sudo apt-get install -y build-essential cmake libsystemd-dev \\"
        echo "      libnl-3-dev libnl-route-3-dev libopencv-dev"
        echo "[DEPS] librknnrt.so 需从 Rockchip 官方 SDK 安装到 /lib 或 /usr/lib。"
        echo "[DEPS] MPP 安装脚本位于独立内核仓库："
        echo "  https://github.com/qianruo-orange/rock-4d-npu-kernel/blob/main/scripts/install_mpp_userspace.sh"
        exit 1
    fi
    echo "[DEPS] all required dependencies satisfied"
}

# ---------- 参数解析（支持组合使用） ----------
BUILD_MODE="release"
while [ $# -gt 0 ]; do
    case "$1" in
        -D|--debug)    BUILD_MODE="debug" ;;
        -R|--release)  BUILD_MODE="release" ;;
        -C|--clean)    BUILD_MODE="clean" ;;
        *)             usage ;;
    esac
    shift
done

# 清理模式不构建
if [ "$BUILD_MODE" = "clean" ]; then
    clean_build
    exit 0
fi

check_deps

echo "[MODEL] 平台: $PLATFORM"

# 模型同步（失败则中止编译：模型是启动必要项）
sync_model "$PLATFORM"

case "$BUILD_MODE" in
    debug)
        echo "=== build $BIN_NAME ==="
        echo "[BUILD] DEBUG mode (-O0 -g)"
        mkdir -p "$BIN_DIR"
        cmake -S "$PROJECT_DIR" -B "$BUILD_DIR" \
            -DCMAKE_BUILD_TYPE=Debug \
            -DDEBUG=ON
        ;;
    release)
        echo "=== build $BIN_NAME ==="
        echo "[BUILD] RELEASE mode (-O3 -DNDEBUG -g)"
        mkdir -p "$BIN_DIR"
        cmake -S "$PROJECT_DIR" -B "$BUILD_DIR" \
            -DCMAKE_BUILD_TYPE=Release
        ;;
esac

make -C "$BUILD_DIR" -j"$(nproc)"
print_version_info

echo "[BUILD] done -> $BIN_PATH"
