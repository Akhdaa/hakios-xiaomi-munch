@echo off
setlocal
rem start.bat: iOS on the POCO F4 (munch), all in RAM: fastboot boot of Mu-Silicium and the loader, the system image
rem uploaded, the kernel started. Nothing is flashed; a reboot brings Android back. Made by the munch kit's assemble.sh.
pushd "%~dp0" || ( echo Cannot open this folder. & pause & exit /b 1 )
if exist "%~dp0bin\fastboot.exe" set "PT=%~dp0bin"
set "BOOT_ARGS="
rem a package built with Sileo has a second payload without it: which one, at each boot
if not exist "run-payload-nojb.bin" goto boot
choice /C YN /N /M "Boot with Sileo (jailbreak prefix)? [Y/N]: "
if errorlevel 2 set "BOOT_ARGS=-NoJB"
:boot
powershell.exe -STA -NoLogo -NoProfile -ExecutionPolicy Bypass -File ".\boot-ios.ps1" %BOOT_ARGS%
set "RESULT=%ERRORLEVEL%"
popd
pause
exit /b %RESULT%
