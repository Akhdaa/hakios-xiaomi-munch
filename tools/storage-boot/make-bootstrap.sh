#!/bin/bash
# tools/storage-boot/make-bootstrap.sh [STAGE0] [OUT]: the small "Bootstrap" APFS the storage boot stages in place of
# system.apfs (NOT TESTED as packaged here; README.md). It holds only stage0 as /sbin/launchd, empty /dev and /system,
# and the marker /storage-bootstrap, by which stage0 (userland/src/stage0.c) knows to switch its root to the system
# image the loader reads from the phone's storage instead of to a resident image.
#   STAGE0  defaults to userland/obj/stage0 (a kit has it in prebuilt/userland/stage0)
#   OUT     defaults to bootstrap.apfs here
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$HERE/../..
STAGE0=${1:-$TOP/userland/obj/stage0}
OUT=${2:-$HERE/bootstrap.apfs}
[ -f "$STAGE0" ] || { echo "make-bootstrap: no stage0 at $STAGE0" >&2; exit 1; }
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/dev" "$work/sbin" "$work/system"
install -m 0755 "$STAGE0" "$work/sbin/launchd"
: > "$work/storage-bootstrap"
python3 "$TOP/tools/apfs/mkapfs.py" -o "$OUT" "Bootstrap=$work" > /dev/null
echo "make-bootstrap: $OUT ($(stat -c %s "$OUT") bytes)"
