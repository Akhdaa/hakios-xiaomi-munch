#!/bin/bash
# Pull the POCO F4's own firmware out of its Android into the fw-*/ directories at the top of the repository (never
# committed: it is the vendor's). The phone is on adb, in Android. Only reads: `adb pull`, nothing is written to the phone.
set -e
cd "$(dirname "$0")/../.."
pull() { # DIR SOURCE...  (missing files are told, not fatal)
	local dir=$1; shift
	mkdir -p "$dir"
	for f in "$@"; do adb pull "$f" "$dir/" >/dev/null 2>&1 || echo "pull-firmware.sh: $f: not on the phone"; done
	echo "pull-firmware.sh: $dir: $(ls -1 "$dir" | wc -l | tr -d ' ') files"
}
adb get-state >/dev/null
# the Adreno 650's (hakios.adreno.c: gpu=1)
pull fw-gpu /vendor/firmware/a650_gmu.bin /vendor/firmware/a650_sqe.fw $(for i in mdt b00 b01 b02; do echo /vendor/firmware/a650_zap.$i; done)
# the audio DSP's (adsp=1) and the speaker amplifiers' configuration (TFA9874: sound=4); the mixer paths are read for the
# microphones' and the speakers' routes
pull fw-audio /vendor/firmware_mnt/image/adsp.mdt $(for i in $(seq -w 0 18); do echo /vendor/firmware_mnt/image/adsp.b$i; done) \
	/vendor/firmware/tfa98xx.cnt
mkdir -p fw-audio/android
for f in mixer_paths.xml mixer_paths_overlay_static.xml mixer_paths_overlay_dynamic.xml audio_platform_info_intcodec.xml; do
	adb pull /vendor/etc/$f fw-audio/android/ >/dev/null 2>&1 || echo "pull-firmware.sh: /vendor/etc/$f: not on the phone"
done
# the QCA6390's (wifi=1, PCIe): its firmware, board files (bd_l11rgl.elf for a GLOBAL phone, ro.boot.hwc) and regulatory data
pull fw-wifi /vendor/firmware_mnt/image/qca6390/ /vendor/firmware/wlan/
# Bluetooth on the QCA6390 (Hastings ROM 2.0): the patch and the generic board configuration.
pull fw-bluetooth /vendor/bt_firmware/image/htbtfw20.tlv /vendor/bt_firmware/image/htnv20.bin
# the vibration motor's waveforms (AW86927: haptic=5)
pull fw-haptic /vendor/firmware/aw8697_haptic.bin
# the external modem, a Snapdragon X55 on PCIe 2 (docs: the modem plan): the images its boot loader asks for over MHI
# (sbl1.mbn by BHI, the rest by Sahara). Generic images of the ROM, not the phone's own data (its EFS stays on the phone).
pull fw-modem /vendor/firmware_mnt/image/sdx55m/
# the cameras' sensor modules (not used yet on this phone)
pull fw-camera $(adb shell ls /vendor/lib64/camera/com.qti.sensormodule.munch_*.bin 2>/dev/null | tr -d '\r')
echo "pull-firmware.sh: done; Wi-Fi also wants fw-wifi/mac (a random locally administered address, not the phone's own)"
echo "pull-firmware.sh: and, to join on its own, fw-wifi/network (the network's name and passphrase): both stay on this computer"
