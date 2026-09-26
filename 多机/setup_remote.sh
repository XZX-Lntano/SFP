#!/usr/bin/env bash
# One-time setup for host B (X520-2):
#   - allocate 256x2 MiB hugepages (if needed)
#   - bind 0000:01:00.0 (eth4/worker3) and 0000:01:00.1 (eth3/worker2) to vfio-pci
#   - allow non-root DPDK: /dev/hugepages 1777, /dev/vfio/* 0666
# Unbinding ixgbe removes enp1s0f0/f1 (management traffic uses enp4s0, unaffected).
# Run as root. Idempotent.
set -euo pipefail
[[ $EUID -eq 0 ]] || { echo "run as root (sudo)"; exit 1; }

B_DPDK="0000:01:00.1 0000:01:00.0"

cur=$(awk '/HugePages_Total/{print $2}' /proc/meminfo)
if (( cur < 256 )); then
    echo 256 > /proc/sys/vm/nr_hugepages
    echo "hugepages: $cur -> 256"
else
    echo "hugepages: $cur (enough)"
fi
mountpoint -q /dev/hugepages || mount -t hugetlbfs -o pagesize=2M nodev /dev/hugepages
modprobe vfio-pci

for b in $B_DPDK; do
    # readlink -f resolves even when the symlink is absent, so test -e first.
    drv=""
    if [[ -e /sys/bus/pci/devices/$b/driver ]]; then
        drv=$(basename "$(readlink -f /sys/bus/pci/devices/$b/driver)")
    fi
    if [[ $drv == "vfio-pci" ]]; then echo "$b already vfio-pci"; continue; fi
    if [[ -n $drv ]]; then echo "$b" > "/sys/bus/pci/drivers/$drv/unbind"; fi
    echo vfio-pci > "/sys/bus/pci/devices/$b/driver_override"
    echo "$b" > /sys/bus/pci/drivers/vfio-pci/bind
    echo "$b -> vfio-pci"
done

chmod 1777 /dev/hugepages
chmod 0666 /dev/vfio/* 2>/dev/null || true
echo "setup complete"
echo "NOTE: /dev/vfio/N nodes are re-created with mode 0600 whenever ALL devices"
echo "of the group leave vfio-pci and come back (e.g. after an ixgbe rebind)."
echo "If non-root EAL then fails with 'Permission denied', re-run:"
echo "  sudo chmod 0666 /dev/vfio/*"
dpdk-devbind.py -s 2>/dev/null | sed -n '1,12p'
