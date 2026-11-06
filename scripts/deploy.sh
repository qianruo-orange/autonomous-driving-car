#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -e

PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
APP_NAME="autonomous-driving-car"
DEPLOY_DIR="/opt/$APP_NAME"
BINARY="$PROJECT_DIR/bin/$APP_NAME"
SERVICE_SRC="$PROJECT_DIR/$APP_NAME.service"
SERVICE_NAME="$APP_NAME"

# 平台固定为 RK3576（本工程唯一目标）
PLATFORM=rk3576

usage() {
    echo "Usage: $0 [-i | -u | -r]"
    echo "  -i   Install: deploy and start service (default)"
    echo "  -u   Uninstall: stop service, remove files"
    echo "  -r   Reinstall: uninstall then install"
    echo "  -h   Show this help"
    exit 0
}

prepare_realtime_cgroup() {
    # cgroup v2 has no per-group RT budget controls. With RT group scheduling
    # enabled, its CPU controller otherwise leaves service groups unable to run FIFO.
    [ -f /sys/fs/cgroup/cgroup.controllers ] || return 0
    local config enabled=1 option disabled controllers
    local -a bootargs
    if [ -r /proc/config.gz ]; then
        config=$(gzip -cd /proc/config.gz) || return 1
    elif [ -r "/boot/config-$(uname -r)" ]; then
        config=$(cat "/boot/config-$(uname -r)") || return 1
    else
        echo "[ERROR] cannot read the running kernel configuration; keeping the existing service running"
        return 1
    fi
    config=$'\n'"$config"$'\n'
    [[ $config == *$'\nCONFIG_RT_GROUP_SCHED=y\n'* ]] || return 0
    [[ $config != *$'\nCONFIG_RT_GROUP_SCHED_DEFAULT_DISABLED=y\n'* ]] || enabled=0
    read -r -a bootargs < /proc/cmdline
    for option in "${bootargs[@]}"; do
        case "$option" in
            rt_group_sched=0) enabled=0 ;;
            rt_group_sched=1) enabled=1 ;;
        esac
    done
    [ "$enabled" = 1 ] || return 0
    controllers=$(cat /sys/fs/cgroup/cgroup.subtree_control) || return 1

    echo "[RT] disabling cgroup v2 CPU group quotas/weights for FIFO compatibility"
    mkdir -p /etc/systemd/system/-.slice.d
    cat > /etc/systemd/system/-.slice.d/90-autonomous-driving-car-rt.conf <<'EOF'
# Managed by autonomous-driving-car/scripts/deploy.sh.
[Slice]
DisableControllers=cpu
EOF
    systemctl daemon-reload
    if [[ " $controllers " == *" cpu "* ]]; then
        disabled=$(systemctl show --value --property=DisableControllers -- -.slice)
        [[ " $disabled " == *" cpu "* ]] || disabled="${disabled:+$disabled }cpu"
        systemctl set-property --runtime -- -.slice "DisableControllers=$disabled"
    fi
    controllers=$(cat /sys/fs/cgroup/cgroup.subtree_control) || return 1
    if [[ " $controllers " == *" cpu "* ]]; then
        echo "[ERROR] CPU controller remains enabled; keeping the existing service running"
        return 1
    fi
}

do_uninstall() {
    echo "=== uninstall $APP_NAME ==="
    
    echo "[STOP] $SERVICE_NAME"
    systemctl stop "$SERVICE_NAME" 2>/dev/null || true
    
    echo "[DISABLE] $SERVICE_NAME"
    systemctl disable "$SERVICE_NAME" 2>/dev/null || true
    
    echo "[REMOVE] systemd service file"
    rm -f /etc/systemd/system/"$SERVICE_NAME".service
    systemctl daemon-reload || true
    
    echo "[REMOVE] $DEPLOY_DIR"
    rm -rf "$DEPLOY_DIR"
    
    echo "=== uninstall complete ==="
}

# RGA 预处理依赖 librga.so（运行时动态加载）与 /dev/rga 设备。
# 全硬件通路要求 RGA：库/设备缺失时推理池不会启动，部署阶段先装好并提示。
install_rga_library() {
    local lib=/usr/local/lib/librga.so
    local inc=/usr/local/include/rga
    if [ -f "$lib" ]; then
        echo "[CHECK] librga: $lib"
    else
        local src="${LIBRGA_SOURCE:-}"
        if [ -z "$src" ]; then
            for candidate in \
                /home/qianruo/kbuild/librga/libs/Linux/gcc-aarch64/librga.so \
                "$PROJECT_DIR/vendor/librga/librga.so"; do
                [ -f "$candidate" ] && { src=$candidate; break; }
            done
        fi
        if [ -n "$src" ] && [ -f "$src" ]; then
            echo "[INSTALL] librga: $src -> $lib"
            install -D -m 755 "$src" "$lib"
            local srcdir
            srcdir=$(dirname "$src")
            if [ -d "$srcdir/../../../include" ] && [ ! -d "$inc" ]; then
                mkdir -p "$inc"
                cp "$srcdir/../../../include/"*.h "$inc/" 2>/dev/null || true
            fi
            /sbin/ldconfig
        else
            echo "[WARN] librga.so 未安装且未找到源文件（可用 LIBRGA_SOURCE 指定）"
            echo "        全硬件通路要求 RGA：缺库时推理池不会启动；安装方式见 README「Preprocessing on the RGA」"
        fi
    fi
    if [ -e /dev/rga ]; then
        echo "[CHECK] /dev/rga 存在（RGA 驱动已加载）"
    else
        echo "[WARN] /dev/rga 不存在：内核未带 RGA 驱动；全硬件通路要求 RGA，推理池不会启动"
    fi
}

do_install() {
    echo "=== deploy $APP_NAME ==="

    if [ "$(id -u)" -ne 0 ]; then
        echo "[ERROR] must run as root"
        exit 1
    fi

    if [ ! -f "$BINARY" ]; then
        echo "[ERROR] binary not found: $BINARY"
        echo "        run scripts/build.sh first"
        exit 1
    fi

    # Check for independently changed models before interrupting the service.
    bash "$PROJECT_DIR/scripts/model_sync.sh" reconcile \
        --project "$PROJECT_DIR" --deployment "$DEPLOY_DIR"

    prepare_realtime_cgroup

    echo "[STOP] existing service (if running)"
    systemctl stop "$SERVICE_NAME" 2>/dev/null || true

    echo "[INSTALL] configuration and canonical model (preserving uploaded model changes)"
    if ! bash "$PROJECT_DIR/scripts/model_sync.sh" install-config \
        --project "$PROJECT_DIR" --deployment "$DEPLOY_DIR"; then
        echo "[ERROR] configuration/model synchronization failed; restarting the previous service"
        systemctl start "$SERVICE_NAME" 2>/dev/null || true
        return 1
    fi

    # 清理旧 WebRTC 边车（mediamtx + fifo pusher）：直播是 multipart MJPEG 直供，
    # 不再需要外部 RTSP 中转。从旧版本升级时留着会白占 VPU/内存并抢 8554/8889 端口
    for u in mediamtx rockchip-smart-gateway-webrtc; do
        if [ -f /etc/systemd/system/"$u".service ]; then
            echo "[CLEAN] legacy webrtc unit: $u"
            systemctl stop "$u" 2>/dev/null || true
            systemctl disable "$u" 2>/dev/null || true
            rm -f /etc/systemd/system/"$u".service
        fi
    done

    echo "[INSTALL] $BINARY -> $DEPLOY_DIR/bin/"
    mkdir -p "$DEPLOY_DIR/bin"
    cp "$BINARY" "$DEPLOY_DIR/bin/$APP_NAME"
    chmod 755 "$DEPLOY_DIR/bin/$APP_NAME"

    echo "[INSTALL] html/"
    rm -rf "$DEPLOY_DIR/html"
    cp -r "$PROJECT_DIR/html" "$DEPLOY_DIR/html"

    echo "[CLEAN] retired tooling"
    if [ -d "$DEPLOY_DIR/scripts" ]; then
        find "$DEPLOY_DIR/scripts" -type f \( -name '*.py' -o -name '*.pyc' -o -name '*.pyo' \) -delete
        find "$DEPLOY_DIR/scripts" -depth -type d -empty -delete
    fi

    echo "[INSTALL] recordings/{logs,videos,results}/"
    mkdir -p "$DEPLOY_DIR/recordings/logs" "$DEPLOY_DIR/recordings/videos" "$DEPLOY_DIR/recordings/results"

    echo "[INSTALL] Codex chat bridge"
    mkdir -p "$DEPLOY_DIR/.codex/out" "$DEPLOY_DIR/.codex/inbox" \
             "$DEPLOY_DIR/recordings/codex"
    cp "$PROJECT_DIR/scripts/codex_bridge.py" "$DEPLOY_DIR/bin/codex_bridge.py"
    chmod 755 "$DEPLOY_DIR/bin/codex_bridge.py"
    if id -u qianruo >/dev/null 2>&1; then
        chown -R qianruo:qianruo "$DEPLOY_DIR/.codex" "$DEPLOY_DIR/recordings/codex"
    fi
    chmod 700 "$DEPLOY_DIR/.codex"

    echo "[INSTALL] network proxy editor"
    mkdir -p "$DEPLOY_DIR/recordings/proxy-out" "$DEPLOY_DIR/recordings/proxy-backups"
    cp "$PROJECT_DIR/scripts/proxy_config.py" "$DEPLOY_DIR/bin/proxy_config.py"
    chmod 755 "$DEPLOY_DIR/bin/proxy_config.py"
    if [ ! -f /etc/proxy-client.conf ]; then
        cat > /etc/proxy-client.conf <<'PROXYEOF'
# 开发板网络代理配置文件
# 由 Web 配置界面「网络代理」管理；保存并应用后整机流量走此代理
PROXY_HOST="127.0.0.1"
PROXY_PORT="7890"
PROXY_SCHEME="http"
NO_PROXY="localhost,127.0.0.1,::1"
PROXYEOF
        chmod 644 /etc/proxy-client.conf
        echo "[INSTALL] created default /etc/proxy-client.conf"
    fi

    echo "[INSTALL] systemd service"
    cp "$SERVICE_SRC" /etc/systemd/system/"$SERVICE_NAME".service
    systemctl daemon-reload
    systemctl enable "$SERVICE_NAME"

    echo "[CHECK] RGA preprocessing dependency"
    install_rga_library

    echo "[START] $SERVICE_NAME"
    systemctl start "$SERVICE_NAME"

    echo "=== deploy complete ==="

    echo ""
    systemctl status "$SERVICE_NAME" --no-pager -l
}

ACTION="${1:--i}"

case "$ACTION" in
    -i) do_install ;;
    -u) do_uninstall ;;
    -r) prepare_realtime_cgroup; do_uninstall; do_install ;;
    -h|--help) usage ;;
    *)  echo "[ERROR] unknown option: $ACTION"; usage ;;
esac
