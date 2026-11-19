#!/bin/bash
# devices/munch/pull-sensors.sh [PERSIST_IMG]: the files the POCO F4's sensor DSP needs, from YOUR phone, into fw-sensors/
# (git-ignored; never in the repository: the vendor's firmware and libraries, and the phone's own calibration).
#
# Needs the phone in Android with USB debugging and root (su): the SLPI's firmware and libraries are readable by root only.
# Nothing on the phone is written. Laid out as devices/munch/drivers/sensors (slpi=1) and userland/build.sh want it:
#   fw-sensors/slpi/                    slpi.mdt, slpi.b00..   (/vendor/firmware_mnt/image)  -> system.apfs /fw
#   fw-sensors/sensors/vendor/etc/sensors                      (/vendor/etc/sensors)         -> /redwood/fw/sensors
#   fw-sensors/sensors/usr/lib/qcom/adsp                       (/vendor/dsp/sdsp)
#   fw-sensors/sensors/sys/devices/soc0                        (/sys/devices/soc0, without the serial number)
#   fw-sensors/sensors/mnt/vendor/persist/sensors              the registry and calibration (see below)
#
# The calibration: from an offline dump of your own persist partition when you give one (PERSIST_IMG, an ext4 image:
# only its /sensors is read, with debugfs, read only), else from the phone (su, read only).
# The sensor DSP's driver is xiongnemo/hakios-alioth's (Redmi K40); this script follows its kit/pull-firmware.sh.
set -eu
cd "$(dirname "$0")/../.."
OUT=fw-sensors
A=${ADB:-adb}
command -v "$A" >/dev/null || { echo "pull-sensors: no adb (ADB=path/to/adb)" >&2; exit 1; }
[ "$("$A" get-state 2>/dev/null)" = device ] || { echo "pull-sensors: no phone in Android on adb" >&2; exit 1; }
"$A" shell su -c true </dev/null >/dev/null 2>&1 || { echo "pull-sensors: no root (allow su on the phone)" >&2; exit 1; }
S="$OUT/sensors"
rm -rf "$OUT/slpi" "$S"
mkdir -p "$OUT/slpi" "$S/vendor/etc" "$S/usr/lib/qcom/adsp" "$S/sys/devices/soc0" "$S/mnt/vendor/persist/sensors"

echo "pull-sensors: the sensor DSP's firmware"
"$A" exec-out "su -c 'cd /vendor/firmware_mnt/image && tar -cf - slpi.mdt slpi.b??'" | tar -C "$OUT/slpi" -xf -
echo "pull-sensors: its configuration and libraries"
"$A" exec-out "tar -C /vendor/etc -cf - sensors 2>/dev/null" | tar -C "$S/vendor/etc" -xf -
"$A" exec-out "su -c 'tar -C /vendor/dsp/sdsp -cf - .'" | tar -C "$S/usr/lib/qcom/adsp" -xf -
echo "pull-sensors: the SoC's identity (no serial number)"
"$A" shell 'for f in /sys/devices/soc0/*; do [ -f "$f" ] && echo "${f##*/}=$(head -1 "$f" 2>/dev/null)"; done' | tr -d '\r' |
	while IFS= read -r line; do
		n=${line%%=*}; v=${line#*=}
		case "$n" in */*|""|.*|serial_number) continue;; esac
		printf '%s\n' "$v" > "$S/sys/devices/soc0/$n"
	done
echo "pull-sensors: the registry and calibration"
if [ $# -ge 1 ]; then
	command -v debugfs >/dev/null || { echo "pull-sensors: no debugfs (e2fsprogs) to read $1" >&2; exit 1; }
	tmp=$(mktemp -d)
	debugfs -R "rdump /sensors $tmp" "$1" >/dev/null 2>&1
	cp -R "$tmp/sensors/." "$S/mnt/vendor/persist/sensors/"
	rm -rf "$tmp"
else
	"$A" exec-out "su -c 'tar -C /mnt/vendor/persist/sensors -cf - .'" | tar -C "$S/mnt/vendor/persist/sensors" -xf -
fi
chmod -R u+rw,go+r "$OUT"
[ -f "$OUT/slpi/slpi.mdt" ] && [ -d "$S/vendor/etc/sensors" ] || { echo "pull-sensors: something is missing" >&2; exit 1; }
echo "pull-sensors: $OUT: $(find "$OUT" -type f | wc -l) files, $(du -sh "$OUT" | cut -f1); now make images"
