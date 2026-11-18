## Firmware Info

- Device: Xiaomi POCO F4 (munch)
- Version: `BOOT.XF.3.2-00354-SM8250-1` (`xbl_b` of the phone, read with `adb exec-out cat`)
- Extracted with [UEFIReader](https://github.com/WOA-Project/UEFIReader) `fc2e9e3` (built for net10.0)

## Patches/Fixes

The same six as `Binaries/alioth` (its `.efi` / `.patched.efi` pairs show the bytes), found in this build
by the code around them. Each was checked to have the old bytes before it was written.

| Driver | Offset | Old | New | What |
|---|---|---|---|---|
| ButtonsDxe | `0x36dc` | `51208052 f1330079` | `b1018052 f1370079` | power key: Enter (`0xd` as UnicodeChar) instead of `0x102` |
| DALSYSDxe | `0x2c654` | `00` | `01` | `\_SB_.UFS0` cache coherency |
| FeatureEnablerDxe | `0x1790` | `ffc300d1 f30b00f9` | `000080d2 c0035fd6` | clock enable returns success (keeps the framebuffer) |
| TzDxeLA | `0xd469` | `00` | `01` | the flag read at `0x32d4` (`ldrb w9,[x22,#0x469]`): TZ applets already loaded |
| UsbConfigDxe | `0x5684`, `0x56e0` | `2a020034` | `11000014` | `cbz w10` -> `b`: USB not deinitialised at ExitBootServices |
| UsbMsdDxe | `0x3a98` | `ec031932` | `0c008052` | storage non-removable |

`.inf` files of these six point at `*.patched.efi`. munchPkg's `RAW.inc` adds this firmware's own panels
(`Panel_L11R_38_08_0a_amoled_dsc_cmd.xml` is the POCO F4's) with the GUIDs UEFIReader gave them.
