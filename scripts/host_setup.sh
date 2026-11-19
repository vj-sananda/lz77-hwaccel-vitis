#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
[[ $EUID == 0 ]] || { echo 'Run with sudo -n.' >&2; exit 1; }
printf '1\n' > /sys/bus/pci/rescan
lspci -Dnn -d 10ee:7021
[[ -n $(lspci -Dn -d 10ee:7021) ]] || { echo 'FPGA did not enumerate.' >&2; exit 1; }
driver=vendor/dma_ip_drivers/XDMA/linux-kernel/xdma/xdma.ko
if [[ ! -d /sys/module/xdma ]]; then
    if mokutil --sb-state 2>/dev/null | rg -q enabled; then
        /lib/modules/"$(uname -r)"/build/scripts/sign-file sha256 \
            /var/lib/shim-signed/mok/MOK.priv /var/lib/shim-signed/mok/MOK.der "$driver"
    fi
    insmod "$driver" poll_mode=1
fi
udevadm settle
if [[ -n ${SUDO_USER:-} ]]; then
    for node in /dev/xdma*; do
        [[ -c $node ]] || continue
        chown "$SUDO_USER" "$node"
        chmod 600 "$node"
    done
fi
lspci -vv -d 10ee:7021
ls -l /dev/xdma*
