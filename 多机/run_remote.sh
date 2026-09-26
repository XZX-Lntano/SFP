#!/usr/bin/env bash
# Run the two remote benchmark processes on host B (X520).
# No root needed after setup_remote.sh.
# Mapping: rank2 (worker2) <-> eth3 <-> 0000:01:00.1 ; rank3 (worker3) <-> eth4 <-> 0000:01:00.0
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

# Pre-flight: after a host reboot /dev/hugepages reverts to 0755 root:root and
# /dev/vfio/N to 0600, so the non-root DPDK primary dies in EAL while the
# secondary, started 3 s later, spins forever in EAL init at 100% CPU.
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

./bench_random --rank 2 --bdfs 0000:01:00.1,0000:01:00.0 --ranks 2,3 --primary --peer 3 \
    --cpu 2 --dpdk-cpu 0 --requests "$REQUESTS" --rounds "$ROUNDS" --workers "$WORKERS" \
    --sync "$SYNC" --file-prefix "$PFX" $EXTRA_ARGS > remote-rank2.log 2>&1 &
P2=$!
sleep 3
# If the primary already died, do NOT start the secondary (it would spin
# forever inside rte_eal_init with no primary to attach to).
if ! kill -0 "$P2" 2>/dev/null; then
    echo "FATAL: rank2 (DPDK primary) died before rank3 could attach; rank3 not started" >&2
    RC2=0; wait "$P2" || RC2=$?
    echo "=== remote rank2 ==="; cat remote-rank2.log
    exit $(( RC2 ? RC2 : 1 ))
fi

./bench_random --rank 3 --bdfs 0000:01:00.1,0000:01:00.0 --ranks 2,3 --peer 2 \
    --cpu 3 --dpdk-cpu 1 --requests "$REQUESTS" --rounds "$ROUNDS" --workers "$WORKERS" \
    --sync "$SYNC" --file-prefix "$PFX" $EXTRA_ARGS > remote-rank3.log 2>&1 &
P3=$!
RC2=0; RC3=0
wait "$P2" || RC2=$?
wait "$P3" || RC3=$?
echo "=== remote rank2 ==="; cat remote-rank2.log
echo "=== remote rank3 ==="; cat remote-rank3.log
echo "=== hugepages after ==="; grep -E 'HugePages_' /proc/meminfo
exit $(( RC2 | RC3 ))
