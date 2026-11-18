#!/bin/bash
# tools/mk-winboot.sh [DIR]: everything a Windows PC needs to start iOS on the POCO F4 (munch) with a double click,
# without WSL or xl: boot-ios.bat, set-epoch.ps1, xnu-usb.img (Mu-Silicium's UEFI with the loader), run-payload.bin
# (the kernel, the small ramdisk, the kernel's arguments) and system.apfs (the system image). DIR defaults to
# win-boot/ here (git-ignored: the payload holds this phone's Wi-Fi address); from Windows it is
# \\wsl.localhost\<distro>\...\win-boot, or copy the folder anywhere. Make the kernel and the userland first.
#
# The same steps as ./xl: the phone into its bootloader, "fastboot boot" of the UEFI image (RAM only: nothing is
# flashed), the loader's "oem ramdisk" + "stage" of system.apfs, the payload, "oem run". The kernel's clock comes
# from epoch=, which set-epoch.ps1 writes into the payload just before it goes.
set -e
cd "$(dirname "$0")/.."
DIR=${1:-win-boot}
SECS=${SECS:-86400}             # how long the kernel runs before it goes back to the loader (a day)
K=hakios-kernel L=xnuloader/loader.efi RD=userland/root.apfs SYS=userland/system.apfs
for f in $K $L $RD $SYS; do [ -f "$f" ] || { echo "mk-winboot: $f is missing: build it first"; exit 1; }; done
mkdir -p "$DIR"

xnuloader/mkpayload.py -o "$DIR/usb-payload.bin" loader.efi=$L --usb >/dev/null
DEVICE=munch ./mkxnuimg.py -p "$DIR/usb-payload.bin" -o "$DIR/xnu-usb.img" >/dev/null
rm -f "$DIR/usb-payload.bin"

args=$(tools/hakios-config.py args | tail -1)
# the radio's address is this phone's (fw-wifi/mac, kept out of the repository; it goes only into DIR)
[ -s fw-wifi/mac ] && args="$args wifi_mac=$(tr -d ' \n\r' < fw-wifi/mac)"
# epoch= gets ten digits now; set-epoch.ps1 puts the PC's time over them, same length, at every boot
xnuloader/mkpayload.py -o "$DIR/run-payload.bin" kernel=$K ramdisk=$RD \
	--cmdline "loader_return_after=$SECS epoch=$(date +%s) sb_delay=8 $args" >/dev/null

# (a hard link when DIR is on the same disk: Windows sees an ordinary file, and 2.5 GB are not spent twice)
ln -f $SYS "$DIR/system.apfs" 2>/dev/null || {
	echo "mk-winboot: copying system.apfs ($(( $(stat -c %s $SYS) / 1048576 )) MB)..."
	cp $SYS "$DIR/system.apfs"
}

cat > "$DIR/set-epoch.ps1" <<'EOF'
# set-epoch.ps1 <payload>: the kernel's clock. Puts the PC's time (seconds since 1970, UTC) over the ten digits
# after "epoch=" in the payload's command line. Nothing else in the file changes.
param([string]$Payload)
$bytes = [System.IO.File]::ReadAllBytes($Payload)
$key = [System.Text.Encoding]::ASCII.GetBytes("epoch=")
$now = [System.Text.Encoding]::ASCII.GetBytes(([DateTimeOffset]::UtcNow.ToUnixTimeSeconds()).ToString())
for ($i = $bytes.Length - 16; $i -ge 0; $i--) {
	$match = $true
	for ($k = 0; $k -lt $key.Length; $k++) { if ($bytes[$i + $k] -ne $key[$k]) { $match = $false; break } }
	if ($match) {
		if ($now.Length -ne 10) { Write-Host "set-epoch: the time is not ten digits"; exit 1 }
		for ($k = 0; $k -lt 10; $k++) { $bytes[$i + $key.Length + $k] = $now[$k] }
		[System.IO.File]::WriteAllBytes($Payload, $bytes)
		exit 0
	}
}
Write-Host "set-epoch: no epoch= in $Payload"
exit 1
EOF

cat > "$DIR/boot-ios.bat" <<'EOF'
@echo off
rem boot-ios.bat: iOS on the POCO F4, all in RAM (fastboot boot, never fastboot flash).
rem   the phone from Android or from fastboot -> Mu-Silicium + the loader -> system.apfs -> the kernel -> iOS
rem Made by tools/mk-winboot.sh. Platform tools: set PT to their folder if they are not where Android Studio puts them.
setlocal EnableDelayedExpansion
rem (pushd: the folder may be a \\wsl.localhost path, which cmd takes only that way)
pushd "%~dp0"
if not defined PT set "PT=%LOCALAPPDATA%\Android\Sdk\platform-tools"
set "ADB=%PT%\adb.exe"
set "FB=%PT%\fastboot.exe"
if not exist "%FB%" (
	echo fastboot.exe is not in %PT%: set PT to the platform-tools folder
	goto :fail
)
for %%f in (xnu-usb.img run-payload.bin system.apfs set-epoch.ps1) do (
	if not exist "%%f" ( echo %%f is missing here & goto :fail )
)

echo [1/6] the phone into its bootloader
"%FB%" devices 2>nul | find "fastboot" >nul
if errorlevel 1 (
	"%ADB%" get-state 2>nul | find "device" >nul
	if errorlevel 1 (
		echo   no phone on USB, neither in Android ^(with USB debugging^) nor in fastboot
		goto :fail
	)
	"%ADB%" reboot bootloader
)
set /a n=0
:wait_fastboot
"%FB%" devices 2>nul | find "fastboot" >nul
if not errorlevel 1 goto :in_fastboot
set /a n+=1
if !n! geq 60 ( echo   the phone did not reach fastboot & goto :fail )
timeout /t 2 /nobreak >nul
goto :wait_fastboot
:in_fastboot

"%FB%" getvar product 2>&1 | find "xnuloader" >nul
if not errorlevel 1 goto :in_loader
"%FB%" getvar product 2>&1 | find "munch" >nul
if errorlevel 1 (
	echo   this phone is not a POCO F4 ^(munch^): the UEFI image is made for that one only
	goto :fail
)

echo [2/6] Mu-Silicium and the loader ^(fastboot boot, RAM only^)
set /a try=0
:boot_again
set /a try+=1
"%FB%" boot xnu-usb.img
set /a n=0
:wait_loader
timeout /t 2 /nobreak >nul
"%FB%" getvar product 2>&1 | find "xnuloader" >nul
if not errorlevel 1 goto :in_loader
set /a n+=1
if !n! lss 20 goto :wait_loader
rem (right after the phone enters fastboot the first boot is sometimes not taken)
if !try! lss 2 goto :boot_again
echo   the loader did not come up
goto :fail
:in_loader

echo [3/6] the system image into the phone's memory ^(about 100 s^)
"%FB%" oem ramdisk 2>&1 | find "OKAY" >nul
if errorlevel 1 ( echo   the loader refused the image & goto :fail )
"%FB%" stage system.apfs
if errorlevel 1 ( echo   the upload failed & goto :fail )

echo [4/6] the clock
powershell -NoProfile -ExecutionPolicy Bypass -File set-epoch.ps1 run-payload.bin
if errorlevel 1 echo   ^(the clock is not set: iOS starts with the time the payload was made^)

echo [5/6] the kernel
"%FB%" stage run-payload.bin
if errorlevel 1 ( echo   the upload failed & goto :fail )

echo [6/6] iOS starts
"%FB%" oem run
echo.
echo iOS is starting on the phone. To end it: fastboot oem stop ^(the phone goes back to the loader^),
echo then fastboot oem reboot-bootloader or fastboot reboot.
pause
exit /b 0

:fail
echo.
echo stopped.
pause
exit /b 1
EOF
sed -i 's/$/\r/' "$DIR/boot-ios.bat"
echo "mk-winboot: $DIR: boot-ios.bat, set-epoch.ps1, xnu-usb.img, run-payload.bin, system.apfs (kernel runs ${SECS} s)"
