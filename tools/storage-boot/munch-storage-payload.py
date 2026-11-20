#!/usr/bin/env python3
"""HakiOS storage boot (NOT TESTED as packaged here; see tools/storage-boot/README.md): a RAM payload whose system image
is read from a file on the phone's userdata partition instead of being uploaded. Takes the map storage-map-probe printed
on the phone (--map), the same system.apfs on this computer (its blocks' CRC-32s go into the payload, so the loader
can tell a block it read is the one meant), the kernel, and a RAM payload to take the small root image and the boot
arguments from (a kit's run-payload.bin). Writes a payload with a "filemap" entry for the loader.

  munch-storage-payload.py --map map.txt --system system.apfs --kernel kernel.bin --base run-payload.bin --output out.bin
     [--expect-sha256 HEX]     refuse when system.apfs is not that image (the one copied to the phone)
"""
import argparse
import hashlib
import re
import struct
import time
import zlib
from pathlib import Path

BLOCK = 4096


def parse_map(path, image_blocks):
    lines = path.read_text(encoding='ascii').splitlines()
    if len(lines) < 3 or lines[-1] != 'filemap-end':
        raise ValueError('Android did not finish reporting the file map')
    header = re.fullmatch(r'filemap1 (\d+) (\d+)', lines[0])
    if not header:
        raise ValueError('Invalid file-map header')
    disk_blocks, mapped_blocks = map(int, header.groups())
    if not 0 < disk_blocks < 2**52 or mapped_blocks != image_blocks:
        raise ValueError('File size does not match the local image')
    extents = []
    for line in lines[1:-1]:
        match = re.fullmatch(r'extent (\d+) (\d+)', line)
        if not match:
            raise ValueError('Invalid extent')
        start, count = map(int, match.groups())
        if start >= disk_blocks or not 0 < count <= disk_blocks - start:
            raise ValueError('Extent outside userdata')
        extents.append((start, count))
    if len(extents) > 4096 or sum(count for _, count in extents) != image_blocks:
        raise ValueError('File map does not cover the image')
    ordered = sorted(extents)
    if any(start + count > next_start for (start, count), (next_start, _) in zip(ordered, ordered[1:])):
        raise ValueError('Overlapping physical extents')
    return struct.pack('<8sQQQ', b'HAKFMAP1', disk_blocks, image_blocks, len(extents)) + b''.join(
        struct.pack('<QQ', *extent) for extent in extents)


def entries(payload):
    if len(payload) < 32 or struct.unpack_from('<4sI', payload) != (b'XNUP', len(payload) - 8):
        raise ValueError('Invalid base RAM payload')
    offset = 8
    while offset + 24 <= len(payload):
        name, size, _ = struct.unpack_from('<16sII', payload, offset)
        if not name.rstrip(b'\0'):
            return
        start = offset + 24
        offset = start + ((size + 15) & ~15)
        if offset > len(payload):
            raise ValueError('Truncated base RAM payload')
        yield name.rstrip(b'\0'), payload[start:start + size]
    raise ValueError('Missing payload terminator')


def entry(name, data):
    return struct.pack('<16sII', name, len(data), 0) + data + bytes(-len(data) % 16)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for arg in ('map', 'system', 'kernel', 'base', 'output'):
        parser.add_argument('--' + arg, type=Path, required=True)
    parser.add_argument('--expect-sha256', default='', help='the sha256 the system image must have')
    args = parser.parse_args()
    size = args.system.stat().st_size
    if not size or size % BLOCK:
        raise ValueError('System image must be 4 KiB aligned')
    filemap = bytearray(parse_map(args.map, size // BLOCK))
    digest = hashlib.sha256()
    with args.system.open('rb') as image:
        for chunk in iter(lambda: image.read(1024 * 1024), b''):
            digest.update(chunk)
            for offset in range(0, len(chunk), BLOCK):
                filemap += struct.pack('<I', zlib.crc32(chunk[offset:offset + BLOCK]))
    if args.expect_sha256 and digest.hexdigest() != args.expect_sha256.lower():
        raise ValueError('system.apfs here is not the image given by --expect-sha256')
    print('system.apfs sha256 %s: the copy on the phone must be this same file' % digest.hexdigest())
    base = dict(entries(args.base.read_bytes()))
    allowed = set('sb_delay cpus bootcpu cpufreq touchscreen gpu adsp sound wifi haptic socinfo thermal battery_stop pmic battery fb_hole gpu_push_ms munch_torch panel panel_level wifi_mac fb_backlight fb_hz gpu_recover loader_return_after cam epoch '
                  'mic slpi hw munch_charge munch_bt'.split())
    cmdline = base[b'cmdline'].rstrip(b'\0').decode('ascii').split()
    keys = [arg.split('=', 1)[0] for arg in cmdline]
    if len(keys) != len(set(keys)) or set(keys) - allowed or 'loader_return_after' not in keys:
        raise ValueError('Unexpected boot arguments in the base payload')
    cmdline = [arg for arg in cmdline if not arg.startswith('epoch=')]
    cmdline.append(f'epoch={int(time.time())}')
    body = b''.join((entry(b'kernel', args.kernel.read_bytes()), entry(b'ramdisk', base[b'ramdisk']),
                     entry(b'filemap', filemap), entry(b'cmdline', ' '.join(cmdline).encode() + b'\0'), bytes(24)))
    if len(body) + 8 > 63 * 1024 * 1024:
        raise ValueError('Payload exceeds loader limit')
    args.output.write_bytes(struct.pack('<4sI', b'XNUP', len(body)) + body)


if __name__ == '__main__':
    main()
