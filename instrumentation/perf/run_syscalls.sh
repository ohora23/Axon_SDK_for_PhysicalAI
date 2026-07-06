#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# run_syscalls.sh — count data-path syscalls for axon vs ROS2 with strace -f -c.
# NO sudo (strace on your own children needs no privilege). strace adds ptrace
# overhead so the *counts* are what matter, not the timing.
#
# axon's payload path is pure shared memory: after the one-time FD handshake, a
# frame publish is a seqlock store — 0 transport syscalls per frame. ROS2 runs
# the DDS machinery. Resource-bounded.
set -euo pipefail
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
BOUND="${REPO}/instrumentation/run_bounded.sh"
MOD="$(ls -d "${REPO}"/build*/python 2>/dev/null | head -1)"
SCALE="${1:-1}"; SECS="${2:-2}"
TR="sendmsg,recvmsg,sendto,recvfrom,writev,readv,write,read"

summary() { grep -iE "^%|sendmsg|recvmsg|sendto|recvfrom|writev|readv|^ *[0-9].* (write|read)$|total|^-" | head -14; }

echo "Data-path syscalls — scale ${SCALE}, ${SECS}s (strace -f -c, no sudo)"
echo "axon: expect transport syscalls = one-time handshake only (not per frame).\n"

echo "########## axon ##########"
AXON_CORES=0-3 AXON_TIMEOUT=90 "$BOUND" \
    strace -f -c -e "trace=${TR}" \
    env PYTHONPATH="$MOD" python3 "${REPO}/benchmarks/mock/mock_axon.py" \
        --scale "$SCALE" --seconds "$SECS" 2>&1 | summary
echo

SETUP="$(ls /opt/ros/*/setup.bash 2>/dev/null | head -1)"
if [ -n "$SETUP" ]; then
    echo "########## ROS2 (Fast-RTPS) ##########"
    AXON_CORES=0-3 AXON_TIMEOUT=120 "$BOUND" \
        bash -lc "source ${SETUP} && strace -f -c -e trace=${TR} \
            python3 ${REPO}/benchmarks/mock/mock_ros2.py --scale ${SCALE} --seconds ${SECS}" \
        2>&1 | summary
fi
echo
echo "axon frames publish via seqlock (shared memory) — no per-frame syscall;"
echo "the FD sidecar handshake is once per stream. ROS2 runs the DDS transport."
