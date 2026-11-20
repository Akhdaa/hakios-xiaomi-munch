# Storage boot (experimental, NOT TESTED as packaged here)

The normal way to start HakiOS on the POCO F4 is **from RAM**: `start.bat` (or the kit's package) uploads `system.apfs`
(about 100 s) and nothing on the phone is touched. That stays the default, and the kit does not use anything here.

This folder is for people who want to try the other way: keep `system.apfs` as a file on the phone's `/data` and let
the loader read it from there, so only a few MB go over USB at each boot. It was brought up once on one phone and then
set aside; the tools here are that work made general, and **this packaging has not been run on a phone**. Expect to
debug it.

## What it does

1. `system.apfs` is copied to the phone, at `/data/media/0/HakiOS/system.apfs` (this **writes 2.5 GB to your userdata
   partition**: your own choice, not the default).
2. `storage-map-probe`, run as root in Android, finds where that file lies on the userdata partition (`FS_IOC_FIEMAP`)
   and proves the map by reading blocks through the file and straight from `/dev/block/by-name/userdata` (they must be
   equal: storage that encrypts file contents on the way cannot be used). It only reads.
3. `munch-storage-payload.py` packs that map, a CRC-32 of every 4 KiB block of the same `system.apfs`, the kernel, and
   the small root and boot arguments of a RAM payload into a payload with a `filemap` entry.
4. `make-bootstrap.sh` makes a tiny `bootstrap.apfs` (stage0 and a marker) that is staged instead of `system.apfs`.
5. The loader (`xnuloader/loader.c`, `block_start_file()`) reads the system image's blocks from userdata through
   Qualcomm's UFS driver (UFSDxe, resident in Mu-Silicium), checks each against its CRC, and serves them to the kernel
   as the root. Nothing is written to the phone by the loader or the kernel.

## Steps (from a kit's `ios-munch` package and `source/`)

On the computer, in WSL, with the phone in Android, USB debugging on and root:

```bash
cd source/tools/storage-boot
aarch64-linux-gnu-gcc -static -O2 -o storage-map-probe storage-map-probe.c   # or the NDK's clang
bash make-bootstrap.sh ../../../prebuilt/userland/stage0 bootstrap.apfs
adb push storage-map-probe /data/local/tmp/ && adb shell chmod 755 /data/local/tmp/storage-map-probe
adb shell mkdir -p /data/media/0/HakiOS
adb push ../../../../ios-munch/system.apfs /data/media/0/HakiOS/system.apfs
adb shell su -c '/data/local/tmp/storage-map-probe --full'                 # the check, prints raw_full_match=yes
adb shell su -c '/data/local/tmp/storage-map-probe --map' > map.txt         # make it again before every boot
python3 munch-storage-payload.py --map map.txt --system ../../../../ios-munch/system.apfs \
    --kernel ../../../prebuilt/kernel-munch.bin --base ../../../../ios-munch/run-payload.bin --output storage-payload.bin
```

Then boot it (the phone in its own fastboot: `adb reboot bootloader`):

```bash
fastboot boot ../../../../ios-munch/xnu-usb.img      # Mu-Silicium and the loader, from RAM
fastboot oem ramdisk
fastboot stage bootstrap.apfs
fastboot stage storage-payload.bin
fastboot oem storagecheck                           # the loader reads the image through the map and checks it
fastboot oem run
```

`get-log.bat` (or `fastboot oem log` and `fastboot get_staged log.txt`) fetches the log while it runs.

## Notes

- The map is only good while the file stays where it is: copy, defragment or delete it and make the map again. The
  payload's CRCs are of the `system.apfs` on the computer, so the phone's copy must be that same file
  (`--expect-sha256` refuses a different one).
- Do not use a raw partition with `dd` for this: the loader reads one file on userdata, through its map, and nothing
  else. HakiOS's base also has a dedicated GPT partition design ("darwin", `ufs=2`); that is not set up for the
  POCO F4.
- Only for the POCO F4 (munch); the loader refuses the file map on any other phone.
