#!/bin/bash
# tools/make-kit.sh VERSION [SOURCE_REPO]: the munch kit (devices/munch/kit/README.md) as build/kit/munch-kit-VERSION.zip:
#   prebuilt/kernel-munch.bin   hakios-kernel, with the build machine's user name replaced (same length) in its strings
#   prebuilt/userland/          userland/obj's programs, libraries and the Metal plug-in (what build.sh installs)
#   prebuilt/silicium/          devices/munch/prebuilt's Mu-Silicium UEFI, boot shim, device tree and ramdisk
#   source/                     SOURCE_REPO (a clone of the published repository, default: this tree) at its HEAD,
#                               by git archive: only committed files
#   assemble.sh/.bat, README.md devices/munch/kit's
# Nothing of Apple's, of the phone's or of this computer's goes in: the kit is checked for fw-* files, IPSW parts, the
# Wi-Fi files and the builder's names before it is zipped, and refused if any is there.
#   KIT_USER=name   the user name to scrub (default: $USER)
set -euo pipefail
cd "$(dirname "$0")/.."
VER=${1:?a version, e.g. 1.0}
SRC=$(realpath "${2:-.}")
WHO=${KIT_USER:-$USER}
NAME=munch-kit-$VER
OUT=build/kit/$NAME
die() { echo "make-kit: $*" >&2; exit 1; }
[ -f hakios-kernel ] && [ -f userland/obj/libhakios.gl.dylib ] && [ -f userland/obj/HakiosMetal ] ||
	die "build the kernel and the userland (GL=1) first"
for f in SILICIUM_UEFI.fd BootShim.bin device-tree.dtb ramdisk; do [ -f devices/munch/prebuilt/$f ] || die "devices/munch/prebuilt/$f is missing"; done
git -C "$SRC" rev-parse HEAD > /dev/null || die "$SRC is not a git repository"
rm -rf "$OUT" && mkdir -p "$OUT/prebuilt/userland" "$OUT/prebuilt/silicium" "$OUT/source"

# the kernel, its build user's name replaced by "build" padded to the same length (paths of panic messages, the version)
python3 - hakios-kernel "$OUT/prebuilt/kernel-munch.bin" "$WHO" <<'EOF'
import sys
src, dst, who = sys.argv[1:]
d = open(src, 'rb').read()
w = who.encode()
r = (b'build' + b'_' * len(w))[:len(w)] if len(w) >= 5 else b'b' * len(w)
n = d.count(w)
open(dst, 'wb').write(d.replace(w, r))
print('make-kit: kernel: %d strings with the build user replaced' % n)
EOF
# userland/obj: the programs and libraries (no objects, no leftovers)
for f in userland/obj/*; do
	case "$f" in *.o|*.a|*.new|*.d|*.tmp) continue;; esac
	[ -f "$f" ] && cp "$f" "$OUT/prebuilt/userland/"
done
cp userland/hello "$OUT/prebuilt/userland/" 2> /dev/null || true
cp devices/munch/prebuilt/{SILICIUM_UEFI.fd,BootShim.bin,device-tree.dtb,ramdisk} "$OUT/prebuilt/silicium/"
git -C "$SRC" archive HEAD | tar -C "$OUT/source" -xf -
cp devices/munch/kit/assemble.sh devices/munch/kit/assemble.bat devices/munch/kit/README.md "$OUT/"
chmod +x "$OUT/assemble.sh"
{
	echo "munch kit $VER, built $(date -u '+%Y-%m-%d %H:%M UTC')"
	echo "source: $(git -C "$SRC" remote get-url origin 2> /dev/null | sed 's#^git@github.com:#https://github.com/#') $(git -C "$SRC" rev-parse HEAD)"
} > "$OUT/prebuilt/BUILD"
(cd "$OUT" && find prebuilt -type f ! -name SHA256SUMS -print0 | sort -z | xargs -0 sha256sum > prebuilt/SHA256SUMS)

# what must not be there
bad=$(cd "$OUT" && find . \( -path '*/fw-*' -o -name '*.ipsw' -o -name '*.im4p' -o -name 'identity.h' -o -name 'network' \
	-o -name '*.apfs' -o -name 'mac' -o -name 'run-payload.bin' -o -name 'xnu-usb.img' -o -name 'efs*' \) -print | head -5)
[ -z "$bad" ] || die "refused: these must not be in a kit: $bad"
if grep -rIl -i -e "/home/$WHO" -e "$WHO@" "$OUT" 2> /dev/null | head -1 | grep -q .; then
	die "refused: the builder's home or address is in: $(grep -rIl -i -e "/home/$WHO" -e "$WHO@" "$OUT" | head -3 | tr '\n' ' ')"
fi
for f in "$OUT"/prebuilt/kernel-munch.bin "$OUT"/prebuilt/userland/*; do
	if LC_ALL=C grep -a -q -e "/home/$WHO/" -e "$WHO:xnu" "$f"; then die "refused: $f still names the builder"; fi
done
(cd build/kit && rm -f "$NAME.zip" && zip -qr "$NAME.zip" "$NAME")
echo "make-kit: build/kit/$NAME.zip ($(du -h "build/kit/$NAME.zip" | cut -f1)); sha256 $(sha256sum "build/kit/$NAME.zip" | cut -c1-16)..."
