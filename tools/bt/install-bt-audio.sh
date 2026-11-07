#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# 让 BlueZ 具备 A2DP / HFP 音频端点。
#
# 背景：AirPods、蓝牙耳机、音箱这类 BR/EDR 音频设备，配对完成后 BlueZ 必须
# 立刻建立 A2DP（或 HFP）连接。如果系统里没有任何音频守护进程向 BlueZ 注册
# MediaEndpoint，bluetoothd 会报
#     btd_service_connect() a2dp-sink profile connect failed ... Protocol not available
# 对端随即断开，bond 来不及落盘，表现为"配对不上"。
#
# 这里用 PulseAudio 系统模式（无桌面会话也能跑）装上 module-bluetooth-discover，
# 由它注册 SBC/AAC 等端点。
set -e

[ "$(id -u)" -eq 0 ] || { echo "[ERROR] must run as root" >&2; exit 1; }
HERE=$(cd "$(dirname "$0")" && pwd)

if ! command -v pulseaudio >/dev/null 2>&1; then
    echo "[BT-AUDIO] installing pulseaudio"
    DEBIAN_FRONTEND=noninteractive apt-get install -y pulseaudio pulseaudio-module-bluetooth
fi

if ! grep -q 'module-bluetooth-discover' /etc/pulse/system.pa 2>/dev/null; then
    echo "[BT-AUDIO] enabling bluetooth modules in /etc/pulse/system.pa"
    cp -n /etc/pulse/system.pa /etc/pulse/system.pa.bak 2>/dev/null || true
    cat >> /etc/pulse/system.pa <<'EOF'

### Bluetooth endpoint registration for BlueZ (A2DP/HFP)
.ifexists module-bluetooth-policy.so
load-module module-bluetooth-policy
.endif
.ifexists module-bluetooth-discover.so
load-module module-bluetooth-discover
.endif
EOF
fi

echo "[BT-AUDIO] installing pulseaudio-system.service"
install -m 644 "$HERE/pulseaudio-system.service" /etc/systemd/system/pulseaudio-system.service
systemctl daemon-reload
systemctl enable --now pulseaudio-system
sleep 4

echo "[BT-AUDIO] waiting for BlueZ to report audio profiles"
for i in $(seq 1 10); do
    if bluetoothctl show 2>/dev/null | grep -qiE "Audio Sink|Audio Source"; then
        bluetoothctl show | grep -iE "Audio Sink|Audio Source|Handsfree|Headset" || true
        echo "[BT-AUDIO] OK: A2DP/HFP endpoints registered"
        exit 0
    fi
    sleep 1
done

echo "[BT-AUDIO] WARN: audio UUIDs not advertised; check 'systemctl status pulseaudio-system'" >&2
echo "          and 'journalctl -u bluetooth | grep -i endpoint'" >&2
exit 1
