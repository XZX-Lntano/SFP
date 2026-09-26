#!/usr/bin/env bash
# Orchestrate the whole two-machine benchmark from THIS machine (host A).
#   ./run_all.sh [REQUESTS]        (default 3000000; e.g. 1000 for a smoke test)
#
# Site-specific values (ssh target of host B, local IP prefix) are read from
# ./local.conf, which is gitignored so no real address enters this repository:
#     REMOTE="userB@hostB"
#     IP_PREFIX="192.168."
# Requires: passwordless ssh to $REMOTE, both machines set up
# (setup_local.sh / setup_remote.sh already run), board powered and bitstream loaded.
# No root needed at run time (setup scripts opened the permissions).
set -uo pipefail
cd -- "$(dirname -- "$0")"
[[ -f local.conf ]] && source ./local.conf
REQUESTS="${1:-3000000}"
EXTRA_ARGS="${EXTRA_ARGS:-}"
WORKERS="${WORKERS:-4}"
ROUNDS="${ROUNDS:-16}"
SYNC="${SYNC:-handshake}"
REMOTE="${REMOTE:-${SFP_REMOTE:-user@remote-host}}"
IP_PREFIX="${IP_PREFIX:-192.168.}"
MASTER_IP="${MASTER_IP:-$(hostname -I | tr ' ' '\n' | grep -E "^${IP_PREFIX}" | head -1)}"
if [[ -z $MASTER_IP ]]; then echo "cannot determine local IP matching ${IP_PREFIX}"; exit 2; fi
echo "master IP: $MASTER_IP, requests: $REQUESTS, sync: $SYNC workers: $WORKERS"

# 1) sync code to the remote, then verify the critical sources actually landed
#    (a silent rsync failure would make the two machines run different code).
#    local.conf is excluded so host A's site values never land on host B.
rsync -az --delete --exclude 'bench_random' --exclude 'local.conf' --exclude '*.log' ./ "$REMOTE:多机/"
SRC="bench_random.c sfp_lib.c sfp_lib.h bridge_protocol.h"
LOCAL_MD5=$(md5sum $SRC | awk '{print $1}' | tr '\n' ' ')
REMOTE_MD5=$(ssh -o BatchMode=yes "$REMOTE" "cd 多机 && md5sum $SRC" | awk '{print $1}' | tr '\n' ' ')
if [[ "$LOCAL_MD5" != "$REMOTE_MD5" ]]; then
    echo "FATAL: remote $SRC differ after rsync"
    echo " local: $LOCAL_MD5"
    echo "remote: $REMOTE_MD5"
    exit 3
fi

# 2) build on the remote
ssh -o BatchMode=yes "$REMOTE" 'cd 多机 && make bench_random'

# 3) start the remote two processes (background); they align with the local
#    two via the fixed-base handshake (or the legacy grid when SYNC=grid)
ssh -o BatchMode=yes "$REMOTE" \
    "cd 多机 && REQUESTS='$REQUESTS' WORKERS='$WORKERS' ROUNDS='$ROUNDS' SYNC='$SYNC' EXTRA_ARGS='$EXTRA_ARGS' ./run_remote.sh" > remote_run.log 2>&1 &
SSH_PID=$!

# 4) local two processes (foreground)
REQUESTS="$REQUESTS" WORKERS="$WORKERS" ROUNDS="$ROUNDS" SYNC="$SYNC" EXTRA_ARGS="$EXTRA_ARGS" ./run_local.sh
RC_LOCAL=$?

# 5) collect the remote result
RC_REMOTE=0
wait "$SSH_PID" || RC_REMOTE=$?
echo "=== remote_run.log ==="
cat remote_run.log
echo "=== summary: local rc=$RC_LOCAL remote rc=$RC_REMOTE ==="
exit $(( RC_LOCAL | RC_REMOTE ))
