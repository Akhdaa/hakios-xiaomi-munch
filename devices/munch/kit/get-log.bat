@echo off
setlocal
rem get-log.bat: the HakiOS log from the phone, while iOS runs or after it went back to the loader, into logs\.
rem The Wi-Fi address and the serial number are blanked out, so the file can be shared. A phone that rebooted to
rem Android has lost it: the log lives in RAM.
pushd "%~dp0" || ( echo Cannot open this folder. & pause & exit /b 1 )
if not exist logs mkdir logs
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -Command ^
  "$fb = Join-Path (Get-Location) 'bin\fastboot.exe'; if (!(Test-Path $fb)) { $fb = 'fastboot.exe' };" ^
  "$p = (& $fb getvar product 2>&1 | Out-String); if ($p -notmatch 'product:\s*xnuloader') { Write-Host 'The phone is not in HakiOS (the loader): nothing to fetch. After a reboot to Android the log is gone.'; exit 1 };" ^
  "$raw = Join-Path (Get-Location) 'logs\raw.tmp'; $out = Join-Path (Get-Location) ('logs\hakios-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.log');" ^
  "& $fb oem log 2>&1 | Out-Null; & $fb get_staged $raw 2>&1 | Out-Null; if (!(Test-Path $raw)) { Write-Host 'No log came back.'; exit 1 };" ^
  "$t = [IO.File]::ReadAllText($raw) -replace \"`0\", '';" ^
  "$t = $t -replace 'wifi_mac=[0-9A-Fa-f:]+', 'wifi_mac=XX:XX:XX:XX:XX:XX' -replace '(serial[ _-]?(number|no)?[^\r\n]{0,12}?)[0-9A-Fa-f.]{4,}', '$1<hidden>' -replace 'soc_serial=\S+', 'soc_serial=<hidden>';" ^
  "[IO.File]::WriteAllText($out, $t); Remove-Item $raw; Write-Host ('Saved ' + $out + ' (' + [int]((Get-Item $out).Length / 1KB) + ' KB)')"
set "RESULT=%ERRORLEVEL%"
popd
pause
exit /b %RESULT%
