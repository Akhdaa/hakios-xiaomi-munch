#!/bin/bash
# tools/ufs-boot/make-ufs-boot.sh [ROOT]: what a boot with the Darwin volume on the phone's "darwin" partition needs
# (HakiOS's base design; NOT TESTED on the POCO F4, README.md). ROOT is the repository or a kit's source/ (default:
# this repository), with sysimage/ unpacked and the fw-* directories there (a kit after assemble.sh has both).
# Writes into tools/ufs-boot/out/:
#   rootfs.apfs       Apple's Darwin volume from the IPSW, raw (5.6 GB): what darwin-write.sh puts on the partition
#   flashboot.apfs    the RAM part (userland/build.sh FLASHBOOT=1): the boot volume and the two cryptexes (~1 GB)
#   root.apfs         the small root image of a flash boot (its launchd job list made for the whole volume)
#   ufs-payload.bin   the kernel, that root.apfs and the boot arguments with ufs=2 root_apfs=ufs_darwin
# The RAM boot's userland/root.apfs and system.apfs are kept as they were.
#   KERNEL=FILE      the kernel (default: a kit's prebuilt/kernel-munch.bin, else ROOT/hakios-kernel)
#   KVER=TEXT        the kernel version for the loader (default: a kit's kernel-munch.ver, else tools/kernel-version.py)
#   WIFI_MAC=...     the Wi-Fi address (default: ROOT/fw-wifi/mac)
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(realpath "${1:-$HERE/../..}")
OUT=$HERE/out
KITDIR=$(realpath -m "$ROOT/..")
die() { echo "make-ufs-boot: $*" >&2; exit 1; }
[ -f "$ROOT/sysimage/.done-rootfs" ] && [ -f "$ROOT/sysimage/rootfs.dmg" ] ||
	die "no unpacked IPSW with rootfs.dmg in $ROOT/sysimage (sysimage/unpack.sh with KEEP=1)"
KERNEL=${KERNEL:-}
[ -n "$KERNEL" ] || { [ -f "$KITDIR/prebuilt/kernel-munch.bin" ] && KERNEL=$KITDIR/prebuilt/kernel-munch.bin || KERNEL=$ROOT/hakios-kernel; }
[ -f "$KERNEL" ] || die "no kernel ($KERNEL)"
KVER=${KVER:-}
[ -n "$KVER" ] || { [ -f "$KITDIR/prebuilt/kernel-munch.ver" ] && KVER=$(cat "$KITDIR/prebuilt/kernel-munch.ver") ||
	KVER=$(python3 "$ROOT/tools/kernel-version.py" | sed 's/-dirty$//'); }
case "$KVER" in *-sm8250-*) ;; *) die "the kernel version has no -sm8250- ($KVER): the phone's drivers would not start";; esac
mac=${WIFI_MAC:-$(head -1 "$ROOT/fw-wifi/mac" 2> /dev/null | tr -d '\r ' || true)}
mkdir -p "$OUT"

echo "make-ufs-boot: 1/3 the Darwin volume, raw, out of rootfs.dmg"
[ -f "$OUT/rootfs.apfs" ] || python3 "$ROOT/tools/apfs/dmg.py" extract "$ROOT/sysimage/rootfs.dmg" "$OUT/rootfs.apfs" --apfs ||
	die "rootfs.dmg was not extracted (its chunks are LZFSE: pip install lzfse)"

echo "make-ufs-boot: 2/3 flashboot.apfs and the flash boot's root.apfs (userland/build.sh FLASHBOOT=1)"
U=$ROOT/userland
for f in root.apfs system.apfs; do [ -f "$U/$f" ] && mv "$U/$f" "$U/$f.ram"; done
restore() { for f in root.apfs system.apfs; do [ -f "$U/$f.ram" ] && mv -f "$U/$f.ram" "$U/$f"; done; }
trap restore EXIT
pre=; [ -d "$ROOT/prebuilt/userland" ] && pre=1
(cd "$U" && PREBUILT=$pre FLASHBOOT=1 GL=1 REST=1 bash build.sh) > "$OUT/build.log" 2>&1 || { tail -20 "$OUT/build.log"; die "build.sh failed"; }
[ -f "$U/flashboot.apfs" ] && [ -f "$U/root.apfs" ] || die "no flashboot.apfs / root.apfs (log: $OUT/build.log)"
mv "$U/flashboot.apfs" "$OUT/flashboot.apfs"
mv "$U/root.apfs" "$OUT/root.apfs"
rm -f "$U/system.apfs"
restore; trap - EXIT

echo "make-ufs-boot: 3/3 the payload"
args="sb_delay=8 cpus=8 bootcpu=7 cpufreq=1 touchscreen=1 gpu=1 adsp=1 sound=4 wifi=1 haptic=5 socinfo=1 thermal=1 \
battery_stop=15 pmic=1 battery=1 fb_hole=23 gpu_push_ms=0 munch_torch=3 panel=2 panel_level=300 fb_backlight=1 fb_hz=60 \
gpu_recover=1 cam=5 munch_bt=4 mic=4 slpi=1 hw=munch munch_charge=1 ufs=2 root_apfs=ufs_darwin${mac:+ wifi_mac=$mac}"
python3 "$ROOT/xnuloader/mkpayload.py" -o "$OUT/ufs-payload.bin" kernel="$KERNEL" ramdisk="$OUT/root.apfs" \
	--kernel-version "$KVER" --cmdline "loader_return_after=86400 epoch=$(date +%s) $args" > /dev/null
ls -la "$OUT"/rootfs.apfs "$OUT"/flashboot.apfs "$OUT"/ufs-payload.bin | awk '{print "  " $5, $NF}'
echo "make-ufs-boot: done; darwin-write.sh puts rootfs.apfs on the phone's darwin partition (README.md)"
