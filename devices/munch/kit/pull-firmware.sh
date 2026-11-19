#!/bin/bash
# devices/munch/kit/pull-firmware.sh ROOT: the firmware the POCO F4's drivers load, from YOUR phone's Android, into
# ROOT/fw-*/ (the kit's assemble.sh calls it). Read only: adb pull, and tar through su for what only root can read
# (the vendor's firmware partition, the sensor DSP's libraries and calibration). Nothing on the phone is written.
#   ADB=...                   the adb to use
#   PERSIST_IMG=FILE          read the sensors' calibration from an offline dump of your persist partition (ext4,
#                             with debugfs) instead of from the phone
set -euo pipefail
ROOT=${1:?the kit source directory}
A=${ADB:-adb}
die() { echo "pull-firmware: $*" >&2; exit 1; }
[ "$("$A" get-state 2>/dev/null | tr -d '\r')" = device ] || die "no phone in Android on adb (USB debugging on, this PC allowed)"
dev=$("$A" shell getprop ro.product.device | tr -d '\r')
[ "$dev" = munch ] || die "this kit is for the POCO F4 (munch); this phone is '$dev'"
"$A" shell su -c id </dev/null 2>/dev/null | tr -d '\r' | grep -q 'uid=0' || die "no root: allow su for the shell on the phone (Magisk, KernelSU)"
echo "pull-firmware: $("$A" shell getprop ro.build.version.incremental | tr -d '\r') on a munch"
rtar() { # DIR TO FILES...: root's tar of FILES (shell patterns) in DIR on the phone, unpacked into TO
	local dir=$1 to=$2; shift 2
	mkdir -p "$to"
	"$A" exec-out "su -c 'cd $dir && tar -cf - $*'" | tar -C "$to" -xf -
}
cd "$ROOT"
rm -rf fw-gpu fw-audio fw-wifi/qca6390 fw-bluetooth fw-haptic fw-camera fw-sensors fw-modem
echo "pull-firmware: GPU";            rtar /vendor/firmware fw-gpu 'a650_gmu.bin a650_sqe.fw a650_zap.*'
echo "pull-firmware: audio DSP";      rtar /vendor/firmware_mnt/image fw-audio 'adsp.mdt adsp.b??'
rtar /vendor/firmware fw-audio 'tfa98xx.cnt' 2>/dev/null || true
mkdir -p fw-audio/android
for f in mixer_paths.xml mixer_paths_overlay_static.xml mixer_paths_overlay_dynamic.xml audio_platform_info_intcodec.xml; do
	"$A" pull /vendor/etc/$f fw-audio/android/ >/dev/null 2>&1 || true
done
echo "pull-firmware: Wi-Fi";          rtar /vendor/firmware_mnt/image fw-wifi 'qca6390'; rtar /vendor/firmware fw-wifi 'wlan'
cp -R fw-wifi/qca6390/. fw-wifi/ 2>/dev/null || true; cp -R fw-wifi/wlan/. fw-wifi/ 2>/dev/null || true
echo "pull-firmware: Bluetooth";      rtar /vendor/bt_firmware/image fw-bluetooth 'htbtfw20.tlv htnv20.bin'
echo "pull-firmware: vibration";      rtar /vendor/firmware fw-haptic 'aw8697_haptic.bin' 2>/dev/null || true
echo "pull-firmware: cameras";        rtar /vendor/lib64/camera fw-camera 'com.qti.sensormodule.munch_*.bin com.qti.tuned.munch_*.bin' 2>/dev/null || true
echo "pull-firmware: modem (X55)";    rtar /vendor/firmware_mnt/image fw-modem 'sdx55m'
rtar /vendor/firmware_mnt/image fw-modem/image 'modem_pr sdx55' 2>/dev/null || true
echo "pull-firmware: sensor DSP"
S=fw-sensors/sensors
rtar /vendor/firmware_mnt/image fw-sensors/slpi 'slpi.mdt slpi.b??'
mkdir -p "$S/vendor/etc" "$S/usr/lib/qcom/adsp" "$S/sys/devices/soc0" "$S/mnt/vendor/persist/sensors"
"$A" exec-out "tar -C /vendor/etc -cf - sensors 2>/dev/null" | tar -C "$S/vendor/etc" -xf -
rtar /vendor/dsp/sdsp "$S/usr/lib/qcom/adsp" '.'
"$A" shell 'for f in /sys/devices/soc0/*; do [ -f "$f" ] && echo "${f##*/}=$(head -1 "$f" 2>/dev/null)"; done' | tr -d '\r' |
	while IFS= read -r line; do
		n=${line%%=*}; v=${line#*=}
		case "$n" in */*|""|.*|serial_number) continue;; esac
		printf '%s\n' "$v" > "$S/sys/devices/soc0/$n"
	done
if [ -n "${PERSIST_IMG:-}" ]; then
	command -v debugfs >/dev/null || die "no debugfs (e2fsprogs) to read $PERSIST_IMG"
	tmp=$(mktemp -d); debugfs -R "rdump /sensors $tmp" "$PERSIST_IMG" >/dev/null 2>&1
	cp -R "$tmp/sensors/." "$S/mnt/vendor/persist/sensors/"; rm -rf "$tmp"
else
	rtar /mnt/vendor/persist/sensors "$S/mnt/vendor/persist/sensors" '.'
fi
# the Wi-Fi radio's address: a random locally administered one, made here (never the phone's own)
if [ ! -s fw-wifi/mac ]; then
	printf '02:%02x:%02x:%02x:%02x:%02x\n' $((RANDOM % 256)) $((RANDOM % 256)) $((RANDOM % 256)) $((RANDOM % 256)) $((RANDOM % 256)) > fw-wifi/mac
fi
chmod -R u+rw,go+r fw-* 2>/dev/null || true
for need in fw-gpu/a650_sqe.fw fw-audio/adsp.mdt fw-wifi/amss20.bin fw-bluetooth/htbtfw20.tlv fw-sensors/slpi/slpi.mdt; do
	[ -f "$need" ] || die "$need is missing: was root allowed?"
done
echo "pull-firmware: $(find fw-* -type f | wc -l) files, $(du -sch fw-* | tail -1 | cut -f1)"
