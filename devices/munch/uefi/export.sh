#!/bin/bash
# Save the munch platform from Mu-Silicium/ into files/ here (uefi/export.sh runs this; HakiOS's own changes are uefi/'s).
set -e
cd "$(dirname "$(readlink -f "$0")")"
MU=${MU:-../../../Mu-Silicium}
mkdir -p files/Resources/Configs files/Resources/DTBs files/Platforms/Xiaomi files/Binaries
rm -rf files/Platforms/Xiaomi/munchPkg
cp -r $MU/Platforms/Xiaomi/munchPkg files/Platforms/Xiaomi/
cp $MU/Resources/Configs/munch.toml files/Resources/Configs/
[ -f $MU/Resources/DTBs/munch.dtb ] && cp $MU/Resources/DTBs/munch.dtb files/Resources/DTBs/
[ -d $MU/Binaries/munch ] && cp -r $MU/Binaries/munch files/Binaries/
find files -name __pycache__ -prune -exec rm -rf {} +
echo "devices/munch/uefi/export.sh: $(find files -type f | wc -l) files"
