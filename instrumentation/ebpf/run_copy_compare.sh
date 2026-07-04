#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# run_copy_compare.sh — trace kernel<->user + net copies while running the dczc
# and ROS2 MockSystem workloads, so the "zero-copy" claim is measured, not
# asserted.
#
# Needs sudo (bpftrace requires root). Everything is resource-bounded so it can't
# freeze the box. Runs each backend separately (both are python3, so we compare
# whole-run totals rather than trying to split one comm).
#
#   sudo -v            # cache credentials first (optional)
#   instrumentation/ebpf/run_copy_compare.sh [scale] [seconds]
set -euo pipefail
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
BT="${REPO}/instrumentation/ebpf/copy_trace.bt"
SCALE="${1:-2}"
SECONDS_RUN="${2:-4}"
BOUND="${REPO}/instrumentation/run_bounded.sh"
MOD="$(ls -d "${REPO}"/build*/python 2>/dev/null | head -1)"

if ! command -v bpftrace >/dev/null; then echo "bpftrace not installed" >&2; exit 1; fi
if [ "$(id -u)" -ne 0 ] && ! sudo -n true 2>/dev/null; then
    echo "This needs root (bpftrace). Re-run with: sudo $0 $*" >&2
    echo "  or run 'sudo -v' first to cache credentials." >&2
fi
SUDO=""; [ "$(id -u)" -ne 0 ] && SUDO="sudo"

trace() {  # $1=label  $2...=workload command (already bounded)
    local label="$1"; shift
    echo "########## ${label} ##########"
    # bpftrace -c runs the command and traces for its lifetime, then prints maps.
    $SUDO bpftrace "$BT" -c "$*" 2>/dev/null \
        | grep -E "@(to_user|from_user)_bytes\[" | sort -t']' -k2 -rn | head -12
    echo
}

DCZC_CMD="env PYTHONPATH=${MOD} DCZC_CORES=0-5 DCZC_TIMEOUT=$((SECONDS_RUN+15)) \
    ${BOUND} python3 ${REPO}/benchmarks/mock/mock_dczc.py --scale ${SCALE} --seconds ${SECONDS_RUN}"

ROS2_SETUP="$(ls /opt/ros/*/setup.bash 2>/dev/null | head -1)"
ROS2_CMD="bash -lc 'source ${ROS2_SETUP} && DCZC_CORES=0-5 DCZC_TIMEOUT=$((SECONDS_RUN+15)) \
    ${BOUND} python3 ${REPO}/benchmarks/mock/mock_ros2.py --scale ${SCALE} --seconds ${SECONDS_RUN}'"

echo "MockSystem copy trace — scale ${SCALE}, ${SECONDS_RUN}s (bounded to 6 cores)"
echo "dczc payload path is pure mmap → expect ~0 payload bytes through copy_*_user."
echo
trace "dczc"  "$DCZC_CMD"
[ -n "$ROS2_SETUP" ] && trace "ROS2 (Fast-RTPS)" "$ROS2_CMD"
echo "Interpretation: compare copy_*_user byte volume attributed to python3."
echo "dczc moves the tensor via shared dma-buf (no per-frame kernel copy);"
echo "ROS2 moves each frame through the DDS transport."
