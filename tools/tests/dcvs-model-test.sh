#!/bin/sh
# The kernel's GPU speed choice (drivers/gpu/hakios.dcvs.h, dcvs_next) against metal-ca-perf's floor property, on this machine.
set -e
cd "$(dirname "$0")/../.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cc -O2 -Wall -Wextra -Idrivers/gpu tools/tests/dcvs_model_test.c -o "$tmp/dcvs_model_test"
"$tmp/dcvs_model_test"
