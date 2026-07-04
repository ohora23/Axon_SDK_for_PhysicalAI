#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# run_membw.sh — "copies aren't free": measure the memory-subsystem cost of the
# same MockSystem workload on dczc vs ROS2. ROS2 serializes+copies each payload
# (both sides), which burns cache and memory bandwidth; dczc keeps the tensor in
# a shared dma-buf, so the RT loop touches only descriptors.
#
# Counters: cache-references, cache-misses, LLC-loads/misses, instructions,
# context-switches. Needs perf privilege. Resource-bounded.
set -euo pipefail
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
BOUND="${REPO}/instrumentation/run_bounded.sh"
MOD="$(ls -d "${REPO}"/build*/python 2>/dev/null | head -1)"
SCALE="${1:-2}"; SECS="${2:-4}"

PERF="perf"
if [ "$(cat /proc/sys/kernel/perf_event_paranoid)" -gt 1 ] && [ "$(id -u)" -ne 0 ]; then
    if sudo -n true 2>/dev/null; then PERF="sudo perf"; else
        echo "perf needs privilege. Re-run with sudo, or: sudo sysctl kernel.perf_event_paranoid=1" >&2
    fi
fi

EVENTS="cache-references,cache-misses,LLC-loads,LLC-load-misses,context-switches,instructions"

echo "MockSystem memory-subsystem cost — scale ${SCALE}, ${SECS}s, bounded to 6 cores\n"

echo "########## dczc ##########"
$PERF stat -e "$EVENTS" -- env PYTHONPATH="$MOD" DCZC_CORES=0-5 DCZC_TIMEOUT=$((SECS+15)) \
    "$BOUND" python3 "${REPO}/benchmarks/mock/mock_dczc.py" --scale "$SCALE" --seconds "$SECS" \
    2>&1 | grep -iE "cache|LLC|context-switches|instructions|seconds time" | grep -v "mock_dczc"
echo

ROS2_SETUP="$(ls /opt/ros/*/setup.bash 2>/dev/null | head -1)"
if [ -n "$ROS2_SETUP" ]; then
    echo "########## ROS2 (Fast-RTPS) ##########"
    $PERF stat -e "$EVENTS" -- bash -lc \
        "source ${ROS2_SETUP} && DCZC_CORES=0-5 DCZC_TIMEOUT=$((SECS+15)) ${BOUND} \
         python3 ${REPO}/benchmarks/mock/mock_ros2.py --scale ${SCALE} --seconds ${SECS}" \
        2>&1 | grep -iE "cache|LLC|context-switches|instructions|seconds time"
fi
echo
echo "Expect ROS2 to show materially more cache-misses / LLC traffic for the same"
echo "delivered bytes — the memory cost of copying every payload."
