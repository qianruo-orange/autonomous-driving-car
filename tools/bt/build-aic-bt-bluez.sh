#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# 把板载 AIC8800 蓝牙驱动重编为 BlueZ（标准 HCI）模式并安装。
#
# 厂商默认编的是 Android bluedroid 模式：只暴露 /dev/aicbt_dev，不注册 hci，
# BlueZ 看不到设备。同一份源码把 CONFIG_BLUEDROID 改成 0 就走标准 HCI 分支。
#
# 用法：
#   sudo ./build-aic-bt-bluez.sh
#   AICBT_SRC=/path/to/aic_btusb sudo ./build-aic-bt-bluez.sh
set -e

KVER=$(uname -r)
KDIR="/lib/modules/$KVER/build"
SRC="${AICBT_SRC:-$HOME/kbuild/kernels/7.4-rc1/preparation/bundle/aic-bluetooth/aic_btusb}"
DEST="/lib/modules/$KVER/updates/aic_btusb_bluez.ko"

if [ "$(id -u)" -ne 0 ]; then
    echo "[ERROR] must run as root" >&2
    exit 1
fi
if [ ! -d "$KDIR" ]; then
    echo "[ERROR] kernel build tree not found: $KDIR" >&2
    echo "        安装 linux-headers-$KVER 后再试" >&2
    exit 1
fi
if [ ! -f "$SRC/aic_btusb.c" ]; then
    echo "[ERROR] driver source not found: $SRC" >&2
    echo "        用 AICBT_SRC 指定 aic_btusb 源码目录" >&2
    exit 1
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
cp -a "$SRC"/. "$WORK"/
chmod -R u+w "$WORK"

echo "[BT] switching aic_btusb to BlueZ (hci) mode"
sed -i 's/^#define CONFIG_BLUEDROID[[:space:]]\+1/#define CONFIG_BLUEDROID        0/' "$WORK/aic_btusb.h"
if ! grep -q '^#define CONFIG_BLUEDROID[[:space:]]\+0' "$WORK/aic_btusb.h"; then
    echo "[ERROR] failed to patch CONFIG_BLUEDROID" >&2
    exit 1
fi

echo "[BT] building against $KDIR"
make -C "$WORK" KDIR="$KDIR" ARCH=arm64 CONFIG_PLATFORM_UBUNTU=n modules

echo "[BT] installing $DEST"
install -D -m 644 "$WORK/aic_btusb.ko" "$DEST"
printf 'install aic_btusb /sbin/insmod %s\n' "$DEST" > /etc/modprobe.d/aicbt-bluez.conf
printf 'aic_btusb\n' > /etc/modules-load.d/aicbt-bluez.conf
depmod -a

if ! dpkg -s bluez >/dev/null 2>&1; then
    echo "[BT] installing bluez"
    DEBIAN_FRONTEND=noninteractive apt-get install -y bluez
fi
systemctl enable --now bluetooth

echo "[BT] reload module: rmmod aic_btusb && modprobe aic_btusb"
rmmod aic_btusb 2>/dev/null || true
modprobe aic_btusb
sleep 2
if [ -e /sys/class/bluetooth/hci0 ]; then
    echo "[BT] OK: hci0 present"
    bluetoothctl show | head -6 || true
else
    echo "[BT] WARN: hci0 not found; check 'journalctl -k | grep -i aic'" >&2
    exit 1
fi
