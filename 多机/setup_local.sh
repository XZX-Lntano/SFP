#!/usr/bin/env bash
# One-time setup for THIS machine (host A, X710-4).
# Run as root. Idempotent.
#
# IOMMU NOTE: all four X710 PFs (0000:01:00.0..3) share IOMMU group 1, so the
# group cannot be split between drivers. DPDK can open the group only if ALL
# four PFs are bound to vfio-pci. We therefore keep all four on vfio-pci, but
# the benchmark's EAL probe list (-a) names only the two ports actually wired
# to the board (01:00.3 = eth1/worker0, 01:00.2 = eth2/worker1). The other two
# (01:00.0/01:00.1) sit in the VFIO container idle and are not used.
#
# This script:
#   - verifies the four PFs are on vfio-pci (rebinds if needed)
#   - allocates 256x2 MiB hugepages (if needed)
#   - allows non-root DPDK: /dev/hugepages 1777, /dev/vfio/* 0666
set -euo pipefail
[[ $EUID -eq 0 ]] || { echo "run as root (sudo)"; exit 1; }

for b in 0000:01:00.0 0000:01:00.1 0000:01:00.2 0000:01:00.3; do
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

cur=$(awk '/HugePages_Total/{print $2}' /proc/meminfo)
if (( cur < 256 )); then
    echo 256 > /proc/sys/vm/nr_hugepages
    echo "hugepages: $cur -> 256"
else
    echo "hugepages: $cur (enough)"
fi
mountpoint -q /dev/hugepages || mount -t hugetlbfs -o pagesize=2M nodev /dev/hugepages

chmod 1777 /dev/hugepages
chmod 0666 /dev/vfio/* 2>/dev/null || true
echo "setup complete"
echo "NOTE: /dev/vfio/N nodes are re-created (mode 0600) whenever all devices of"
echo "the group leave vfio-pci and come back. If a rebind happens, re-run:"
echo "  chmod 0666 /dev/vfio/*"
dpdk-devbind.py -s 2>/dev/null | sed -n '1,12p'
