# munch kit: iOS 16.5 on the POCO F4

Built by `tools/make-kit.sh` from [Akhdaa/hakios-xiaomi-munch](https://github.com/Akhdaa/hakios-xiaomi-munch). It is half
of a bootable package; `assemble.bat` (Windows) or `assemble.sh` (Linux) makes the other half on your computer.
Modelled on [xiongnemo/hakios-alioth](https://github.com/xiongnemo/hakios-alioth)'s kit for the Redmi K40.

| Here | |
|---|---|
| `prebuilt/kernel-munch.bin` | The XNU kernel with this phone's drivers (no firmware in it) |
| `prebuilt/userland/` | Our programs and libraries for iOS, Mesa's OSMesa, the Metal plug-in |
| `prebuilt/silicium/` | Mu-Silicium's UEFI for munch (`SILICIUM_UEFI.fd`), its boot shim and device tree |
| `prebuilt/BUILD`, `SHA256SUMS` | What it was built from |
| `source/` | The repository at that commit: the scripts that assemble the package, and the source of everything above |
| `assemble.bat`, `assemble.sh` | Make the package |

## What you need

- A POCO F4 (`munch`) with its bootloader unlocked, USB debugging on, and **root** in Android (Magisk, KernelSU): the
  audio and sensor DSPs' firmware and the sensors' calibration are readable by root only. Nothing on the phone is written.
- Windows 10 or 11 with WSL (`wsl --install` in an administrator PowerShell), or Linux.
- About 20 GB of free space and an Internet connection.

## Making the package

Connect the phone in Android, then run `assemble.bat` (or `./assemble.sh`). It

1. downloads the iOS 16.5 IPSW for the iPhone X (`iPhone10,3`, 20F66) from Apple's public servers and unpacks the parts
   it needs (`IPSW=FILE` to use one you have; nothing is decrypted or circumvented);
2. copies the firmware from the phone (`source/devices/munch/kit/pull-firmware.sh`; allow root when the phone asks);
3. asks for the Wi-Fi network iOS should join (optional), and whether to add Sileo, a rootless jailbreak prefix
   (optional, off by default; `JB=1`);
4. builds the system image and the root disk with the programs in `prebuilt/`;
5. builds the loader with the iPhone identity from the IPSW, the boot image (Mu-Silicium + loader) and the payload;
6. writes `ios-munch/` beside the kit, with `start.bat` and Google's adb and fastboot.

Then run `start.bat` in `ios-munch/` with the phone in Android or fastboot. A reboot brings Android back.

**Keep `ios-munch/` to yourself**: it holds Apple's files, the vendor's firmware and your Wi-Fi password.

## Known limits

- Only the POCO F4 (`munch`). Another phone is refused before anything is done to it.
- Tested on HyperOS `OS4.0.24.0` firmware. The memory regions XBL leaves can move between firmware versions; if the
  kernel stops early on another version, say which one in an issue.
- iOS runs from RAM: settings and data are gone after a reboot.
