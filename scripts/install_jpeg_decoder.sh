#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build the reviewed JPEG series with the tested ROCK 4D/RK3576 adaptation.
set -euo pipefail
project=$(cd "$(dirname "$0")/.." && pwd)
release=$(uname -r)
cache=/var/cache/autonomous-driving-car/jpeg-decoder/$release
dtb=/boot/dtb/rockchip/rk3576-rock-4d.dtb

[[ $(id -u) == 0 ]] || { echo "Run this installer as root"; exit 1; }
tr '\0' '\n' < /proc/device-tree/compatible | grep -qx 'radxa,rock-4d' || {
    echo "This installation is validated only for the Radxa ROCK 4D"; exit 1;
}
[[ -d /lib/modules/$release/build && -f $dtb ]] || { echo "Matching headers/DTB missing"; exit 1; }
for command in make gcc dtc fdtget fdtoverlay modprobe modinfo depmod; do
    command -v "$command" >/dev/null || { echo "Missing $command"; exit 1; }
done
mkdir -p "$cache/driver" "$cache/overlay"
cp "$project"/kernel/rockchip-jpegd/{Makefile,rkjpegd.c,rkjpegd-vdpu720-regs.h} "$cache/driver/"
cp "$project"/kernel/live-overlay/{Makefile,rk3576-jpeg-overlay.c} "$cache/overlay/"
make -C "/lib/modules/$release/build" M="$cache/driver" CONFIG_VIDEO_ROCKCHIP_JPEGD=m -j8 modules
make -C "/lib/modules/$release/build" M="$cache/overlay" -j8 modules
[[ $(modinfo -F vermagic "$cache/driver/rockchip-jpegd.ko") == "$release "* ]] || exit 1

make_overlay() {
    local base=$1 output=$2 cru power gic
    cru=$(fdtget -t x "$base" /soc/clock-controller@27200000 phandle)
    power=$(fdtget -t x "$base" /soc/power-management@27380000/power-controller phandle)
    gic=$(fdtget -t x "$base" / interrupt-parent)
    sed -e "s/@CRU@/0x$cru/g" -e "s/@POWER@/0x$power/g" -e "s/@GIC@/0x$gic/g" \
        "$project/kernel/live-overlay/rk3576-jpeg.dts.in" > "$output.dts"
    # No symbol export: this board's base DTB has no __symbols__ table.
    dtc -I dts -O dtb -o "$output" "$output.dts" 2> "$output.log"
}
make_overlay "$dtb" "$cache/boot.dtbo"
fdtoverlay -i "$dtb" -o "$cache/rock-4d-jpeg.dtb" "$cache/boot.dtbo"
[[ $(fdtget "$cache/rock-4d-jpeg.dtb" /soc/jpegd@27910000 compatible) == rockchip,rk3576-jpegd ]] || exit 1

backup=$(mktemp -d /var/backups/car-jpeg-decoder-XXXXXX)
cp -a "$dtb" "$backup/board.dtb"
printf '%s\n' "$dtb" > "$backup/dtb-path"
mkdir -p "/lib/modules/$release/updates" /lib/firmware/autonomous-driving-car
for module in "$cache/driver/rockchip-jpegd.ko" "$cache/overlay/rk3576-jpeg-overlay.ko"; do
    name=$(basename "$module")
    [[ ! -f /lib/modules/$release/updates/$name ]] || cp -a "/lib/modules/$release/updates/$name" "$backup/"
    install -m 644 "$module" "/lib/modules/$release/updates/$name"
done
depmod -a "$release"
mkdir -p /etc/modprobe.d
# Keep decoder registration from taking a USB camera's low video node number.
printf 'options rockchip_jpegd video_nr=32\n' > /etc/modprobe.d/autonomous-driving-car-jpeg.conf
# The VEPU510 RFC remains available for explicit tests, but fails normal GOPs.
printf 'blacklist rockchip_vepu510\n' > /etc/modprobe.d/autonomous-driving-car-experimental-encoder.conf
modprobe v4l2-jpeg
modprobe v4l2-mem2mem
modprobe videobuf2-dma-contig
if [[ ! -d /sys/firmware/devicetree/base/soc/jpegd@27910000 ]]; then
    dtc -I fs -O dtb -o "$cache/live.dtb" /sys/firmware/devicetree/base 2> "$cache/live-dtc.log"
    make_overlay "$cache/live.dtb" "$cache/live.dtbo"
    install -m 644 "$cache/live.dtbo" /lib/firmware/autonomous-driving-car/rk3576-jpeg.dtbo
    modprobe rk3576-jpeg-overlay
fi
modprobe rockchip-jpegd
found=0
for name in /sys/class/video4linux/video*/name; do
    [[ $(cat "$name") != rockchip-jpegd ]] || found=1
done
[[ $found == 1 ]] || { echo "JPEG probe failed; boot DTB remains unchanged. Backup: $backup"; exit 1; }
install -m 644 "$cache/rock-4d-jpeg.dtb" "$dtb.new"
mv -f "$dtb.new" "$dtb"
printf 'rockchip-jpegd\n' > /etc/modules-load.d/autonomous-driving-car-jpeg.conf
echo "JPEG decoder installed for $release; DTB persists across reboot. Backup: $backup"
