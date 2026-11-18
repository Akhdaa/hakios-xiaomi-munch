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
| Metal | partial | with `metal=1` the desktop (CoreAnimation) draws through hakios-metal on the A650; smoother since 18 November but still a bit choppy, and one Reachability area is drawn wrong. Compute, copy/fill and offscreen render through Metal.framework pass |
| Wi-Fi | works | QCA6390 on PCIe 0 with the phone's own firmware; joins a network from Settings |
| Bluetooth | partial | QCA6390 on QUP0 SE6; Classic earbuds connect from Settings and play A2DP (AAC-LC). Control Center tile and CoreBluetooth discovery open |
| Sound | works | both speakers (TFA9874) through the audio DSP |
| Microphone | no | the driver is built; switching it on breaks touch |
| Camera | partial | OV64B40 wide and IMX596 front preview in Camera.app; the OV8856 ultra-wide is not started |
| Flashlight | works | from Control Center, with its levels (`munch_torch=3`) |
| Haptics | works | Awinic AW86927 |
| Battery | partial | level, voltage, current and temperature in the status bar; charging under this kernel is not done |
| Auto-Lock | works | follows the Settings timeout; no dimming before the lock |
| Thermal | partial | 25 sensors read; the prime core and the GPU slowed when hot; the stop at 105 °C not yet seen |
| Cellular | no | the Snapdragon X55 is found on PCIe 2, but its boot ROM refuses the boot loader over BHI (`0xef0e0003`) |
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
- No UEFI binaries or device tree taken from a phone, no serial numbers, MAC addresses, IMEI or calibration data.
- No keys, no activation, no Apple services (Apple ID, iCloud, App Store), no DRM circumvention.
- No writes to the phone's storage: no `fastboot flash`, no partition written. The modem's file systems, once
  cellular gets that far, are to be served read only.

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
