#!/bin/bash
# tools/ufs-boot/darwin-write.sh IMAGE: put IMAGE (make-ufs-boot.sh's out/rootfs.apfs) at the start of the phone's
# GPT partition named "darwin", and nowhere else; then read it back and compare. NOT TESTED on the POCO F4 (README.md).
# THIS WRITES TO THE PHONE. It runs only with DARWIN_WRITE=yes, and refuses unless:
#   - the phone in Android on adb is a munch, with root;
#   - /dev/block/by-name/darwin exists, is a block device whose partition name in sysfs is "darwin", is not any of
#     Android's own partitions (userdata, super, boot, vendor_boot, dtbo, vbmeta, modem, persist, ... are refused by
#     name), is not mounted, and is at least as large as IMAGE;
# then IMAGE is streamed into it with dd (adb exec-in; nothing is staged on /data), and the same number of bytes is
# read back and its sha256 compared with IMAGE's.
#   darwin-write.sh --check     only the checks, with what it would write (writes nothing)
set -euo pipefail
A=${ADB:-adb}
die() { echo "darwin-write: $*" >&2; exit 1; }
CHECK=; [ "${1:-}" = --check ] && { CHECK=1; shift; }
IMAGE=${1:-$(dirname "$0")/out/rootfs.apfs}
[ -f "$IMAGE" ] || die "no image ($IMAGE)"
size=$(stat -c %s "$IMAGE")
[ $((size % 4096)) = 0 ] || die "$IMAGE is not a whole number of 4 KiB blocks"
[ "$(head -c 36 "$IMAGE" | tail -c 4)" = NXSB ] || die "$IMAGE is not an APFS container (no NXSB)"
[ "$("$A" get-state 2> /dev/null | tr -d '\r')" = device ] || die "no phone in Android on adb"
dev=$("$A" shell getprop ro.product.device | tr -d '\r')
[ "$dev" = munch ] || die "this is for the POCO F4 (munch); the phone is '$dev'"
su() { "$A" shell "su -c '$1'" < /dev/null | tr -d '\r'; }
su id | grep -q 'uid=0' || die "no root (allow su for the shell)"
real=$(su 'readlink -f /dev/block/by-name/darwin')
[ -n "$real" ] && [ "$real" != /dev/block/by-name/darwin ] || die "the phone has no partition named darwin (/dev/block/by-name/darwin)"
base=${real##*/}
pname=$(su "cat /sys/class/block/$base/uevent" | sed -n 's/^PARTNAME=//p')
[ "$pname" = darwin ] || die "$real is named '$pname' in sysfs, not darwin"
for bad in $(su 'ls /dev/block/by-name'); do
	case "$bad" in darwin) continue;; esac
	[ "$(su "readlink -f /dev/block/by-name/$bad")" = "$real" ] && die "$real is also Android's partition $bad"
done
su 'cat /proc/mounts' | awk '{print $1}' | grep -q -x -e "$real" -e /dev/block/by-name/darwin && die "$real is mounted"
psize=$(su "blockdev --getsize64 $real")
[ "$psize" -ge "$size" ] 2> /dev/null || die "darwin ($psize bytes) is smaller than $IMAGE ($size bytes)"
echo "darwin-write: darwin is $real, $psize bytes; $IMAGE is $size bytes, written from its start"
want=$(sha256sum "$IMAGE" | cut -d' ' -f1)
[ -n "$CHECK" ] && { echo "darwin-write: --check: nothing written"; exit 0; }
[ "${DARWIN_WRITE:-}" = yes ] || die "this writes to the phone: run it again with DARWIN_WRITE=yes"
echo "darwin-write: writing (several minutes)..."
"$A" exec-in "su -c 'dd of=/dev/block/by-name/darwin bs=4194304 conv=fsync 2>/dev/null'" < "$IMAGE"
got=$(su "head -c $size /dev/block/by-name/darwin | sha256sum" | cut -d' ' -f1)
[ "$got" = "$want" ] || die "read back $got, expected $want: the partition does not hold the image"
echo "darwin-write: done, read back and equal (sha256 $want)"
