# HakiOS for the POCO F4 (`munch`)

HakiOS on the **Xiaomi POCO F4** (`munch`, also sold as the Redmi K40S): Qualcomm Snapdragon 870 (**SM8250-AC**),
Adreno 650, UFS. Apple's open-source Darwin kernel (XNU, xnu-8796.121.2) with an iOS 16.5 user space, built on
[HakiOS](https://github.com/oosexgang/hakios-xnu-kernel).

It runs from RAM: the phone is started with `fastboot boot`, nothing is flashed, no partition is written, and a
reboot gives the phone back to Android. This is a research bench with known defects, not a daily system.

**This branch.** `SM8250-AC` holds the port: HakiOS's tree with the device under `devices/munch/` and the changes
the port needed in the common tree. `main` is upstream as it was forked.

## What works

"Works" means seen working on the phone in a RAM run, not only built. Details and measurements, part by part:
[devices/munch/README.md](devices/munch/README.md).

| Part | State | In short |
|---|---|---|
| Boot | works | Mu-Silicium UEFI and the loader by `fastboot boot`; the system image uploaded to RAM (about 100 s); several kernels per loader session |
| Display | works | 1080 x 2400 at 60 Hz; physical brightness from Settings and Control Center (`panel=2 fb_backlight=1`); the panel sleeps when the screen is off |
| Touch | works | FocalTech FT3658U on SPI, 10 fingers |
| Buttons | works | power and volume |
| CPU | works | all 8 cores; each group's clock follows its load (`cpugov`, schedutil-like with the device tree's frequency tables); lower caps with the screen off |
| GPU | works | Adreno 650 through Mesa (OpenGL ES), the default; clock follows the load between 305 and 670 MHz (`gpu_dcvs`) |
| Metal | partial | with `metal=1` the desktop (CoreAnimation) draws through hakios-metal on the A650; all 60 uber-shader configurations seen are specialised up front, and without the diagnostic logging (`metal_fps`) most frames come at 60 fps, with occasional hitches from the CPU copy to the panel. One Reachability area is pink: its memoryless attachment is loaded DontCare and keeps old memory. Compute, copy/fill and offscreen render through Metal.framework pass |
| Wi-Fi | works | QCA6390 on PCIe 0 with the phone's own firmware; joins a network from Settings |
| Bluetooth | works | QCA6390 on QUP0 SE6, on by default (`munch_bt=4`, bluetoothd and BlueTool in the standard image); earbuds pair and connect from Settings and play A2DP (AAC-LC) |
| Sound | works | both speakers (TFA9874) through the audio DSP |
| Microphone | works | WCD938x ADC1 over SoundWire into decimator 0 (`mic=4`): Voice Memos records |
| Camera | partial | OV64B40 wide and IMX596 front preview in Camera.app; the OV8856 ultra-wide is not started |
| Flashlight | works | from Control Center, with its levels (`munch_torch=3`) |
| Haptics | works | Awinic AW86927 |
| Sensors | works | the sensor DSP (`slpi=1`): ambient light, accelerometer, gyroscope, compass and rotation; the screen turns, and CoreMotion and the compass answer System Check and Compass. Auto-Brightness not yet |
| Battery | works | level, voltage, current and temperature in the status bar; charges with `munch_charge=1` (charger watchdog petted, input 1500 mA, battery 1800 mA): 55 to 58 % in about 5 minutes on a wall charger; slow from a computer's port (500 mA) |
| Auto-Lock | works | follows the Settings timeout; no dimming before the lock |
| Thermal | partial | 25 sensors read; the prime core and the GPU slowed when hot; the stop at 105 °C not yet seen |
| Cellular | partial | the Snapdragon X55 boots to its mission mode: boot loader over BHI, the rest of its firmware over Sahara, its services over QRTR (DMS online, NAS answers), its carrier configurations over TFTP; it restarts itself about a minute later, before registering: no signal, SIM, Cellular pane or data yet |
| Deep sleep | no | idle cores wait in WFI; no power collapse, no system suspend |
| Passcode, Apple ID, Face ID | no | there is no Secure Enclave |
| Sileo | works | with the `JB=1` image (Procursus bootstrap) |
| Data across reboots | no, by design | everything lives in RAM |

Known problems: the music panel on the lock screen flickers (MediaRemoteUI crashes and is started again); a tear
line can show while things move fast.

## What is here

| Path | What |
|---|---|
| `devices/munch/` | the device: drivers (display, touch, camera, torch, modem, Wi-Fi, audio, Bluetooth), configuration, UEFI platform sources, firmware script |
| `hakios-metal/` | the Metal driver for the Adreno, through the kernel's GPU driver, with its host tests |
| `userland/src/HakiosMetalCA.m` | the CoreAnimation plug-in that puts the desktop on Metal |
| `drivers/`, `xnuloader/`, `xnu/`, `userland/`, `tools/` | HakiOS's common tree with the changes this port needed |

## Not here, by design

- No file of iOS. The user space is laid out from an iPhone X IPSW (iOS 16.5, 20F66) you get yourself.
- No firmware of Qualcomm's or Xiaomi's. It comes off your own phone (`devices/munch/pull-firmware.sh`).
- No UEFI binaries or device tree taken from a phone in the repository, no serial numbers, MAC addresses, IMEI or
  calibration data. (The kit on the Releases page carries a Mu-Silicium build for munch, as hakios-alioth's kit does.)
- No keys, no activation, no Apple services (Apple ID, iCloud, App Store), no DRM circumvention.
- No writes to the phone's storage: no `fastboot flash`, no partition written. The modem's file systems, once
  cellular gets that far, are to be served read only.

## The phone it is developed on

This port is developed and tested on one phone, and parts of it are still fitted to that phone rather than read from
yours:

| | |
|---|---|
| Model | POCO F4, `22021211RG` (Global), `hwc=GLOBAL` |
| Memory, storage | 8 GB RAM, 256 GB UFS |
| Android on it | HyperOS `OS4.0.24.0`, a port by `@Jnt100` (the build of 2026-02-11; not Xiaomi's own build for munch) |
| Modem firmware | `2.5.1.c1-14.1-0324_0657_6488b87c` (Android's baseband version) |
| `reserved-memory` | as in [`prebuilt/device-tree.dtb`](devices/munch/prebuilt/device-tree.dtb): `removed_region@80b00000` (0x5300000 long), then `pil_gpu_region@8681a000`, `pil_slpi_region@88c00000`, `pil_adsp_region@8bb00000`, `cont_splash_region@9c000000` among the others |

What is fitted to this phone so far:

- The device tree the boot stage hands the kernel is this phone's (`devices/munch/prebuilt/device-tree.dtb`), not
  your phone's. With it are the Mu-Silicium build it boots (`prebuilt/SILICIUM_UEFI.fd`, `prebuilt/BootShim.bin`).
- Three drivers put their firmware at addresses taken from this phone's `reserved-memory`: the GPU's zap shader
  (`pil_gpu_region`, `0x8681a000`), the sensor DSP (`pil_slpi_region`, `0x88c00000`) and the audio DSP
  (`pil_adsp_region`, `0x8bb00000`). Firmware can move these regions: if the kernel stops early or a part does not
  start on your phone, compare yours. Taking them from your own phone is planned.
- The display: Silicium's framebuffer at `0x9c000000` (`cont_splash_region`; 1080 x 2400, 4320 bytes a row).

To compare yours, in Android: `adb shell ls /proc/device-tree/reserved-memory/` lists the regions with their
addresses (no root needed); with root, `adb shell su -c 'cat /sys/firmware/fdt' > fdt.bin` gives the whole tree
(`dtc -I dtb -O dts fdt.bin`). Don't share that file as it is: its `/chosen` node has your phone's serial number.

**If you try it on your own phone and something goes wrong, please
[open an issue](https://github.com/Akhdaa/hakios-xiaomi-munch/issues).** Say which phone it is (model, RAM, region,
ROM and firmware version) and what happened, and attach your `reserved-memory` list. Leave out anything personal:
the serial number, IMEI, Wi-Fi names and passwords, Bluetooth and MAC addresses.

This section follows [xiongnemo/hakios-alioth](https://github.com/xiongnemo/hakios-alioth)'s.

## The kit (the easy way)

[Releases](https://github.com/Akhdaa/hakios-xiaomi-munch/releases) has a **munch kit**, like
[xiongnemo/hakios-alioth](https://github.com/xiongnemo/hakios-alioth)'s: the prebuilt kernel, our userland and a
Mu-Silicium build. `assemble.bat` (Windows with WSL) or `assemble.sh` makes the rest on your computer: it downloads the
iOS 16.5 IPSW from Apple, copies the firmware from your own phone (root, read only), optionally adds Sileo (`JB=1`), and
writes an `ios-munch` folder with `start.bat` and `get-log.bat`. Details: [devices/munch/kit/README.md](devices/munch/kit/README.md).

Experimental and not tested, outside the kit: booting the system from storage instead of RAM,
[tools/storage-boot](tools/storage-boot/README.md) (a file on `/data`) and [tools/ufs-boot](tools/ufs-boot/README.md)
(HakiOS's base design, a `darwin` partition).

## Building and running

What you need and the full steps: [devices/munch/README.md](devices/munch/README.md#what-you-need). In short:

```sh
make munch_defconfig DEVICE=munch
make kernel JOBS=12
(cd userland && GL=1 REST=1 ./build.sh)
./xl ramdisk userland/system.apfs      # once per loader session
XL_SECS=1800 ./xl run
```

Every kernel argument: [docs/boot-args.md](docs/boot-args.md). From Windows without WSL, `tools/mk-winboot.sh`
makes a folder with `boot-ios.bat`. HakiOS's general guides: [docs/](docs/index.md).

## Documentation

Documentation of the port: [t.me/ProjectIseng](https://t.me/ProjectIseng) (Telegram).

## Credits

[HakiOS](https://github.com/oosexgang/hakios-xnu-kernel) and its team (the kernel port, loader, common drivers,
user space and hakios-metal), [Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium) (UEFI), Mesa
(freedreno, ir3), Apple's open-source releases (xnu and friends), and Qualcomm's and Xiaomi's published Linux
kernel sources for the hardware details.

## Licenses

The XNU patch and the files derived from it are under the Apple Public Source License 2.0 ([LICENSE.APSL](LICENSE.APSL)),
the rest under MIT ([LICENSE](LICENSE)), as the file headers say. Third parties: [NOTICE](NOTICE),
[THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md), [COMPLIANCE.md](COMPLIANCE.md). What is and is not distributed:
[LEGAL.md](LEGAL.md). None of this is legal advice.

This is an independent research project, **not affiliated with or endorsed by Apple Inc. or Xiaomi**; "Darwin",
"iPhone", "XNU", "Apple" and "POCO" are named only to describe what the code works with. If you believe a file here
infringes anyone's rights, open an issue and it will be removed.
