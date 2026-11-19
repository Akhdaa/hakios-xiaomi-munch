# POCO F4 (munch, Snapdragon 870 / SM8250-AC)

HakiOS on the Xiaomi POCO F4: the XNU kernel with the iOS 16.5 user space, started from RAM with
`fastboot boot`. Nothing is flashed and no partition of the phone is written; a reboot brings Android back
as it was. This is a bring-up for people who want to work on it, not a system for daily use.

Status as of 11 November 2026 (Metal updated 18 November). "Works" means seen working on the phone, not only built.

## What works

| Part | State | Notes |
|---|---|---|
| Boot | works | Mu-Silicium UEFI and the loader by `fastboot boot`, the system image uploaded to RAM (about 100 s for 2.5 GB); several kernels in one loader session without uploading again |
| Display | works | 1080 x 2400 at 60 Hz, with the camera hole kept black. Settings and Control Center change the physical DCS brightness with `panel=2 fb_backlight=1` and rebuilt userland. The normal range is 1–2047. The panel sleeps when blanked and wakes with the power key |
| Touch | works | FocalTech FT3658U on SPI, 10 fingers |
| Buttons | works | power and volume |
| CPU | works | all 8 cores; each group's clock follows its load (`cpugov`, like schedutil with the device tree's frequency tables): idle 883 / 825 / 844 MHz, up to 1804 / 2419 / 2745 MHz; lower caps while the screen is off |
| GPU | works | Adreno 650 through Mesa (OpenGL ES); the clock follows the load between 305 and 670 MHz (`gpu_dcvs`). The Metal backend passed copy/fill, compute and a sysmem offscreen triangle. Isolated Metal.framework copy/fill, compute and offscreen render passed |
| Metal desktop | experimental | With `metal=1` (not the default) the CoreAnimation desktop draws through Metal on the A650 in a RAM test (17 November). Shader linkage no longer refuses pipelines and backboardd stays up. Each display surface is drawn into a driver-owned target and copied whole at each commit; by the owner's eyes this removed the duplicated/offset rows and black bands while moving. `metal_blit=0` turns that copy off. On 18 November, with a stage cache seeded on the host (`metal_cache=0` off), a damaged-region copy (`metal_damage=0` off) and `gpu_anim_floor=587`, the owner found it smoother but still a bit choppy. Reachability still shows a wrong (pink) area at the top; being fixed. Stand-ins now keep the same effect (`metal_ca_nearest=old` gives the old rule back). The real cameras (OV64B40, IMX596) and Bluetooth (460800 baud, a headset connected) work alongside Metal. GMEM and UBWC are off (`metal_noubwc=1`) |
| Wi-Fi | works | QCA6390 on PCIe 0 with the phone's own firmware; joins a network from Settings |
| Sound | works | both speakers (TFA9874) through the audio DSP |
| Haptics | works | Awinic AW86927 |
| Battery | works | level, voltage, current and temperature from the fuel gauge, shown in the status bar. Charging works with `munch_charge=1` (19 November, RAM test): the PM8150B charger's watchdog is petted (its bite stopped charging before), and on a wall charger or charging port the input limit goes to 1500 mA and the battery current to 1800 mA, only through the charger channels this system owns. On a wall charger the level rose from 55 to 58 % in about 5 minutes; from a computer's port (500 mA) it charges slowly. `munch_charger_look=1` prints the charger's registers once, read only |
| Auto-Lock | works | the kernel follows the Settings timeout via SpringBoard. Changing 1 Minute to 30 Seconds took effect in the same RAM session and locked after 30 s of inactivity. Dimming before lock is still open |
| Sileo | works | with the `JB=1` image (Procursus bootstrap); SSH is installed but not tried |
| Camera | partial | Real OV64B40 wide and IMX596 front previews work in Camera.app in the RAM test. With Bluetooth disabled, first frames arrived in about 0.67 s when switching to front and 2.2–2.4 s for wide. Wide digital zoom can stutter. DW9800V autofocus builds but still needs a hardware test. OIS and exposure compensation are unverified. OV8856 ultrawide answers the ID probe. The iPhone X camera configuration currently exposes wide and front only |
| Flashlight | works | Control Center switches the PM8150L torch on and off and adjusts its level (`munch_torch=3 cam=5 cam_fake=1`). On 10 November the USB log confirmed four levels, 37 to 150 mA, and off at level 0. Timeout and loader return are still to check |
| Thermal | partial | 25 sensors read; the prime core and the GPU are slowed when hot; the stop at 105 degrees has not been seen |
| Microphone | not yet | the driver is built; switching it on (`mic=1`) breaks touch, so it is off |
| Cellular | not yet | the Snapdragon X55 is found on PCIe 2 (link gen 3 x2), but its boot ROM refuses the boot loader over BHI (error `0xef0e0003`); no SIM, signal or data |
| Bluetooth | partial | QCA6390 on QUP0 SE6. Settings discovers and connects Classic Bluetooth earbuds. A2DP AAC-LC played smoothly at 460800 baud. The Control Center tile and CoreBluetooth discovery are still open |
| Deep sleep | not yet | idle cores wait in WFI; no power collapse, no system suspend |
| Passcode, Apple ID, Face ID | not yet | there is no Secure Enclave |
| Persistent data | by design not | everything lives in RAM; a reboot loses it |

Known problems: the music panel on the lock screen flickers (its process, MediaRemoteUI, crashes and is
started again); a tear line can show while things move fast; a run that ends on a core other than the boot
core used to hang and is believed fixed.

## What you need

- A POCO F4 with an unlocked bootloader, USB debugging on, and a PC with adb and fastboot.
- The iOS 16.5 (20F66) IPSW for the iPhone X, which the build takes the user space from. It is not in
  this repository and must not be added to it.
- The phone's own firmware, taken from your own phone by `devices/munch/pull-firmware.sh` into `fw-*/`
  (GPU, Wi-Fi, Bluetooth, audio DSP, haptics, camera tables, modem). It is the vendor's: never commit it.
- The UEFI binaries from your own phone's `xbl`, put under `devices/munch/uefi/files/Binaries/` (`devices/munch/uefi/apply.sh` builds the UEFI with them).

## Running it

```sh
make munch_defconfig DEVICE=munch
make kernel JOBS=12
(cd userland && GL=1 REST=1 ./build.sh)
./xl ramdisk userland/system.apfs      # once per loader session
XL_SECS=1800 ./xl run                  # the kernel's arguments come from the .config
```

`tools/mk-winboot.sh` makes a folder with `boot-ios.bat` for starting it from Windows without WSL.
The general guide is `docs/getting-started/`; every kernel argument is in `docs/boot-args.md`.

## Bluetooth test image

Build the munch kernel and the small userland tree first, then run
`bash tools/munch-bt-test-image.sh`. The script builds
`logs/munch-bt-ios/ios-bluetooth-payload.bin` for `munch_bt=3` and embeds the
small APFS with the QCA6390 firmware, the stock Bluetooth launch jobs, and a
CoreBluetooth scan probe. Use the existing `userland/system.apfs` as the
resident system image. Start the first test from a fresh Mu-Silicium boot.

The first builds reached CoreBluetooth state 5 about 50 seconds after kernel start.
The 14 November build starts Bluetooth after Wi-Fi radio bring-up and logged
`iOS H4BC transport ready` in the first 12-second snapshot. That build caused
severe system lag. An idle wait in the UART worker builds but is untested. The controller accepts legacy BLE scan
parameters, start and stop, and sends advertising events. The CoreBluetooth
probe received zero devices. Classic discovery in Settings connected TWS
earbuds and opened an AAC-LC A2DP streaming channel.
Use `MUNCH_BT_AUDIO=1` to stage the stock Bluetooth audio plug-in from your own
local `sysimage/rootfs.dmg`. `MUNCH_BT_MODE=4` selects the experimental 460800
baud bridge, verified on the phone with AAC-LC playback. Mode 3 retains the
115200 baud baseline. `MUNCH_BT_SCAN=0` leaves out the automatic scan probe
and verbose Bluetooth daemon logs.
The image returns to the loader after five minutes. Previous warm kernel
restarts sometimes hung while configuring the UART.

## Rules of this port

- Only `fastboot boot` and RAM. No `fastboot flash`, no writes to any partition.
- The modem's file systems, when cellular gets that far, are to be served read-only, with the modem's
  writes kept in memory.
- Serial numbers, MAC addresses, IMEI and firmware stay out of the repository.

---

The rest of this file was made by `tools/new-device.py` from the phone's device tree.

## The device tree's numbers

Made by `tools/new-device.py` on 2026-11-06, from the file munch-live.dtb. Edit it as the port goes.

| What | Value |
|---|---|
| Board (the tree's `model`) | Qualcomm Technologies, Inc. xiaomi munch |
| Compatible | `qcom,kona-mtp qcom,kona qcom,mtp` |

## The SoC addresses

No reference tree to compare with (devices/redwood/prebuilt/device-tree.dtb is missing).

| Argument | Value | Against the reference |
|---|---|---|
| `soc_ipa_base` | `0x1e40000` |  |
| `soc_gsi_base` | `0x1e04000` |  |
| `soc_ipcc` | `0x408000` |  |
| `soc_aoss` | `0xc300000` |  |
| `soc_rsc` | `0x18220000` |  |
| `soc_apps_smmu` | `0x15000000` |  |
| `soc_cmd_db` | `(not found)` | **missing**: the tree has no node this was read from; see the table in devices/redwood/README.md |
| `soc_mpss_mem` | `(not found)` | **missing**: the tree has no node this was read from; see the table in devices/redwood/README.md |
| `modem_ipa_fw_at` | `(not found)` | **missing**: the tree has no node this was read from; see the table in devices/redwood/README.md |
| `soc_smem` | `0x80900000` |  |
| `soc_imem` | `0x14600000` |  |
| `soc_ipa_stream` | `0x5c0` |  |
| `soc_ipa_stream_uc` | `0x5c2` |  |
| `soc_pas_mpss` | `(not found)` | **missing**: the tree has no node this was read from; see the table in devices/redwood/README.md |
| `soc_pas_ipa` | `0xf` |  |
| `soc_ipa_fw` | `ipa_fws` |  |

`soc.args` is what `tools/soc-args.py` read out of the tree; `pull-soc.sh` makes it again from the phone on
adb. The run scripts add it to the kernel command line. Where each argument comes from in the tree:
`devices/redwood/README.md`.

## What is this phone's own

The kernel, loader and user space know nothing about a phone: addresses, interrupts, rails and firmware
come from its tree and its Android. What is written for one phone is the list below. The head of each file
says where its numbers came from; that is the map of what to rewrite.

- [ ] **UEFI**: the firmware that starts the loader: devices/redwood/uefi/ is a port of Mu-Silicium; the vendor's drivers are extracted from your own phone (uefi/README.md)
- [ ] **Display, buttons, touch**: drivers/input/hakios.input.c, hakios.buttons.c, hakios.goodix.c (the reference phone's controller; another phone has another)
- [ ] **Storage**: hakios.ufs.c and hakios.ufshc.c (UFS); a phone with eMMC needs another driver
- [ ] **CPU clocks**: hakios.cpufreq.c
- [ ] **GPU**: hakios.adreno.c (Adreno 642L here: another generation needs its own firmware and registers)
- [ ] **Wi-Fi**: hakios.wifi.c, hakios.lx.c and wifi/lx/ (the WCN6750 radio of the reference phone)
- [ ] **Audio and battery**: hakios.adsp.c, hakios.audio.c, hakios.sound.c, hakios.speaker.c, hakios.mic.c, hakios.battery.c
- [ ] **Cellular**: hakios.mpss.c, hakios.qmi.c, hakios.nas.c, hakios.ipa.c, hakios.cell.c; soc.args carries the addresses, the IPA layout is for IPA 4.11 (docs/cellular.md)
- [ ] **Haptics and cameras**: hakios.haptic.c, hakios.camera.c

## Configuration

The kernel's settings are layered the way an Android kernel's are: the common kernel, the SoC's, this phone's
(`docs/configs.md`). Two short files make this phone a configuration:

    configs/munch.config          CONFIG_DEVICE="munch"   and whatever is true of this phone only
    configs/munch_defconfig       #include common_defconfig
                                   #include <the SoC's fragment: sc7280.config for the SC7280 family>
                                   #include munch.config

Then `make munch_defconfig`. Turn on only what this phone has been shown to do; `tools/hakios-config.py check` says what the
choice needs.

The drivers are part of the configuration too (`docs/drivers-config.md`): every one is built unless a configuration leaves it out,
and the addresses, pins and rails in each are the reference phone's. Leave out in `configs/munch.config` what has not been
ported to this phone yet (`# CONFIG_HAKIOS_GOODIX is not set`, ...; `configs/qemu_defconfig` leaves out all of them), and put
each one back when it has been made to work here. `tools/hakios-config.py show` lists what is built in.

## Firmware

Pulled from your own phone, never committed: `devices/redwood/pull-firmware.sh`, or
`pull-firmware-root.sh` where the firmware is readable by root only. Check with `make doctor`.

## Before you open a pull request

`tools/provenance-check.py` must pass: no vendor firmware, no UEFI drivers, no Apple files in the tree.

## The repository

A HakiOS device repository: what is true of this device and of no other. The kernel, the loader, the drivers and the tools are
the common repository's; the sm8250 family's facts are in `soc/`, carried with the device. How the three are put together:
[docs/porting/repositories.md](https://github.com/oosexgang/hakios-xnu-kernel/blob/main/docs/porting/repositories.md).

| File | What |
|---|---|
| `metal.args` | what `tools/metal-run.sh` gives this phone: its cores, its panel's rate, the threads that copy frames, the touch poll |
| `device.conf` | what the device is, and how far each part of it works: its page in the documentation is made from it |
| `configs/munch_defconfig` | the common kernel, the SoC's fragment, this device's: `make munch_defconfig` |
| `configs/munch.config` | what is this device's own: the drivers built in, the arguments it needs |
| `soc/` | its SoC family's layer (sm8250): `soc.conf`, `configs/sm8250.config`, the SoC's drivers; carried here so that this one repository is all the device needs |
| `drivers/` | drivers of its own, if it needs any (`drivers/<subsystem>/conf`, as in the common repository) |

Every phone driver of the common repository is left out to begin with: their addresses and pins are the phone they were written
for. Put one back in `configs/munch.config` when it has been made to work here, and say so in `device.conf`.

Check with `tools/hakios-device.py check munch`.
