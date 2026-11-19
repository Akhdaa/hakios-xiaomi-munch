@echo off
setlocal
if /I "%~1"=="-StorageTest" goto storage
if /I "%~1"=="-Check" goto check
if /I "%~1"=="-Ram" goto ram
if /I "%~1"=="-Storage" goto storageboot
if /I "%~1"=="-Bluetooth" goto bluetooth
if /I "%~1"=="-InstallIPA" goto installipa
if /I "%~1"=="-LiveIPA" goto liveipa
if not "%~1"=="" goto usage

:menu
cls
echo HakiOS - POCO F4
echo.
echo   1. Boot iOS from RAM
echo   2. Test the system files on storage - phone in Android
echo   3. Boot iOS from storage - experimental, start from Android
echo   4. Boot iOS from RAM with Bluetooth - experimental
echo   5. Install an IPA into running iOS - needs SSH from option 6
echo   6. Boot iOS with the IPA installer and SSH - experimental
echo   0. Quit
echo.
choice /C 1234560 /N /M "Choose [1/2/3/4/5/6/0]: "
if errorlevel 255 exit /b 1
if errorlevel 7 exit /b 0
if errorlevel 6 goto installipa
if errorlevel 5 goto liveipa
if errorlevel 4 goto bluetooth
if errorlevel 3 goto storageboot
if errorlevel 2 goto storage
if errorlevel 1 goto ram
exit /b 1

:storage
call "%~dp0test-storage.bat"
exit /b

:check
set "BOOT_ARGS=-Check"
goto run

:storageboot
set "BOOT_ARGS=-Storage"
goto run

:bluetooth
set "BOOT_ARGS=-Bluetooth"
goto run

:installipa
set "BOOT_ARGS=-InstallIPA"
goto run

:liveipa
set "BOOT_ARGS=-LiveIPA"
goto run

:ram
set "BOOT_ARGS="

:run
pushd "%~dp0" || (
    echo Cannot open the HakiOS folder.
    pause
    exit /b 1
)
powershell.exe -STA -NoLogo -NoProfile -ExecutionPolicy Bypass -File ".\boot-ios.ps1" %BOOT_ARGS%
set "BOOT_RESULT=%ERRORLEVEL%"
popd
if /I not "%~1"=="-Check" pause
exit /b %BOOT_RESULT%

:usage
echo Run without arguments for the menu, or with -Check, -Ram, -Storage, -StorageTest, -Bluetooth, -LiveIPA, -InstallIPA.
exit /b 2
