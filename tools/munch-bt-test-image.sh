#!/usr/bin/env bash
# Build the small APFS and a RAM-only Bluetooth payload for the POCO F4.
# Usage: [MUNCH_BT_AUDIO=1] [MUNCH_BT_MODE=4] [MUNCH_BT_SCAN=0] bash tools/munch-bt-test-image.sh [BASE_ROOT [OUTPUT_DIR]]
set -euo pipefail
cd "$(dirname "$0")/.."
base=${1:-userland/root}
out=${2:-logs/munch-bt-ios}
mode=${MUNCH_BT_MODE:-3}
case "$mode" in 3|4) ;; *) exit 1 ;; esac
sdk="${THEOS:-$HOME/theos}/sdks/iPhoneOS16.5.sdk"
export PATH="$PWD/xnu-deps/host/bin:$PATH"

rg -q '^CONFIG_DEVICE="munch"$' .config
rg -q '^CONFIG_HAKIOS_MUNCH_BT=y$' .config
rg -aq 'iOS H4BC transport ready' hakios-kernel
test -d "$sdk"
test -s "$base/etc/inject.conf"
if ! rg -aq 'EDRServerStartService' "$base/lib/libhakios.dylib"; then
    echo 'munch-bt-test-image: rebuild libhakios.dylib from userland/src/inject.c (missing EDR service support)' >&2
    exit 1
fi
test -s fw-bluetooth/htbtfw20.tlv
if ! rg -aq '_canAdjustBrightness' "$base/lib/libhakios.sb.dylib"; then
    echo 'munch-bt-test-image: rebuild libhakios.sb.dylib from userland/src/inject_sb.c (missing Control Center brightness support)' >&2
    exit 1
fi
test -s fw-bluetooth/htnv20.bin
mac=$(tr -d '\n\r' < fw-wifi/mac)
[[ "$mac" =~ ^([[:xdigit:]]{2}:){5}[[:xdigit:]]{2}$ ]]

mkdir -p "$out"
work=$(mktemp -d "$out/root.XXXXXX")
trap 'rm -rf -- "$work"' EXIT
cp -a "$base"/. "$work"/
mkdir -p "$work/fw"
install -m 0644 fw-bluetooth/htbtfw20.tlv fw-bluetooth/htnv20.bin "$work/fw/"

if [[ ${MUNCH_BT_AUDIO:-0} == 1 ]]; then
    plugin=System/Library/Audio/Plug-Ins/HAL/BTAudioHALPlugin.driver
    audio_staging=$(mktemp -d "$work/.audio-hal.XXXXXX")
    test -L "$work/hal/HakiosAudio.driver"
    test -s sysimage/rootfs.dmg
    7z x -y -o"$audio_staging" sysimage/rootfs.dmg "$plugin/*" >/dev/null
    test -s "$audio_staging/$plugin/Info.plist"
    test -s "$audio_staging/$plugin/BTAudioHALPlugin"
    test -s "$audio_staging/$plugin/_CodeSignature/CodeResources"
    mv "$audio_staging/$plugin" "$work/hal/"
    rmdir "$audio_staging/System/Library/Audio/Plug-Ins/HAL" "$audio_staging/System/Library/Audio/Plug-Ins" \
        "$audio_staging/System/Library/Audio" "$audio_staging/System/Library" "$audio_staging/System" "$audio_staging"
    xnu-clang -arch arm64 -Os -static -nostdlib -ffreestanding -fno-stack-protector -fno-builtin -Wall -Wextra \
        -Wl,-e,_start -Wl,-pagezero_size,0x100000000 -Iuserland/libmini -o "$work/sbin/init" \
        userland/src/init.c userland/obj/mini.o userland/obj/start.o
fi

cc=(xnu-clang -arch arm64 -isysroot "$sdk" -O1 -Wall -Wextra -Werror)
"${cc[@]}" -fblocks -dynamiclib -install_name /redwood/lib/libhakios.bt.dylib \
    -framework CoreFoundation -framework IOKit \
    -o "$work/lib/libhakios.bt.dylib" userland/src/munch_bluetooth.c
printf '\n/redwood/lib/libhakios.bt.dylib BlueTool bluetoothd\n' >> "$work/etc/inject.conf"

bt_oslog=
if [[ ${MUNCH_BT_SCAN:-1} == 1 ]]; then
    bt_oslog="com.apple.bluetoothd com.apple.BlueTool"
fi
OSLOG="$bt_oslog" python3 userland/mkjobs.py sysimage/rootfs "$work/LaunchDaemons" \
    com.apple.BlueTool com.apple.bluetoothd com.apple.tccd com.apple.securityd
if [[ ${MUNCH_BT_SCAN:-1} == 1 ]]; then
    "${cc[@]}" -fobjc-arc -framework Foundation -framework CoreBluetooth \
        -o "$work/bin/munch_bt_test" userland/src/munch_bt_test.m
    python3 - "$work" <<'PY'
import plistlib
import sys
from pathlib import Path

jobs = Path(sys.argv[1]) / "LaunchDaemons"
label = "com.hakios.munch-bt-test"
job = {
    "Label": label,
    "ProgramArguments": ["/redwood/bin/munch_bt_test", "150"],
    "RunAtLoad": True,
    "StandardOutPath": "/dev/console",
    "StandardErrorPath": "/dev/console",
}
(jobs / (label + ".plist")).write_bytes(plistlib.dumps(job, fmt=plistlib.FMT_BINARY))
PY

fi
# MUNCH_BT_ROOT_OUT (optional): keep a copy of the finished tree, a base root for another image builder (the Metal
# payload's CA_BASE_ROOT). It must not exist yet or be empty; unset, nothing changes.
if [[ -n ${MUNCH_BT_ROOT_OUT:-} ]]; then
    mkdir -p "$MUNCH_BT_ROOT_OUT"
    if [[ -n $(ls -A "$MUNCH_BT_ROOT_OUT") ]]; then
        echo "munch-bt-test-image: $MUNCH_BT_ROOT_OUT is not empty" >&2
        exit 1
    fi
    cp -a "$work"/. "$MUNCH_BT_ROOT_OUT"/
fi
python3 tools/apfs/mkapfs.py -z --plain fw -o "$out/root.apfs" "redwood=$work"

args=$(tools/hakios-config.py args | tail -1)
case " $args " in
    *" ufs_write=1 "*|*" var_store=1 "*|*" munch_bt="*) exit 1 ;;
esac
case " $args " in *" wifi=1 "*) ;; *) exit 1 ;; esac
xnuloader/mkpayload.py -o "$out/ios-bluetooth-payload.bin" kernel=hakios-kernel ramdisk="$out/root.apfs" \
    --cmdline "loader_return_after=300 epoch=$(date +%s) sb_delay=8 $args wifi_mac=$mac fb_backlight=1 munch_bt=$mode"
