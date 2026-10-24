#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -e

PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$PROJECT_DIR/build"
BIN_DIR="$PROJECT_DIR/bin"
BIN_NAME="rockchip-smart-gateway"
BIN_PATH="$BIN_DIR/$BIN_NAME"
CMAKE_DIR="$PROJECT_DIR/cmake"
LOGS_DIR="$PROJECT_DIR/logs"

usage() {
    echo "Usage: $0 [-D|--debug] [-R|--release] [-C|--clean] [-P|--platform <platform>]"
    echo "  -D, --debug     build with debug output to terminal"
    echo "  -R, --release   build for production (default)"
    echo "  -C, --clean     remove build directory, bin directory, cmake directory, generated CMake artifacts, and clear logs"
    echo "  -P, --platform  目标 RKNPU 平台（rk3588/rk3576/rk3568/rk3566/rk3562 等）"
    echo "                  默认从 /proc/device-tree/compatible 自动辨识当前板卡芯片"
    echo ""
    echo "编译前自动同步模型：部署 config/models/<平台>/yolo26.rknn 到 config/，"
    echo "缺失或平台不匹配时调用 ultralytics 官方 CLI 在板端自动转换"
    echo "（yolo export model=<权重> format=rknn name=<平台>，权重缺失自动下载，"
    echo "  可用环境变量 YOLO_WEIGHTS 指定权重，默认 yolo26n.pt）。"
    exit 1
}

# 平台自动辨识：从设备树 compatible 提取 Rockchip RKNPU 芯片型号（rkXXXX / rvXXXX）
detect_platform() {
    local compat plat
    compat=$(tr -d '\0' < /proc/device-tree/compatible 2>/dev/null) || return 1
    plat=$(printf '%s' "$compat" | grep -oE 'rockchip,(rk|rv)[0-9]+[a-z]*' | head -1 | cut -d, -f2)
    [ -n "$plat" ] || return 1
    printf '%s' "$plat"
}

# 模型同步：部署匹配平台的 RKNN 模型；不匹配时直接调用 ultralytics 官方转换
# CLI（yolo export ... format=rknn name=<平台>）在板端转换——rknn-toolkit2 >= 2.3.0
# 提供 aarch64 wheel。权重缺失时 ultralytics 自动下载最新官方权重，下载后缓存到
# config/models/weights/ 复用；转换产物存入 config/models/<平台>/ 供后续编译复用。
sync_model() {
    local plat="$1"
    local models_dir="$PROJECT_DIR/config/models/$plat"
    local runtime_model="$PROJECT_DIR/config/yolo26.rknn"
    local weights="${YOLO_WEIGHTS:-yolo26n.pt}"

    if [ -f "$models_dir/yolo26.rknn" ]; then
        cp -f "$models_dir/yolo26.rknn" "$runtime_model"
        echo "[MODEL] 已部署 $plat 平台模型 -> config/yolo26.rknn"
        return 0
    fi

    if [ -f "$runtime_model" ]; then
        local cur
        cur=$(grep -aoiE 'r[kv][0-9]{4}[a-b]?' "$runtime_model" | head -1 | tr 'A-Z' 'a-z')
        if [ "$cur" = "$plat" ]; then
            echo "[MODEL] config/yolo26.rknn 平台匹配 ($plat)"
            return 0
        fi
        echo "[MODEL] config/yolo26.rknn 目标平台 ${cur:-未知} 与当前平台 $plat 不匹配，尝试板端自动转换..."
    else
        echo "[MODEL] 缺少运行模型 config/yolo26.rknn，尝试板端自动转换（目标平台 $plat）..."
    fi

    # 定位 ultralytics 官方 CLI（yolo）：项目 .venv（rknn-toolkit2 需 Python 3.12，板端
    # 用 uv 建独立转换环境）→ 系统 PATH → ~/.local/bin
    local yolo_bin wbase weights_cache workdir rknn_file
    yolo_bin="$PROJECT_DIR/.venv/bin/yolo"
    [ -x "$yolo_bin" ] || yolo_bin=$(command -v yolo || true)
    [ -z "$yolo_bin" ] && [ -x "$HOME/.local/bin/yolo" ] && yolo_bin="$HOME/.local/bin/yolo"
    if [ ! -x "$yolo_bin" ]; then
        echo "[MODEL] 未找到 ultralytics (yolo CLI)，请先初始化板端转换环境："
        echo "    sudo apt-get install -y python3-pip && pip3 install --break-system-packages uv"
        echo "    uv venv .venv --python 3.12 && uv pip install --python .venv/bin/python ultralytics"
        echo "    # rknn-toolkit2 (aarch64, >= 2.3.0) wheel 从 airockchip/rknn-toolkit2 仓库安装"
        echo "    详见 README「RKNN 转换工作流」章节"
        return 1
    fi

    # 权重缓存：config/models/weights/<权重名>（ultralytics 权重缺失时会在工作目录自动下载）
    wbase=$(basename "$weights")
    weights_cache="$PROJECT_DIR/config/models/weights/$wbase"
    mkdir -p "$models_dir" "$PROJECT_DIR/config/models/weights"

    workdir=$(mktemp -d /tmp/yolo_rknn_XXXXXX)
    [ -f "$weights_cache" ] && cp -f "$weights_cache" "$workdir/"

    echo "[MODEL] 调用 ultralytics 官方导出: yolo export model=$wbase format=rknn name=$plat"
    if (cd "$workdir" && "$yolo_bin" export model="$wbase" format=rknn name="$plat"); then
        rknn_file=$(find "$workdir" -name '*.rknn' | head -1)
        if [ -n "$rknn_file" ]; then
            cp -f "$rknn_file" "$models_dir/yolo26.rknn"
            cp -f "$models_dir/yolo26.rknn" "$runtime_model"
            [ -f "$workdir/$wbase" ] && cp -f "$workdir/$wbase" "$weights_cache"
            echo "[MODEL] 板端转换完成（ultralytics -> $plat），已部署 -> config/yolo26.rknn"
            rm -rf "$workdir"
            return 0
        fi
        echo "[MODEL] ultralytics 导出成功但未找到 .rknn 产物，请检查导出日志"
    fi
    rm -rf "$workdir"
    echo "[MODEL] 自动转换失败，详见上方 ultralytics 导出日志"
    return 1
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
    for cmd in cmake make gcc g++; do
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
    check_pkg "libjpeg 开发库 (libjpeg62-turbo-dev)" libjpeg62-turbo-dev libjpeg-dev
    check_pkg "OpenCV 开发库 (libopencv-dev)" libopencv-dev

    # RKNN NPU 运行时（板载 Rockchip SDK 安装，非 apt 包）
    local rknn_ok=0
    for r in /lib/librknnrt.so /usr/lib/librknnrt.so /usr/lib/aarch64-linux-gnu/librknnrt.so; do
        [ -f "$r" ] && { rknn_ok=1; break; }
    done
    [ "$rknn_ok" = 1 ] || missing_pkgs+=("librknnrt.so (RKNN NPU 运行时)")

    if [ ${#missing_cmds[@]} -gt 0 ] || [ ${#missing_pkgs[@]} -gt 0 ]; then
        echo "[DEPS] 缺少以下依赖："
        for c in "${missing_cmds[@]}"; do echo "  - 命令: $c"; done
        for p in "${missing_pkgs[@]}"; do echo "  - 包: $p"; done
        echo "[DEPS] 安装命令（Debian/Ubuntu）："
        echo "  sudo apt-get install -y build-essential cmake libsystemd-dev \\"
        echo "      libnl-3-dev libnl-route-3-dev libjpeg62-turbo-dev libopencv-dev"
        echo "[DEPS] librknnrt.so 需从 Rockchip 官方 SDK 安装到 /lib 或 /usr/lib。"
        exit 1
    fi

    # 可选依赖：FFmpeg（h264_rkmpp 硬编码后端），缺失时自动回退 V4L2 rkvenc
    if ! dpkg -s libavcodec-dev >/dev/null 2>&1; then
        echo "[DEPS] 提示: 未安装 libavcodec-dev，FFmpeg rkmpp 后端不可用（可选，回退 V4L2）"
    fi
    echo "[DEPS] all required dependencies satisfied"
}

# ---------- 参数解析（支持组合使用） ----------
BUILD_MODE="release"
PLATFORM_OVERRIDE=""
while [ $# -gt 0 ]; do
    case "$1" in
        -D|--debug)    BUILD_MODE="debug" ;;
        -R|--release)  BUILD_MODE="release" ;;
        -C|--clean)    BUILD_MODE="clean" ;;
        -P|--platform) shift
                       [ -n "${1:-}" ] || { echo "[BUILD] 错误: -P 需要平台参数"; usage; }
                       PLATFORM_OVERRIDE="$1" ;;
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

# ---------- 平台辨识（-P 显式指定优先，否则自动辨识） ----------
PLATFORM=""
detected=$(detect_platform)
if [ -n "$PLATFORM_OVERRIDE" ]; then
    PLATFORM="$PLATFORM_OVERRIDE"
    echo "[MODEL] 使用指定平台: $PLATFORM"
elif [ -n "$detected" ]; then
    PLATFORM="$detected"
    echo "[MODEL] 自动辨识平台: $PLATFORM"
else
    echo "[MODEL] 错误: 无法自动辨识平台（读不到 /proc/device-tree/compatible），请用 -P <platform> 显式指定"
    exit 1
fi

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
        echo "[BUILD] RELEASE mode (-O3 -DNDEBUG)"
        mkdir -p "$BIN_DIR"
        cmake -S "$PROJECT_DIR" -B "$BUILD_DIR" \
            -DCMAKE_BUILD_TYPE=Release
        ;;
esac

make -C "$BUILD_DIR" -j"$(nproc)"
print_version_info

echo "[BUILD] done -> $BIN_PATH"
