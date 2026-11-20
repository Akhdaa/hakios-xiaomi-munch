#!/bin/bash
# Make the bootable package for a POCO F4 (munch) out of this kit (tools/make-kit.sh) and what cannot be published,
# made here on your computer:
#   - the system image, from the iOS 16.5 IPSW for the iPhone X, downloaded from Apple's public servers (or IPSW=FILE);
#     nothing is decrypted or circumvented;
#   - the firmware the drivers load, from your phone's own Android (pull-firmware.sh: read only, needs root);
#   - the loader with the iPhone identity, made from the IPSW's device tree;
#   - the Wi-Fi network iOS joins.
# The package (ios-munch/, with start.bat) is yours: it holds Apple's files, the vendor's firmware and your Wi-Fi
# password -- do not pass it on. Nothing here writes to the phone.
#
# usage: assemble.sh [OUT_DIR]           (default: ios-munch beside the kit)
#   ADB=...            the adb to use (on WSL the kit fetches Google's Windows platform tools and uses their adb.exe)
#   IPSW=FILE          an iPhone10,3,iPhone10,6_16.5_20F66_Restore.ipsw you already have
#   SSID=... PSK=...   the Wi-Fi network, instead of being asked
#   FW_FROM=DIR        take the fw-* directories from DIR instead of the phone (a second assembly)
#   FW_PULL=1          copy the firmware from the phone again (by default the copy of the last run is used)
#   JB=1 / JB=0        add Sileo (a rootless jailbreak prefix) to the system, or not (asked when run in a terminal)
#   PERSIST_IMG=FILE   the sensors' calibration from an offline dump of your persist partition
#   SYSIMAGE_FROM=DIR  use an IPSW extraction already made (another tree's sysimage/)
#
# The structure follows xiongnemo/hakios-alioth's kit (assemble.sh, kit-v1.4) for the Redmi K40.
set -euo pipefail
KIT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# A kit on a Windows drive: the work is done in the Linux file system (iOS's file names differ in case only, which
# a Windows drive cannot keep apart, and it is much faster); only the finished package is written beside the kit.
case "$KIT" in
/mnt/[a-z]/*)
	WORK=${MUNCH_WORK:-$HOME/$(basename "$KIT")}
	echo "assemble: the kit is on a Windows drive; working in $WORK, the package beside the kit"
	mkdir -p "$WORK"
	cp -a "$KIT/prebuilt" "$KIT/source" "$KIT/assemble.sh" "$KIT/README.md" "$WORK/"
	exec bash "$WORK/assemble.sh" "$(realpath -m "${1:-$KIT/../ios-munch}")"
	;;
esac
ROOT=$KIT/source
OUT=$(realpath -m "${1:-$KIT/../ios-munch}")
TOOLS=$KIT/.tools
mkdir -p "$TOOLS"
export PATH=$TOOLS:$PATH
log() { printf '\n== %s\n' "$*"; }
die() { echo "assemble: $*" >&2; exit 1; }
[ -f "$KIT/prebuilt/kernel-munch.bin" ] && [ -f "$KIT/prebuilt/kernel-munch.ver" ] && [ -d "$ROOT/devices/munch" ] || die "this is not an unpacked kit ($KIT)"

log "what this computer needs"
missing=
for t in python3 cc make clang curl unzip git; do command -v $t > /dev/null || missing="$missing $t"; done
command -v lld-link > /dev/null || command -v ld.lld > /dev/null || missing="$missing lld"
command -v debugfs > /dev/null || [ -z "${PERSIST_IMG:-}" ] || missing="$missing e2fsprogs"
if [ -n "$missing" ]; then
	echo "missing:$missing"
	command -v apt-get > /dev/null || die "install:$missing"
	echo "installing them with apt (sudo may ask for your password)"
	sudo apt-get update -qq
	pkgs=$(echo "$missing" | sed 's/\bcc\b/build-essential/; s/\bmake\b/build-essential/')
	sudo apt-get install -y -qq $pkgs > /dev/null
fi
if ! command -v lld-link > /dev/null; then
	for l in lld ld.lld; do command -v $l > /dev/null && { ln -sf "$(command -v $l)" "$TOOLS/lld-link"; break; }; done
fi
command -v lld-link > /dev/null || die "no lld-link (install lld)"
# 7-Zip 23 or newer reads APFS: the official Linux build when the system's is older
if ! { command -v 7z > /dev/null && [ "$(7z 2>&1 | grep -o '7-Zip [0-9]*' | head -1 | awk '{print $2}')" -ge 23 ] 2> /dev/null; }; then
	echo "7-Zip 23 or newer: fetching the official Linux build"
	curl -fsSL https://github.com/ip7z/7zip/releases/download/25.01/7z2501-linux-x64.tar.xz | tar -xJ -C "$TOOLS" 7zz
	ln -sf "$TOOLS/7zz" "$TOOLS/7z"
fi
PT=$TOOLS/platform-tools-windows
if [ ! -f "$PT/adb.exe" ]; then
	curl -fsSL -o "$TOOLS/pt.zip" https://dl.google.com/android/repository/platform-tools-latest-windows.zip
	rm -rf "$PT" "$TOOLS/pt" && mkdir -p "$PT" && unzip -q -o "$TOOLS/pt.zip" -d "$TOOLS/pt" &&
		cp "$TOOLS"/pt/platform-tools/* "$PT/" && rm -rf "$TOOLS/pt" "$TOOLS/pt.zip"
fi
if [ -z "${ADB:-}" ]; then
	if grep -qi microsoft /proc/version 2> /dev/null; then ADB=$PT/adb.exe; chmod +x "$ADB"
	elif command -v adb > /dev/null; then ADB=adb
	else die "no adb: install Android's platform tools, or set ADB=..."; fi
fi
export ADB

log "1/5 the iOS 16.5 IPSW (iPhone X, 20F66): about 6.2 GB from Apple's servers the first time"
if [ -n "${SYSIMAGE_FROM:-}" ]; then
	# an extraction already made (another tree's or kit's sysimage/): linked, not copied
	for d in rootfs cryptex cryptex-app 20F66__iPhone10,3_6; do
		[ -e "$ROOT/sysimage/$d" ] || cp -al "$SYSIMAGE_FROM/$d" "$ROOT/sysimage/$d"
	done
	cp -p "$SYSIMAGE_FROM"/{system.manifest,system.files,var.skel,rootfs.slt,DeviceTree.d22ap.im4p,cryptex-app.dmg} "$ROOT/sysimage/" 2> /dev/null || true
	cp -p "$SYSIMAGE_FROM"/.done-* "$ROOT/sysimage/" 2> /dev/null || true
	touch "$ROOT"/sysimage/.done-* "$ROOT/sysimage/system.manifest" 2> /dev/null || true
fi
if [ ! -f "$ROOT/sysimage/.done-rootfs" ] || [ ! -f "$ROOT/sysimage/DeviceTree.d22ap.im4p" ]; then
	if [ -z "${IPSW:-}" ]; then
		IPSW=$KIT/iPhone10,3,iPhone10,6_16.5_20F66_Restore.ipsw
		if [ ! -f "$IPSW" ] || [ "$(stat -c %s "$IPSW")" != 6165698198 ]; then
			url=$(curl -fsSL https://api.ipsw.me/v4/ipsw/iPhone10,3/20F66 | python3 -c 'import json,sys; print(json.load(sys.stdin)["url"])')
			case "$url" in https://updates.cdn-apple.com/*|https://*.apple.com/*) ;; *) die "no Apple URL for the IPSW ($url)";; esac
			echo "from $url"
			curl -fL -C - -o "$IPSW" "$url"
		fi
	fi
	[ "$(stat -c %s "$IPSW")" = 6165698198 ] || die "$IPSW is not the 6 165 698 198-byte iOS 16.5 IPSW"
	(cd "$ROOT" && bash sysimage/unpack.sh "$IPSW") > "$KIT/unpack.log" 2>&1 || { tail -20 "$KIT/unpack.log"; die "the IPSW was not unpacked"; }
fi

log "2/5 the firmware, from your phone's Android (read only; allow root for the shell when the phone asks)"
if [ -n "${FW_FROM:-}" ]; then
	for d in fw-gpu fw-audio fw-wifi fw-bluetooth fw-camera fw-sensors fw-haptic fw-modem; do
		[ -d "$FW_FROM/$d" ] && { rm -rf "$ROOT/$d"; cp -r "$FW_FROM/$d" "$ROOT/$d"; }
	done
elif [ -z "${FW_PULL:-}" ] && [ -f "$ROOT/fw-gpu/a650_sqe.fw" ] && [ -f "$ROOT/fw-audio/adsp.mdt" ] && [ -f "$ROOT/fw-wifi/amss20.bin" ] &&
	[ -f "$ROOT/fw-bluetooth/htbtfw20.tlv" ] && [ -f "$ROOT/fw-sensors/slpi/slpi.mdt" ]; then
	echo "the firmware copied last time is used (FW_PULL=1 copies it from the phone again)"
else
	bash "$ROOT/devices/munch/kit/pull-firmware.sh" "$ROOT"
fi

log "3/5 the Wi-Fi network iOS joins at every boot (optional: empty to join it in Settings each time)"
# iOS runs from RAM: what is typed in Settings is gone at the next boot, so a network kept in the package is joined at
# every boot without asking. Leave the name empty (or run without a terminal and no SSID=) to skip it.
if [ ! -f "$ROOT/fw-wifi/network" ] || [ -n "${SSID:-}" ]; then
	if [ -z "${SSID:-}" ] && [ -t 0 ]; then read -r -p "Wi-Fi name (SSID), empty to skip: " SSID || SSID=; fi
	if [ -n "${SSID:-}" ]; then
		[ -n "${PSK:-}" ] || { read -r -s -p "its password: " PSK || die "no Wi-Fi password given (PSK=...)"; echo; }
		mkdir -p "$ROOT/fw-wifi"
		(umask 077; printf '%s\n%s\n' "$SSID" "$PSK" > "$ROOT/fw-wifi/network")
	else
		echo "no network kept: join Wi-Fi in Settings after each boot"
	fi
fi

log "4/5 the system image and the root disk, with the kit's programs (mostly packing 2.5 GB)"
# Optional: a jailbreak prefix with Sileo (Procursus's rootless bootstrap under /var/jb, tools/mk-jb.sh fetches it from
# Procursus and Sileo's public repositories). Off unless JB=1 or answered yes; JB=0 removes one an earlier run added.
if [ -z "${JB:-}" ] && [ -t 0 ]; then
	read -r -p "Add Sileo (a rootless jailbreak prefix, ~130 MB) to the system? [y/N] " answer || answer=
	case "$answer" in [yY]*) JB=1;; *) JB=0;; esac
fi
if [ "${JB:-0}" = 1 ]; then
	[ -d "$ROOT/jb/root/var/jb" ] || (cd "$ROOT" && bash tools/mk-jb.sh) > "$KIT/jb.log" 2>&1 ||
		{ tail -15 "$KIT/jb.log"; die "the jailbreak prefix was not fetched (log: $KIT/jb.log)"; }
	echo "Sileo: in (the prefix is copied to /var/jb at every boot)"
else
	rm -rf "$ROOT/jb"
fi
rm -rf "$ROOT/prebuilt/userland" && mkdir -p "$ROOT/prebuilt" && cp -r "$KIT/prebuilt/userland" "$ROOT/prebuilt/userland"
mkdir -p "$ROOT/apps"
url=$(curl -fsSL https://api.github.com/repos/xiongnemo/ios-system-check/releases/latest | grep -o 'https://[^"]*/SystemCheck-[^"]*\.ipa' | head -1 || true)
if [ -n "$url" ] && [ ! -d "$ROOT/apps/SystemCheck.app" ]; then
	curl -fsSL -o "$TOOLS/SystemCheck.ipa" "$url" && rm -rf "$TOOLS/ipa" && unzip -q "$TOOLS/SystemCheck.ipa" 'Payload/*' -d "$TOOLS/ipa" &&
		mv "$TOOLS/ipa/Payload/SystemCheck.app" "$ROOT/apps/" && echo "apps: System Check (xiongnemo/ios-system-check)"
fi
(cd "$ROOT/userland" && PREBUILT=1 GL=1 REST=1 bash build.sh) > "$KIT/userland.log" 2>&1 ||
	{ tail -30 "$KIT/userland.log"; die "the images were not made (log: $KIT/userland.log)"; }
U=$ROOT/userland
[ -f "$U/root.apfs" ] && [ -f "$U/system.apfs" ] || die "no root.apfs / system.apfs (log: $KIT/userland.log)"

log "5/5 the loader (with the identity from the IPSW), the boot image and the payload"
(cd "$ROOT/xnuloader" && rm -f identity.h && bash mkidentity.sh "$ROOT/sysimage/DeviceTree.d22ap.im4p" && bash build.sh) \
	> "$KIT/loader.log" 2>&1 || { tail -20 "$KIT/loader.log"; die "the loader did not build"; }
mkdir -p "$ROOT/devices/munch/prebuilt" && cp "$KIT"/prebuilt/silicium/* "$ROOT/devices/munch/prebuilt/"
mkdir -p "$OUT/bin"
python3 "$ROOT/xnuloader/mkpayload.py" -o "$KIT/usb-payload.bin" loader.efi="$ROOT/xnuloader/loader.efi" --usb > /dev/null
(cd "$ROOT" && DEVICE=munch python3 mkxnuimg.py -p "$KIT/usb-payload.bin" -o "$OUT/xnu-usb.img" > /dev/null)
mac=$(head -1 "$ROOT/fw-wifi/mac" | tr -d '\r ')
args="sb_delay=8 cpus=8 bootcpu=7 cpufreq=1 touchscreen=1 gpu=1 adsp=1 sound=4 wifi=1 haptic=5 socinfo=1 thermal=1 \
battery_stop=15 pmic=1 battery=1 fb_hole=23 gpu_push_ms=0 munch_torch=3 panel=2 panel_level=300 fb_backlight=1 fb_hz=60 \
gpu_recover=1 cam=5 munch_bt=4 mic=4 slpi=1 hw=munch munch_charge=1 wifi_mac=$mac"
# (kernel.ver: the loader hands it to the kernel as hakios_kver, where the drivers find the SoC: without -sm8250- none of
# the phone's drivers starts -- kit 1.0's crash at the Apple logo)
python3 "$ROOT/xnuloader/mkpayload.py" -o "$OUT/run-payload.bin" kernel="$KIT/prebuilt/kernel-munch.bin" ramdisk="$U/root.apfs" \
	--kernel-version "$(cat "$KIT/prebuilt/kernel-munch.ver")" --cmdline "loader_return_after=86400 epoch=$(date +%s) $args" > /dev/null
[ "$(stat -c %s "$OUT/run-payload.bin")" -le $((63 << 20)) ] || die "the payload is more than the loader takes (63 MB)"
echo "copying the system image ($(( $(stat -c %s "$U/system.apfs") >> 20 )) MB)"
cp "$U/system.apfs" "$OUT/system.apfs"
cp "$ROOT/win-boot/boot-ios.ps1" "$ROOT/devices/munch/kit/start.bat" "$ROOT/devices/munch/kit/get-log.bat" \
	"$ROOT/devices/munch/kit/README-package.txt" "$OUT/"
cp "$PT"/adb.exe "$PT"/fastboot.exe "$PT"/AdbWinApi.dll "$PT"/AdbWinUsbApi.dll "$OUT/bin/" 2> /dev/null || true
rm -f "$KIT/usb-payload.bin"
echo
echo "assemble: the package is $OUT"
echo "  With the phone in Android (or fastboot) and connected, run start.bat there. Nothing is flashed; a reboot brings Android back."
echo "  It holds Apple's files, the vendor's firmware and your Wi-Fi password: keep it to yourself."
