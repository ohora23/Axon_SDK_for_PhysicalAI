#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# run_bounded.sh — resource harness so verification never freezes the machine.
#
# Wraps a command with:
#   - taskset : hard-pin to a subset of CPU cores (the rest stay free for the
#               desktop). This is the real freeze protection — even a runaway
#               busy loop can only saturate the pinned cores.
#   - systemd-run --user --scope : cgroup MemoryMax + MemorySwapMax=0 so a leak
#               can't drive the box into swap thrash.
#   - nice/ionice : deprioritize vs interactive work.
#   - timeout : hard wall-clock cap.
#
# Defaults are conservative on a 16-core box: 6 cores, 8G RAM, 120s.
# Override via env: DCZC_CORES, DCZC_MEM, DCZC_TIMEOUT, DCZC_CPUQUOTA.
#
# Usage:  instrumentation/run_bounded.sh <command> [args...]
#         DCZC_CORES=0-3 DCZC_MEM=4G DCZC_TIMEOUT=30 run_bounded.sh python3 x.py

set -euo pipefail

CORES="${DCZC_CORES:-0-5}"          # 6 of 16 cores by default
MEM="${DCZC_MEM:-8G}"
TIMEOUT="${DCZC_TIMEOUT:-120}"
CPUQUOTA="${DCZC_CPUQUOTA:-600%}"   # cgroup ceiling (belt-and-suspenders w/ taskset)

if [ "$#" -eq 0 ]; then
    echo "usage: $0 <command> [args...]" >&2
    exit 2
fi

echo "[run_bounded] cores=$CORES mem=$MEM timeout=${TIMEOUT}s cpuquota=$CPUQUOTA" >&2
echo "[run_bounded] cmd: $*" >&2

# Prefer systemd-run for the cgroup memory ceiling; fall back to plain
# taskset+timeout if user-scope delegation is unavailable.
if systemd-run --user --scope -q -p Description="dczc-bounded" true >/dev/null 2>&1; then
    exec systemd-run --user --scope -q \
        -p "MemoryMax=$MEM" -p "MemorySwapMax=0" -p "CPUQuota=$CPUQUOTA" \
        -- timeout --signal=TERM --kill-after=5 "$TIMEOUT" \
           taskset -c "$CORES" nice -n 10 "$@"
else
    echo "[run_bounded] systemd user-scope unavailable — taskset+timeout only" >&2
    exec timeout --signal=TERM --kill-after=5 "$TIMEOUT" \
         taskset -c "$CORES" nice -n 10 "$@"
fi
