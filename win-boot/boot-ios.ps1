param([switch]$Check, [switch]$Storage, [switch]$Bluetooth, [switch]$InstallIPA, [switch]$LiveIPA)

$ErrorActionPreference = 'Stop'

function Get-WslProject {
    # The repository's path inside WSL, from where this launcher is (win-boot/ in it): \\wsl.localhost\DISTRO\... or
    # \\wsl$\DISTRO\... directly, else as wslpath gives it.
    $parent = Split-Path -Parent $PSScriptRoot
    if ($parent -match '^\\\\wsl(?:\.localhost|\$)\\[^\\]+(\\.*)$') { return ($Matches[1] -replace '\\', '/') }
    $path = & "$env:windir\System32\wsl.exe" -d Ubuntu wslpath -a ($parent -replace '\\', '/')
    if ($LASTEXITCODE -ne 0 -or !$path) { throw "Cannot find this folder inside WSL: $parent" }
    return $path.Trim()
}

function Find-PlatformTool([string]$Name) {
    if ($env:PT) {
        $path = Join-Path $env:PT $Name
        if (Test-Path -LiteralPath $path -PathType Leaf) { return $path }
    }
    $command = Get-Command $Name -CommandType Application -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }
    $path = Join-Path $env:LOCALAPPDATA "Android\Sdk\platform-tools\$Name"
    if (Test-Path -LiteralPath $path -PathType Leaf) { return $path }
    throw "$Name not found. Install Android Platform Tools, or set PT to the folder that has it."
}

function Invoke-PlatformTool([string]$Tool, [string[]]$Arguments, [int]$Seconds = 5) {
    $start = New-Object System.Diagnostics.ProcessStartInfo
    $start.FileName = $Tool
    $start.Arguments = ($Arguments | ForEach-Object { '"' + $_ + '"' }) -join ' '
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $process = [System.Diagnostics.Process]::Start($start)
    try {
        $output = $process.StandardOutput.ReadToEndAsync()
        $errors = $process.StandardError.ReadToEndAsync()
        if (!$process.WaitForExit(1000 * $Seconds)) {
            $process.Kill()
            throw "$([IO.Path]::GetFileName($Tool)) $($Arguments[0]) did not answer within $Seconds seconds."
        }
        if ($process.ExitCode -ne 0) {
            throw "$($Arguments[0]) failed. $($errors.Result.Trim())"
        }
        return ($output.Result + $errors.Result).Trim()
    } finally {
        $process.Dispose()
    }
}

function Find-FastbootProduct {
    $devices = Invoke-PlatformTool $fastboot @('devices')
    $connected = @($devices -split '\r?\n' | Where-Object { $_ -match '\sfastboot\s*$' })
    if (!$connected.Count) { return '' }
    if ($connected.Count -ne 1) { throw 'Connect only one phone in fastboot.' }
    $product = Invoke-PlatformTool $fastboot @('getvar', 'product')
    if ($product -match '(?m)^\s*(?:\(bootloader\)\s*)?product:\s*(\S+)\s*$') {
        return $Matches[1]
    }
    throw 'Fastboot did not give the device name.'
}

function Wait-FastbootProduct([string]$Expected, [int]$Seconds) {
    $timer = [System.Diagnostics.Stopwatch]::StartNew()
    do {
        try {
            $product = Find-FastbootProduct
        } catch {
            $product = ''
        }
        if ($product -eq $Expected) { return }
        Start-Sleep -Seconds 1
    } while ($timer.Elapsed.TotalSeconds -lt $Seconds)
    throw "$Expected mode not seen yet. Check the phone's screen and the USB cable."
}

function Prepare-Payload([string]$Path) {
    $bytes = [IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -lt 32 -or [BitConverter]::ToUInt32($bytes, 0) -ne 0x50554E58 -or
        [BitConverter]::ToUInt32($bytes, 4) -ne $bytes.Length - 8) {
        throw 'run-payload.bin is damaged or not a payload.'
    }

    $clockOffset = -1
    $entries = @{}
    for ($offset = 8; $offset + 24 -le $bytes.Length;) {
        $name = [Text.Encoding]::ASCII.GetString($bytes, $offset, 16).Trim([char]0)
        if (!$name) { break }
        $length = [BitConverter]::ToUInt32($bytes, $offset + 16)
        if ($length -gt $bytes.Length - $offset - 24 -or $entries.ContainsKey($name)) {
            throw 'run-payload.bin has invalid entries.'
        }
        $entries[$name] = $length
        if ($name -eq 'cmdline') {
            $settings = [Text.Encoding]::ASCII.GetString($bytes, $offset + 24, $length).Trim([char]0)
            $arguments = $settings -split '\s+'
            $allowedKeys = @('sb_delay','cpus','bootcpu','cpufreq','touchscreen','gpu','adsp','sound',
                'wifi','haptic','socinfo','thermal','battery_stop','pmic','battery','fb_hole',
                'gpu_push_ms','munch_torch','panel','panel_level','wifi_mac','fb_backlight','fb_hz',
                'gpu_recover','loader_return_after','cam','epoch')
            $keys = @($arguments | ForEach-Object { ($_ -split '=', 2)[0] })
            if ($Bluetooth) { $allowedKeys += 'munch_bt' }
            if (@($keys | Where-Object { $_ -notin $allowedKeys }).Count -or
                @($keys | Sort-Object -Unique).Count -ne $keys.Count) {
                throw 'The payload has options outside the tested POCO F4 configuration.'
            }
            foreach ($required in @('cpus=8','gpu=1','wifi=1','fb_backlight=1','battery_stop=15','cam=5','munch_torch=3')) {
                if ($required -notin $arguments) { throw "$required is missing from the payload." }
            }
            $clock = [regex]::Match($settings, '(?:^|\s)epoch=(\d{10})(?=\s|$)')
            if (!$clock.Success) { throw 'The boot clock (epoch=) is missing from the payload.' }
            $clockOffset = $offset + 24 + $clock.Groups[1].Index
            if ($Bluetooth) {
                $timer = [regex]::Match($settings, '(?:^|\s)loader_return_after=(\d+)(?=\s|$)')
                if ($timer.Success) {
                    $digits = $timer.Groups[1]
                    $zeros = [Text.Encoding]::ASCII.GetBytes('0' * $digits.Length)
                    [Array]::Copy($zeros, 0, $bytes, $offset + 24 + $digits.Index, $zeros.Length)
                }
            }
        }
        $offset += 24 + 16 * [int][Math]::Ceiling($length / 16.0)
    }
    if ($clockOffset -lt 0 -or !$entries['kernel'] -or !$entries['ramdisk'] -or $entries.ContainsKey('resident')) {
        throw 'The payload must hold a kernel, the small ramdisk and the boot arguments.'
    }

    $now = [Text.Encoding]::ASCII.GetBytes([DateTimeOffset]::UtcNow.ToUnixTimeSeconds().ToString())
    if ($now.Length -ne 10) { throw "Windows' clock is outside the supported range." }
    [Array]::Copy($now, 0, $bytes, $clockOffset, $now.Length)
    return ,$bytes
}

$temporaryPayload = $null
$temporaryMap = $null
$ipaWork = $null
try {
    if ($InstallIPA -and ($Storage -or $Bluetooth)) { throw 'Choose one boot mode.' }
    $fastboot = Find-PlatformTool 'fastboot.exe'
    $loader = Join-Path $PSScriptRoot 'xnu-usb.img'
    $system = Join-Path $PSScriptRoot 'system.apfs'
    $source = Join-Path $PSScriptRoot 'run-payload.bin'
    if ($Bluetooth) {
        if ($Storage) { throw 'Choose one boot mode.' }
        $source = Join-Path (Split-Path -Parent $PSScriptRoot) 'logs/munch-camera/focus-bt-qs-test.bin'
    }
    if ($Storage) {
        $project = Get-WslProject
        $build = Join-Path (Split-Path -Parent $PSScriptRoot) 'logs/storage-boot'
        $loader = Join-Path $build 'xnu-storage-usb.img'
        $bootstrap = Join-Path $build 'bootstrap.apfs'
        $probe = Join-Path $build 'storage-map-probe'
        foreach ($path in @($bootstrap, $probe, (Join-Path $build 'hakios-kernel'))) {
            if (!(Test-Path -LiteralPath $path -PathType Leaf)) { throw "Storage boot file not found: $path" }
        }
    }
    foreach ($path in @($loader, $system, $source)) {
        if (!(Test-Path -LiteralPath $path -PathType Leaf)) { throw "$([IO.Path]::GetFileName($path)) is missing from the launcher folder." }
    }
    $payload = Prepare-Payload $source
    if ($InstallIPA -or $LiveIPA) {
        $build = Join-Path (Split-Path -Parent $PSScriptRoot) 'logs/ipa-test'
        if (!(Test-Path -LiteralPath (Join-Path $build 'munch_ipa_install') -PathType Leaf)) {
            throw 'The IPA installer is not built yet.'
        }
    }
    if ($Check) {
        Write-Host 'OK: platform tools and boot files are here.'
        exit 0
    }

    if ($InstallIPA -or $LiveIPA) {
        if ($LiveIPA) {
            $phoneIP = Read-Host "HakiOS's Wi-Fi IP (Settings > Wi-Fi > the network's info)"
            if ($phoneIP -notmatch '^\d{1,3}(\.\d{1,3}){3}$') { throw "Enter the phone's IPv4 address." }
        }
        Add-Type -AssemblyName System.Windows.Forms
        $dialog = New-Object System.Windows.Forms.OpenFileDialog
        try {
            $dialog.Title = 'Choose an IPA to install in the HakiOS session'
            $dialog.Filter = 'iOS apps (*.ipa)|*.ipa'
            if ($dialog.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) { exit 0 }
            if ((Get-Item -LiteralPath $dialog.FileName).Length -gt 32MB) { throw 'The IPA limit for this test is 32 MiB.' }
            $tag = [Guid]::NewGuid().ToString('N')
            $ipaWork = New-Item -ItemType Directory -Path (Join-Path $build $tag)
            Copy-Item -LiteralPath $dialog.FileName -Destination (Join-Path $ipaWork.FullName 'test.ipa')
        } finally { $dialog.Dispose() }
        $temporaryPayload = Join-Path $ipaWork.FullName 'payload.bin'
        $relative = "logs/ipa-test/$tag"
        Push-Location -LiteralPath $env:windir
        try {
            if ($LiveIPA) {
                & "$env:windir\System32\wsl.exe" -d Ubuntu --cd (Get-WslProject) -- python3 `
                    tools/munch-ipa-payload.py --ipa "$relative/test.ipa" --ssh-keys logs/ipa-test/ssh --live $phoneIP
                if ($LASTEXITCODE -ne 0) { throw 'The live install failed. See the SSH or installer messages above.' }
                exit 0
            }
            Write-Host 'Preparing the IPA and the small image for a RAM boot...'
            & "$env:windir\System32\wsl.exe" -d Ubuntu --cd (Get-WslProject) -- python3 `
                tools/munch-ipa-payload.py --ipa "$relative/test.ipa" --base win-boot/run-payload.bin `
                --root logs/munch-camera/switch-bt-root.KxqQbe --installer logs/ipa-test/munch_ipa_install `
                --ssh-keys logs/ipa-test/ssh --output "$relative/payload.bin"
            if ($LASTEXITCODE -ne 0) { throw 'Making the IPA payload failed.' }
        } finally { Pop-Location }
        [IO.File]::WriteAllBytes($temporaryPayload, (Prepare-Payload $temporaryPayload))
    }

    Write-Host '[1/4] Looking for the POCO F4 on USB...'
    $product = Find-FastbootProduct
    if ($Storage) {
        if ($product) { throw 'For a storage boot, start from Android, so the block map is read again before the reboot.' }
        $adb = Find-PlatformTool 'adb.exe'
        $device = Invoke-PlatformTool $adb @('-d', 'shell', 'getprop ro.product.device')
        if ($device -ne 'munch') { throw "This launcher is only for the POCO F4 (munch); this phone is '$device'. Nothing was done to it." }
        if ((Invoke-PlatformTool $adb @('-d', 'shell', 'su -c id')) -notmatch '\buid=0\(') { throw 'Allow root for Shell/ADB (KernelSU or Magisk).' }
        $null = Invoke-PlatformTool $adb @('-d', 'push', $probe, '/data/local/tmp/hakios-storage-boot-map') 30
        $null = Invoke-PlatformTool $adb @('-d', 'shell', 'chmod 700 /data/local/tmp/hakios-storage-boot-map')
        $map = Invoke-PlatformTool $adb @('-d', 'shell', "su -c '/data/local/tmp/hakios-storage-boot-map --map'") 30
        $id = [Guid]::NewGuid().ToString('N')
        $mapName = "map-$id.txt"
        $payloadName = "payload-$id.bin"
        $temporaryMap = Join-Path $build $mapName
        $temporaryPayload = Join-Path $build $payloadName
        [IO.File]::WriteAllText($temporaryMap, $map, [Text.Encoding]::ASCII)
        Write-Host 'Preparing the checksums and the small payload on the PC...'
        # WSL parses its switches from the Windows command line. Start from a
        # Windows path so it needn't translate the drive mapped by pushd.
        Push-Location -LiteralPath $env:windir
        try {
            & "$env:windir\System32\wsl.exe" -d Ubuntu --cd $project -- python3 `
                tools/munch-storage-payload.py --map "logs/storage-boot/$mapName" --system win-boot/system.apfs `
                --kernel logs/storage-boot/hakios-kernel --base win-boot/run-payload.bin --output "logs/storage-boot/$payloadName"
            if ($LASTEXITCODE -ne 0) { throw 'Making the storage payload in WSL failed.' }
        } finally {
            Pop-Location
        }
    }
    if (!$product) {
        $adb = Find-PlatformTool 'adb.exe'
        $device = Invoke-PlatformTool $adb @('-d', 'shell', 'getprop', 'ro.product.device')
        if (!$device) { throw 'No phone in Android on adb, and none in fastboot. Put the POCO F4 in fastboot (Power + Volume Down) or start it in Android with USB debugging on.' }
        if ($device -ne 'munch') {
            throw "This launcher is only for the POCO F4 (munch); this phone is '$device'. Nothing was done to it."
        }
        try {
            $null = Invoke-PlatformTool $adb @('-d', 'reboot', 'bootloader') 15
        } catch {
            # USB can disappear before adb returns. Confirm fastboot below.
            Write-Host 'Android disconnected; waiting for fastboot...'
        }
        Wait-FastbootProduct 'munch' 75
        $product = 'munch'
    }
    if ($product -eq 'munch') {
        Write-Host '[2/4] Booting Mu-Silicium into RAM...'
        for ($attempt = 1; $attempt -le 2; $attempt++) {
            $bootError = $null
            try {
                Write-Host (Invoke-PlatformTool $fastboot @('boot', $loader) 20)
            } catch {
                $bootError = $_.Exception.Message
                if ($bootError -notmatch 'Status read failed|did not answer') { throw }
                Write-Host 'USB dropped during the boot. Checking whether Mu-Silicium came up...'
            }
            try {
                Wait-FastbootProduct 'xnuloader' 30
                break
            } catch {
                if ($attempt -eq 2 -or !$bootError -or (Find-FastbootProduct) -ne 'munch') {
                    if ($bootError) { throw "$bootError`n$($_.Exception.Message)" }
                    throw
                }
                Write-Host 'The phone is still in its own fastboot. Booting once more...'
            }
        }
    } elseif ($product -ne 'xnuloader') {
        throw "This launcher is only for the POCO F4 (munch); the phone in fastboot is '$product'. Nothing was done to it."
    } else {
        Write-Host '[2/4] Mu-Silicium is already running.'
    }

    $state = Invoke-PlatformTool $fastboot @('getvar', 'result')
    if ($state -match 'result:\s+running\b') {
        if ($InstallIPA) { throw 'End the running HakiOS session before booting a build with an IPA.' }
        if ($Bluetooth) { throw 'A HakiOS session is still running. End it, or go to fastboot, before choosing the Bluetooth build.' }
        Write-Host 'HakiOS is already running.'
        exit 0
    }
    if ($state -notmatch '(?m)result:\s+(?:idle|returned\s+\d+\s+0)\s*$') {
        throw "The loader is not ready. Go back to the phone's own fastboot and run the launcher again."
    }

    $rootImage = if ($Storage) { $bootstrap } else { $system }
    if ($Storage) { Write-Host "[3/4] Uploading the small bootstrap. The system files are read from the phone's storage." }
    else { Write-Host '[3/4] Uploading the system into RAM, usually about 100 seconds...' }
    $null = Invoke-PlatformTool $fastboot @('oem', 'ramdisk')
    Write-Host (Invoke-PlatformTool $fastboot @('stage', $rootImage) 240)

    Write-Host '[4/4] Starting HakiOS...'
    if (!$Storage -and !$InstallIPA) {
        $temporaryPayload = [IO.Path]::GetTempFileName()
        [IO.File]::WriteAllBytes($temporaryPayload, $payload)
    }
    $null = Invoke-PlatformTool $fastboot @('stage', $temporaryPayload) 45
    if ($Storage) {
        Write-Host (Invoke-PlatformTool $fastboot @('oem', 'storagecheck') 15)
    }
    $null = Invoke-PlatformTool $fastboot @('oem', 'run')
    Start-Sleep -Seconds 2
    $state = Invoke-PlatformTool $fastboot @('getvar', 'result')
    if ($state -notmatch 'result:\s+running\b') { throw "The kernel did not start. $state" }
    if ($Storage) {
        Write-Host "The kernel started. Watching iOS's startup log over USB..."
        $bootLog = Join-Path $build ('startup-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff') + '.log')
        $nextLog = "$bootLog.next"
        $timer = [Diagnostics.Stopwatch]::StartNew()
        do {
            try {
                $null = Invoke-PlatformTool $fastboot @('oem', 'log') 5
                $null = Invoke-PlatformTool $fastboot @('get_staged', $nextLog) 10
                Move-Item -LiteralPath $nextLog -Destination $bootLog -Force
                if (Select-String -LiteralPath $bootLog -Pattern 'systemShellDidFinishLaunching|Dismissing overlay: shell launched' -Quiet) {
                    Write-Host 'SpringBoard has started. Check the Home screen on the phone.'
                    exit 0
                }
                $state = Invoke-PlatformTool $fastboot @('getvar', 'result') 5
            } catch {
                throw "USB stopped answering before iOS's startup was confirmed. Last log (if any): $bootLog"
            }
            if ($state -notmatch 'result:\s+running\b') { throw "The kernel stopped before iOS's startup was confirmed. Log: $bootLog" }
            Start-Sleep -Seconds 2
        } while ($timer.Elapsed.TotalSeconds -lt 60)
        throw "iOS's startup was not confirmed within 60 seconds. Log: $bootLog"
    }
    Write-Host 'The kernel is running. iOS appears in about 30-45 seconds.'
    exit 0
} catch {
    Write-Host ("Boot stopped: " + $_.Exception.Message) -ForegroundColor Red
    exit 1
} finally {
    if ($temporaryPayload) { Remove-Item -LiteralPath $temporaryPayload -Force -ErrorAction SilentlyContinue }
    if ($temporaryMap) { Remove-Item -LiteralPath $temporaryMap -Force -ErrorAction SilentlyContinue }
    if ($ipaWork) {
        Remove-Item -LiteralPath (Join-Path $ipaWork.FullName 'test.ipa') -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $ipaWork.FullName -ErrorAction SilentlyContinue
    }
}
