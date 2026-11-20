# Booting from a "darwin" partition (HakiOS's base design, NOT TESTED on the POCO F4)

The normal way to start HakiOS on the POCO F4 is **from RAM** (`start.bat`, the kit's package): nothing on the phone
is touched. That stays the default; the kit does not use anything here.

This folder brings HakiOS's base design (the POCO X5 Pro's) to the POCO F4: Apple's Darwin volume lives on a GPT
partition of the phone named **`darwin`**, the loader reads it through Qualcomm's own UFS driver, and only a boot
volume and the cryptexes go over USB. **None of this has been run on a POCO F4.** Expect to debug it, and keep the log.

## How it works

- `ufs=2`: the loader (`xnuloader/loader.c`, UFS block service) keeps Mu-Silicium's UFS driver (UFSDxe) and serves
  blocks to the kernel through a shared page; the kernel (`drivers/storage/hakios.ufs.c`, `HAKIOS_UFS`) makes a
  read-only block device for every GPT partition, `/dev/ufs_NAME`.
- `root_apfs=ufs_darwin`: init (`userland/src/init.c`, `flash_root`) mounts the Darwin volume from `/dev/ufs_darwin`
  read only, puts this port's launchd jobs and audio plug-in over it, makes it the root, and mounts the cryptexes from
  the resident `flashboot.apfs`.
- Nothing writes to the phone during a boot: the block devices refuse writes (only `ufs_write=1`, which this does not
  use, lets one place of `darwin` be written).

## What you need

- A POCO F4 with a GPT partition **named `darwin`**, at least 6 GB, that Android does not use. Making or renaming a
  partition is up to you and can brick the phone if done wrong; it is not covered here.
- The kit assembled once (its `source/` has the unpacked IPSW and your firmware), or this repository with the same.
  `sysimage/rootfs.dmg` must be kept (`KEEP=1 sysimage/unpack.sh ...`), and `pip install lzfse` to read it.

## Steps

```bash
cd source/tools/ufs-boot             # in a kit, after assemble.sh; or tools/ufs-boot in the repository
bash make-ufs-boot.sh ..             # out/rootfs.apfs, out/flashboot.apfs, out/root.apfs, out/ufs-payload.bin
bash darwin-write.sh --check         # the checks only: is there a darwin partition, large enough, not Android's
DARWIN_WRITE=yes bash darwin-write.sh  # WRITES rootfs.apfs to the start of darwin, then reads it back and compares
```

Then boot (the phone in its own fastboot, `adb reboot bootloader`):

```bash
fastboot boot ../../../ios-munch/xnu-usb.img     # Mu-Silicium and the loader, from RAM
fastboot oem ramdisk
fastboot stage out/flashboot.apfs                 # about 1 GB: the boot volume and the cryptexes
fastboot stage out/ufs-payload.bin
fastboot oem run
```

`get-log.bat` in the package (or `fastboot oem log` then `fastboot get_staged log.txt`) fetches the log while it runs.
Look for `ufs:` lines (the partitions the kernel found) and `init:` lines about `root_apfs`.

## Notes

- `darwin-write.sh` writes only to `/dev/block/by-name/darwin`, only when sysfs names it `darwin`, it is not also any
  other by-name partition, not mounted, and large enough; with `DARWIN_WRITE=yes` only. The image is streamed, not
  staged on `/data`, and read back.
- The base design can also keep the data volume on `darwin` past the Darwin volume (`ufs_write=1`, `var_hfs=`); that is
  not set up for the POCO F4 and not used here.
- `tools/storage-boot/` is a different, simpler experiment: `system.apfs` as a file on `/data`.
