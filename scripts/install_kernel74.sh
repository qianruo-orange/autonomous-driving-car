#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Install the verified ROCK 4D candidate and select it for the next boot.
set -euo pipefail
export PATH="$PATH:/usr/sbin:/sbin"
source_tree=$(realpath -e "${1:-/home/qianruo/kbuild/kernels/7.4-rc1/source}")
status_dir=$(realpath -e "${2:-/home/qianruo/kbuild/kernels/7.4-rc1/records}")
release=7.4.0-rc1-npu-rt
version=7.4~rc1-npu-rt-2
aic_version=5.0-kiln-linux74
package_dir=$(dirname "$source_tree")/packages
vendor_source=$(dirname "$source_tree")/preparation/bundle/aic8800-usb-5.0-kiln
hook=/etc/initramfs/post-update.d/99-uboot
checker=/usr/local/sbin/rock4d-rt-boot-check
expected=/var/lib/rock4d-rt/expected-kernel

[[ $(id -u) == 0 ]] || { echo 'Run as root'; exit 1; }
[[ $# -le 2 ]] || exit 2
exec 9>/var/lock/rock4d-kernel-install.lock
flock -n 9 || { echo 'Another kernel installation is active'; exit 1; }
tr '\0' '\n' </proc/device-tree/compatible | grep -qx radxa,rock-4d
[[ $(cat "$status_dir/build-exit-code.txt") == 0 ]]
[[ $(cat "$status_dir/build-stage.txt") == complete ]]
[[ -f $hook && ! -e $hook.disabled && -f $checker ]]
[[ ! -e /usr/src/aic8800-usb-$aic_version ]]
[[ -d $vendor_source ]]
for name in Image uInitrd dtb; do [[ -L /boot/$name ]]; done
sha256sum --check "$status_dir/artifacts.sha256"

backup=$(mktemp -d /var/backups/rock4d-linux74-XXXXXXXX)
chmod 700 "$backup"
printf '%s\n' "$backup" > "$status_dir/install-backup-path.txt"
cp -a "$hook" "$checker" /boot/armbianEnv.txt "$backup/"
[[ ! -e $expected ]] || cp -a "$expected" "$backup/expected-kernel"
for name in Image uInitrd dtb; do cp -a "/boot/$name" "$backup/$name"; done
cp -L /boot/Image "$backup/previous-Image"
cp -L /boot/uInitrd "$backup/previous-uInitrd"
cp -L /boot/dtb/rockchip/rk3576-rock-4d.dtb "$backup/previous-board.dtb"
dkms status > "$backup/dkms-status.txt"
cat /proc/sys/kernel/random/boot_id > "$backup/boot-id"

cat > "$backup/rollback.sh" <<'ROLLBACK'
#!/bin/bash
set -euo pipefail
backup=$(cd "$(dirname "$0")" && pwd)
for name in Image uInitrd dtb; do
    ln -s "$(readlink "$backup/$name")" "/boot/.$name.rollback.$$"
    mv -Tf "/boot/.$name.rollback.$$" "/boot/$name"
done
cp -a "$backup/rock4d-rt-boot-check" /usr/local/sbin/rock4d-rt-boot-check
if [[ -f $backup/expected-kernel ]]; then
    cp -a "$backup/expected-kernel" /var/lib/rock4d-rt/expected-kernel
else
    rm -f /var/lib/rock4d-rt/expected-kernel
fi
sync
echo 'Previous boot selection restored; no reboot performed.'
ROLLBACK
chmod 700 "$backup/rollback.sh"

finish() {
    local result=$?
    set +e
    if [[ -e $hook.disabled ]]; then
        mv "$hook.disabled" "$hook" || result=1
    fi
    if [[ $result != 0 ]]; then
        if bash "$backup/rollback.sh"; then
            printf 'Installation failed; previous boot restored. Backup: %s\n' "$backup" >&2
        else
            result=1
            printf 'Installation and boot restoration failed. Backup: %s\n' "$backup" >&2
        fi
    fi
    printf '%s\n' "$result" > "$status_dir/install-exit-code.txt"
    date --iso-8601=seconds > "$status_dir/install-finished.txt"
    trap - EXIT
    exit "$result"
}
trap finish EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
date --iso-8601=seconds > "$status_dir/install-started.txt"

# run-parts ignores names containing a dot; keep the old boot intact throughout.
mv "$hook" "$hook.disabled"
dpkg -i "$package_dir/linux-headers-${release}_${version}_arm64.deb" \
        "$package_dir/linux-image-${release}_${version}_arm64.deb"
if ! dkms status -m kiln-rknpu -v 0.9.8 -k "$release" -a aarch64 | grep -q ': installed$'; then
    dkms install -m kiln-rknpu -v 0.9.8 -k "$release" -a aarch64 -j 24
fi

# Keep the old DKMS source and installed 7.3 modules for rollback.
cp -a "$vendor_source" \
      "/usr/src/aic8800-usb-$aic_version"
python3 - "/usr/src/aic8800-usb-$aic_version/dkms.conf" "$aic_version" <<'PY'
from pathlib import Path
import sys
p=Path(sys.argv[1]);s=p.read_text()
assert 'PACKAGE_VERSION="5.0-kiln"' in s and 'AUTOINSTALL=no' in s
s=s.replace('PACKAGE_VERSION="5.0-kiln"', 'PACKAGE_VERSION="'+sys.argv[2]+'"')
s=s.replace('AUTOINSTALL=no', 'AUTOINSTALL=yes\nBUILD_EXCLUSIVE_KERNEL="^7[.]4[.]"')
p.write_text(s)
PY
dkms add -m aic8800-usb -v "$aic_version"
dkms build -m aic8800-usb -v "$aic_version" -k "$release" -a aarch64 -j 24
dkms install -m aic8800-usb -v "$aic_version" -k "$release" -a aarch64
depmod -a "$release"
for module in rknpu aic_load_fw_usb aic8800_fdrv_usb aic_btusb_usb rockchip_jpegd; do
    magic=$(modinfo -k "$release" -F vermagic "$module")
    [[ $magic == "$release "* && $magic == *preempt_rt* ]]
    modprobe --show-depends --set-version "$release" "$module" >/dev/null
    printf 'VERIFIED_MODULE %s %s\n' "$module" "$magic"
done

gzip -dc "/boot/vmlinuz-$release" > "/boot/.Image-$release.new"
cmp "/boot/.Image-$release.new" "$source_tree/arch/arm64/boot/Image"
chmod 644 "/boot/.Image-$release.new"
mv -f "/boot/.Image-$release.new" "/boot/Image-$release"
[[ ! -e /boot/dtb-$release ]]
mkdir "/boot/dtb-$release"
cp -a "/usr/lib/linux-image-$release/." "/boot/dtb-$release/"
cmp "/boot/dtb-$release/rockchip/rk3576-rock-4d.dtb" \
    "$source_tree/arch/arm64/boot/dts/rockchip/rk3576-rock-4d.dtb"
[[ $(fdtget "/boot/dtb-$release/rockchip/rk3576-rock-4d.dtb" /soc/jpegd@27910000 status) == okay ]]
[[ $(fdtget "/boot/dtb-$release/rockchip/rk3576-rock-4d.dtb" /soc/npu@27700000 status) == okay ]]

update-initramfs -u -k "$release"
gzip -t "/boot/initrd.img-$release"
lsinitramfs "/boot/initrd.img-$release" > "$status_dir/initramfs-files.txt"
mkimage -A arm64 -O linux -T ramdisk -C gzip -n uInitrd \
        -d "/boot/initrd.img-$release" "/boot/.uInitrd-$release.new"
dumpimage -T ramdisk -p 0 -o "$backup/candidate-initrd-payload" "/boot/.uInitrd-$release.new"
cmp "$backup/candidate-initrd-payload" "/boot/initrd.img-$release"
chmod 644 "/boot/.uInitrd-$release.new"
mv -f "/boot/.uInitrd-$release.new" "/boot/uInitrd-$release"

python3 - "$checker" "$expected" "$release" <<'PY'
from pathlib import Path
import sys
p=Path(sys.argv[1]);s=p.read_text()
old="assert result['kernel']=='7.3.0-npu-rt',result"
assert old in s
s=s.replace(old,"expected=Path('/var/lib/rock4d-rt/expected-kernel').read_text().strip()\n"
                "result['expected_kernel']=expected\nassert result['kernel']==expected,result")
p.write_text(s)
Path(sys.argv[2]).write_text(sys.argv[3]+'\n')
PY

# Select only the complete, checked set; a failure restores all three links.
for pair in "Image Image-$release" "uInitrd uInitrd-$release" "dtb dtb-$release"; do
    read -r name target <<< "$pair"
    ln -s "$target" "/boot/.$name.next.$$"
    mv -Tf "/boot/.$name.next.$$" "/boot/$name"
done
cmp "$backup/boot-id" /proc/sys/kernel/random/boot_id
[[ $(uname -r) != "$release" ]]
dkms status -k "$release" > "$status_dir/installed-dkms.txt"
python3 - "$status_dir" "$release" "$backup" <<'PY'
from pathlib import Path
import datetime,hashlib,json,os,platform,subprocess,sys
status,release,backup=Path(sys.argv[1]),sys.argv[2],Path(sys.argv[3])
mods={}
for name in ['rknpu','aic_load_fw_usb','aic8800_fdrv_usb','aic_btusb_usb','rockchip_jpegd']:
    filename=subprocess.check_output(['modinfo','-k',release,'-F','filename',name],text=True).strip()
    mods[name]={'file':filename,'vermagic':subprocess.check_output(['modinfo','-k',release,'-F','vermagic',name],text=True).strip()}
for name in ['Image','uInitrd']:
    old=backup/('previous-'+name)
    original=Path('/boot')/os.readlink(backup/name)
    assert hashlib.sha256(old.read_bytes()).digest()==hashlib.sha256(original.read_bytes()).digest()
assert hashlib.sha256((backup/'previous-board.dtb').read_bytes()).digest()==hashlib.sha256((Path('/boot')/os.readlink(backup/'dtb')/'rockchip/rk3576-rock-4d.dtb').read_bytes()).digest()
result={'time':datetime.datetime.now().astimezone().isoformat(timespec='seconds'),'installed_kernel':release,
        'running_kernel':platform.release(),'rebooted':False,'next_boot':{n:os.readlink('/boot/'+n) for n in ['Image','uInitrd','dtb']},
        'modules':mods,'previous_boot_files_verified':True,'backup':str(backup)}
(status/'installation-verification.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n')
print(json.dumps(result,ensure_ascii=False,indent=2))
PY
sync
echo "Kernel installed and selected for next boot. No reboot performed. Backup: $backup"
