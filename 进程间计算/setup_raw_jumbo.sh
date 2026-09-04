#!/usr/bin/env bash
set -euo pipefail

for interface in enp1s0f3np3 enp1s0f2np2 enp1s0f1np1 enp1s0f0np0; do
    ip link set dev "$interface" mtu 9000
    ip link set dev "$interface" up
done

ip -br link show enp1s0f3np3 enp1s0f2np2 enp1s0f1np1 enp1s0f0np0
