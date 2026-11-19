@echo off
setlocal
rem Makes the bootable iOS package for a POCO F4 (munch) from this kit, in WSL (Windows Subsystem for Linux):
rem assemble.sh does the work (see README.md). The package appears beside this folder, as ios-munch.
where wsl > nul 2>&1
if errorlevel 1 goto nowsl
wsl -e true > nul 2>&1
if errorlevel 1 goto nodistro
set "KIT=%~dp0"
set "KIT=%KIT:~0,-1%"
for /f "usebackq delims=" %%p in (`wsl wslpath -a "%KIT%"`) do set "WKIT=%%p"
wsl bash "%WKIT%/assemble.sh" %*
echo.
pause
exit /b

:nowsl
echo WSL is needed: run "wsl --install" in an administrator PowerShell, restart, then run this file again.
pause
exit /b 1

:nodistro
echo WSL has no Linux yet: run "wsl --install -d Ubuntu", set up its user, then run this file again.
pause
exit /b 1
