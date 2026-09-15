#!/bin/sh
# Flash the merged MCUboot + application image over J-Link SWD.
#
# The merged hex (bootloader at 0x0 + signed app in slot0 at 0x10000) is
# produced by the sysbuild:
#   west build --sysbuild -b holyiot_25008/nrf54l15/cpuapp . -- \
#       -DEXTRA_CONF_FILE="debug.conf;diag.conf;power.conf;dfu.conf"
#
# Usage:
#   tools/flash_mcuboot.sh [path/to/merged.hex]

set -e

DEFAULT_HEX="$(dirname "$0")/../../builds/speed_meter_dfu/merged_holyiot_25008_nrf54l15_cpuapp.hex"
HEX="${1:-$DEFAULT_HEX}"

# Resolve to an absolute path (JLinkExe needs the file where it can read it).
HEX="$(cd "$(dirname "$HEX")" && pwd)/$(basename "$HEX")"

if [ ! -f "$HEX" ]; then
	echo "error: merged hex not found: $HEX" >&2
	exit 1
fi

echo "Flashing: $HEX"
JLinkExe -device nRF54L15_M33 -if SWD -speed 4000 -autoconnect 1 <<EOF
r
h
loadfile ${HEX}
r
g
qc
EOF

echo "Done."
