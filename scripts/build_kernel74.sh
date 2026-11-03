#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Build the prepared ROCK 4D tree and its matching external modules only.
set -euo pipefail
export PATH="$PATH:/usr/sbin:/sbin"

usage() {
    printf 'Usage: %s [source-directory [status-directory]]\n' "$0"
}
if [[ ${1:-} == -h || ${1:-} == --help ]]; then
    usage
    exit 0
fi
[[ $# -le 2 ]] || { usage >&2; exit 2; }

source_tree=$(realpath -e "${1:-/home/qianruo/kbuild/kernels/7.4-rc1/source}")
status_dir=${2:-/home/qianruo/kbuild/kernels/7.4-rc1/records}
package_version=7.4~rc1-npu-rt-2
release=7.4.0-rc1-npu-rt
jobs=24
drivers=$source_tree/.rock4d/drivers
npu=$drivers/kiln-rknpu-0.9.8
aic=$drivers/aic8800-usb-5.0-kiln/USB/driver_fw/drivers

for command in make gcc dpkg-buildpackage modinfo sha256sum flock; do
    command -v "$command" >/dev/null || { printf 'Missing command: %s\n' "$command" >&2; exit 1; }
done
for path in "$source_tree/.config" "$source_tree/.rock4d/source-verification.json" \
            "$npu/Kbuild" "$aic/aic8800/Makefile" "$aic/aic_btusb/Makefile"; do
    [[ -f $path ]] || { printf 'Missing prepared input: %s\n' "$path" >&2; exit 1; }
done
exec 9> "$source_tree/.rock4d/build.lock"
flock -n 9 || { printf 'This source tree already has an active build.\n' >&2; exit 1; }
mkdir -p "$status_dir"
status_dir=$(realpath -e "$status_dir")

# Do not inherit an earlier API-only check's relaxed symbol handling.
unset KBUILD_MODPOST_WARN KBUILD_EXTRA_SYMBOLS
current_stage=preflight
finish() {
    local result=$?
    printf '%s\n' "$result" > "$status_dir/build-exit-code.txt"
    date --iso-8601=seconds > "$status_dir/build-finished.txt"
    [[ $result != 0 ]] || current_stage=complete
    printf '%s\n' "$current_stage" > "$status_dir/build-stage.txt"
    printf '%s\t%s\t%s\n' "$(date --iso-8601=seconds)" "$result" "$current_stage" >> "$status_dir/build-runs.tsv"
}
trap finish EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
rm -f "$status_dir/build-exit-code.txt" "$status_dir/build-finished.txt"
date --iso-8601=seconds > "$status_dir/build-started.txt"
printf 'source=%s\nrelease=%s\npackage_version=%s\njobs=%s\n' \
       "$source_tree" "$release" "$package_version" "$jobs" > "$status_dir/build-inputs.txt"
[[ $(make -s -C "$source_tree" kernelrelease) == "$release" ]] || {
    printf 'Prepared kernel release does not match %s.\n' "$release" >&2; exit 1;
}
for setting in CONFIG_ARM64=y CONFIG_PREEMPT_RT=y CONFIG_VIDEO_ROCKCHIP_JPEGD=m; do
    grep -qx "$setting" "$source_tree/.config" || {
        printf 'Required configuration missing: %s\n' "$setting" >&2; exit 1;
    }
done

build_stage() {
    local name=$1 result
    shift
    current_stage=$name
    printf '%s\n' "$name" > "$status_dir/build-stage.txt"
    date --iso-8601=seconds > "$status_dir/$name-started.txt"
    printf 'Building %s; log: %s/%s.log\n' "$name" "$status_dir" "$name"
    if "$@" > "$status_dir/$name.log" 2>&1; then
        result=0
    else
        result=$?
    fi
    printf '%s\n' "$result" > "$status_dir/$name-exit-code.txt"
    date --iso-8601=seconds > "$status_dir/$name-finished.txt"
    return "$result"
}

build_stage kernel make -C "$source_tree" -j"$jobs" \
    KDEB_PKGVERSION="$package_version" bindeb-pkg
[[ -s $source_tree/Module.symvers && -s $source_tree/vmlinux ]] || {
    printf 'Complete kernel symbols or vmlinux are missing.\n' >&2; exit 1;
}
build_external() {
    # Discard API-preflight outputs, which may lack final symbol versions.
    make -C "$source_tree" M="$1" clean
    make -C "$source_tree" M="$1" -j"$jobs" modules
}
build_stage rknpu build_external "$npu"
build_stage wifi build_external "$aic/aic8800"
build_stage bluetooth build_external "$aic/aic_btusb"

current_stage=verify
modules=("$npu/rknpu.ko" "$aic/aic8800/aic_load_fw/aic_load_fw.ko"
         "$aic/aic8800/aic8800_fdrv/aic8800_fdrv.ko" "$aic/aic_btusb/aic_btusb.ko")
for module in "${modules[@]}"; do
    [[ $(modinfo -F vermagic "$module") == "$release "* ]] || {
        printf 'Module ABI mismatch: %s\n' "$module" >&2; exit 1;
    }
done
package_parent=$(dirname "$source_tree")
package_dir=$package_parent/packages
mkdir -p "$package_dir"
for artifact in "linux-image-${release}_${package_version}_arm64.deb" \
                "linux-image-${release}-dbg_${package_version}_arm64.deb" \
                "linux-headers-${release}_${package_version}_arm64.deb" \
                "linux-libc-dev_${package_version}_arm64.deb" \
                "linux-upstream_${package_version}_arm64.buildinfo" \
                "linux-upstream_${package_version}_arm64.changes"; do
    [[ ! -f $package_parent/$artifact ]] || mv -f -- "$package_parent/$artifact" "$package_dir/"
done
packages=("$package_dir/linux-image-${release}_${package_version}_arm64.deb"
          "$package_dir/linux-headers-${release}_${package_version}_arm64.deb")
sha256sum "${packages[@]}" "${modules[@]}" > "$status_dir/artifacts.sha256"
printf 'Kernel packages and four matching external modules built. No installation or reboot performed.\n'
