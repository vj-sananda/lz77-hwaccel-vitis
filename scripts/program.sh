#!/usr/bin/env bash
# Volatile SRAM programming only. The board's boot flash is preserved.
set -euo pipefail
cd "$(dirname "$0")/.."
test -f build/accelerator.bit
# Keep enumeration outside process substitution so failures stop programming.
device_list=$(lspci -Dn -d 10ee:)
devices=()
if [[ -n $device_list ]]; then
    mapfile -t devices < <(awk '{print $1}' <<< "$device_list")
fi
[[ ${#devices[@]} -le 1 ]] || { echo 'Multiple Xilinx cards found; select and detach the intended card manually.' >&2; exit 1; }
if [[ ${#devices[@]} == 1 ]]; then
    sudo -n bash scripts/detach_fpga.sh "${devices[0]}" > reports/detach.log
fi
vivado -mode batch -nojournal -log reports/program.log -source scripts/program.tcl
sudo -n bash scripts/host_setup.sh
