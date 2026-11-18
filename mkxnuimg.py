#!/usr/bin/env python3
"""
Pack a phone's UEFI (with the hook that starts the XNU loader) and an optional payload
into an Android boot image for `fastboot boot`.

  kernel = gzip(BootShim + UEFI FD + ["XNUP" container]) + DTB

The boot shim copies the FD to its base and the payload to PAYLOAD_BASE.

The UEFI pieces are taken from a Mu-Silicium tree built here (Mu-Silicium/, see uefi/README.md)
or, when there is none, from devices/<device>/prebuilt/. The device is $DEVICE, else the .config's
CONFIG_DEVICE, else redwood; devices/<device>/uefi/image.conf, when it is there, gives its UEFI's
addresses (FD_BASE, FD_SIZE, PAYLOAD_BASE as NAME=VALUE lines): another phone's memory map is not
redwood's, and its UEFI is not either.

usage: mkxnuimg.py [-p payload.bin] [-o out.img]
"""
import argparse, gzip, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
MU = os.path.join(HERE, 'Mu-Silicium')


def config_device():
    try:
        for line in open(os.path.join(HERE, '.config')):
            if line.startswith('CONFIG_DEVICE='):
                return line.split('=', 1)[1].strip().strip('"')
    except OSError:
        pass
    return None


DEVICE = os.environ.get('DEVICE') or config_device() or 'redwood'
PREBUILT = os.path.join(HERE, 'devices', DEVICE, 'prebuilt')
IMAGE = {}
try:
    for line in open(os.path.join(HERE, 'devices', DEVICE, 'uefi', 'image.conf')):
        line = line.split('#', 1)[0].strip()
        if '=' in line:
            name, value = line.split('=', 1)
            IMAGE[name.strip()] = value.strip()
except OSError:
    pass
FD_BASE, FD_SIZE = int(IMAGE.get('FD_BASE', '0x9FD00000'), 0), int(IMAGE.get('FD_SIZE', '0x200000'), 0)
PAYLOAD_BASE, PAYLOAD_MAX = int(IMAGE.get('PAYLOAD_BASE', '0xF0000000'), 0), 0x04000000

ap = argparse.ArgumentParser()
ap.add_argument('-p', '--payload')
ap.add_argument('-o', '--output', default=os.path.join(HERE, f'xnu-{DEVICE}.img'))
ap.add_argument('--mode', default='RELEASE')
args = ap.parse_args()

fd_path = os.path.join(MU, f'Build/{DEVICE}Pkg/{args.mode}_CLANGPDB/FV/SILICIUM_UEFI.fd')
if os.path.isfile(fd_path):
    shim_dir = os.path.join(MU, 'BootShim')
    for f in ('BootShim.elf', 'BootShim.bin'):
        try: os.unlink(os.path.join(shim_dir, f))
        except FileNotFoundError: pass
    subprocess.check_call(['make', 'REQUIRES_KERNEL_HEADER=1', f'FD_BASE={FD_BASE:#x}', f'FD_SIZE={FD_SIZE:#x}',
                           f'CFLAGS=--target=aarch64-linux-gnu -DPAYLOAD_BASE={PAYLOAD_BASE:#x}'],
                          cwd=shim_dir, stdout=subprocess.DEVNULL)
    parts = {'shim': os.path.join(shim_dir, 'BootShim.bin'), 'fd': fd_path,
             'dtb': os.path.join(MU, f'Resources/DTBs/{DEVICE}.dtb'), 'ramdisk': os.path.join(MU, 'Resources/ramdisk'),
             'mkbootimg': os.path.join(MU, 'Resources/Scripts/mkbootimg.py')}
else:
    parts = {'shim': os.path.join(PREBUILT, 'BootShim.bin'), 'fd': os.path.join(PREBUILT, 'SILICIUM_UEFI.fd'),
             'dtb': os.path.join(PREBUILT, 'device-tree.dtb'), 'ramdisk': os.path.join(PREBUILT, 'ramdisk'),
             'mkbootimg': os.path.join(PREBUILT, 'mkbootimg.py')}
for name, path in parts.items():
    if not os.path.isfile(path):
        hint = ''
        if name == 'fd':
            hint = ("\n  The UEFI image is not in the repository: it carries the phone vendor's own drivers, which are not\n"
                    "  redistributed here. Build it from your own phone's firmware: uefi/README.md")
        sys.exit(f'mkxnuimg.py: {path} not found{hint}')
shim = open(parts['shim'], 'rb').read()
fd = open(parts['fd'], 'rb').read()
assert len(fd) == FD_SIZE, hex(len(fd))

blob = shim + fd
if args.payload:
    data = open(args.payload, 'rb').read()
    # the payload file is a complete "XNUP" container (see xnuloader/mkpayload.py)
    assert data[:4] == b'XNUP' and len(data) <= PAYLOAD_MAX - 0x1000, 'bad or oversized payload'
    blob += data
    blob += b'\0' * (-len(blob) % 16)
    print(f'payload: {len(data)} bytes, bytesum={sum(data) & 0xffffffff:08x}')

dtb = open(parts['dtb'], 'rb').read()
kernel = args.output + '.kernel~'
open(kernel, 'wb').write(gzip.compress(blob, 6) + dtb)
try:
    subprocess.check_call([sys.executable, parts['mkbootimg'], '--kernel', kernel, '--ramdisk', parts['ramdisk'],
                           '--header_version', '1', '-o', args.output])
finally:
    os.unlink(kernel)
print(f'{args.output}: {os.path.getsize(args.output)} bytes (shim {len(shim)}, uncompressed {len(blob)})')
