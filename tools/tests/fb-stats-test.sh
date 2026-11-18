#!/bin/sh
# The display driver's "fb:" line arithmetic (xnu/files/iokit/Kernel/MiniFBStats.h, fb_stats=1) on this machine.
set -e
cd "$(dirname "$0")/../.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cc -O2 -Wall -Wextra -Ixnu/files/iokit/Kernel tools/tests/fb_stats_test.c -o "$tmp/fb_stats_test"
"$tmp/fb_stats_test"
