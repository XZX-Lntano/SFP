#!/usr/bin/env bash
# Run the two local benchmark processes (rank0 = DPDK primary, rank1 = secondary).
# No root needed after setup_local.sh.
# Env: REQUESTS (default 3000000), WORKERS (default 4), ROUNDS (default 16),
#      SYNC (default handshake; grid = legacy NTP wall-clock grid)
set -uo pipefail
cd -- "$(dirname -- "$0")"
make bench_random >/dev/null 2>&1 || make bench_random
REQUESTS="${REQUESTS:-3000000}"
WORKERS="${WORKERS:-4}"
ROUNDS="${ROUNDS:-16}"
SYNC="${SYNC:-handshake}"
EXTRA_ARGS="${EXTRA_ARGS:-}"
PFX="sfp-mm-$(date +%s)-$$"
# Reclaim hugepages pinned by orphaned map files left behind when a run
# crashed before the primary's fini removed them. Only touch files >60 min
# old so a concurrent run's files are never deleted.
find /dev/hugepages -maxdepth 1 -name 'sfp-mm-*' -mmin +60 -delete 2>/dev/null || true

# Pre-flight. After a host reboot the setup script's chmods are lost:
# /dev/hugepages reverts to 0755 root:root and /dev/vfio/N to 0600. A non-root
# DPDK primary then dies in EAL ("Permission denied" on <prefix>map_0) while the
# secondary, started 3 s later, spins forever in EAL init at 100% CPU. Catch it
# here instead of leaving an orphan pegging a core.
if [[ ! -w /dev/hugepages ]]; then
    echo "FATAL: /dev/hugepages not writable by $(id -un): $(ls -ld /dev/hugepages)" >&2
    echo "       re-run:  sudo chmod 1777 /dev/hugepages && sudo chmod 0666 /dev/vfio/*" >&2
    exit 3
fi
for f in /dev/vfio/*; do
    case $f in /dev/vfio/vfio|/dev/vfio/devices) continue;; esac
    if [[ ! -r $f || ! -w $f ]]; then
        echo "FATAL: $f not rw for $(id -un): $(ls -l "$f")" >&2
        echo "       re-run:  sudo chmod 1777 /dev/hugepages && sudo chmod 0666 /dev/vfio/*" >&2
        exit 3
    fi
done

./bench_random --rank 0 --bdfs 0000:01:00.3,0000:01:00.2 --ranks 0,1 --primary --peer 1 \
    --cpu 4 --dpdk-cpu 0 --requests "$REQUESTS" --rounds "$ROUNDS" --workers "$WORKERS" \
    --sync "$SYNC" --file-prefix "$PFX" $EXTRA_ARGS > local-rank0.log 2>&1 &
P0=$!
sleep 3
# If the primary already died (EAL/config/link failure), do NOT start the
# secondary: with no primary it would spin forever inside rte_eal_init.
if ! kill -0 "$P0" 2>/dev/null; then
    echo "FATAL: rank0 (DPDK primary) died before rank1 could attach; rank1 not started" >&2
    RC0=0; wait "$P0" || RC0=$?
    echo "=== local rank0 ==="; cat local-rank0.log
    exit $(( RC0 ? RC0 : 1 ))
fi

./bench_random --rank 1 --bdfs 0000:01:00.3,0000:01:00.2 --ranks 0,1 --peer 0 \
    --cpu 5 --dpdk-cpu 1 --requests "$REQUESTS" --rounds "$ROUNDS" --workers "$WORKERS" \
    --sync "$SYNC" --file-prefix "$PFX" $EXTRA_ARGS > local-rank1.log 2>&1 &
P1=$!
RC0=0; RC1=0
wait "$P0" || RC0=$?
wait "$P1" || RC1=$?
echo "=== local rank0 ==="; cat local-rank0.log
echo "=== local rank1 ==="; cat local-rank1.log
echo "=== hugepages after ==="; grep -E 'HugePages_' /proc/meminfo
exit $(( RC0 | RC1 ))
