#!/bin/bash
# The munch platform on HakiOS's UEFI: uefi/apply.sh (Mu-Silicium with HakiOS's changes and banner), with this phone's
# platform from files/ here. Its vendor drivers (files/Binaries/munch) and its device tree (files/Resources/DTBs) are the
# phone's own and stay on the computer that extracted them: README.md here, and files/Binaries/munch/README.md.
exec "$(dirname "$(readlink -f "$0")")/../../../uefi/apply.sh" munch
