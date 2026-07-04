#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# run_pagefaults.sh — show that the dczc RT path takes ~0 page faults in steady
# state (design doc §5.4: mlockall + MAP_POPULATE + prefault).
#
# Uses /usr/bin/time -v (getrusage) — NO sudo/perf privilege needed. Runs the
# C++ closed-loop demo at two very different frame counts: if the RT loop faulted
# per frame, page faults would scale with frames. They don't — so the counts are
# near-identical and major (disk) faults stay at 0. Resource-bounded.
set -euo pipefail
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
DEMO="${REPO}/build/examples/demo/dczc_demo"
BOUND="${REPO}/instrumentation/run_bounded.sh"
[ -x "$DEMO" ] || { echo "build the demo first: cmake --build build -j" >&2; exit 1; }

run() {  # $1=frames
    echo "── demo --frames $1 (bounded to 4 cores) ──"
    DCZC_CORES=0-3 DCZC_TIMEOUT=60 /usr/bin/time -v \
        "$BOUND" "$DEMO" --frames "$1" --rate-hz 500 --quiet 2>&1 \
        | grep -iE "Major .*page faults|Minor .*page faults|Voluntary context|Elapsed" \
        | sed 's/^[[:space:]]*//'
    echo
}

echo "dczc RT page-fault check (getrusage, no sudo) — faults should NOT scale"
echo "with frame count, and Major (disk) faults must be 0."
echo
run 100
run 2000
echo "If Minor page-faults(2000) ≈ (100) and Major=0, the RT loop itself adds"
echo "~0 faults: the mlockall / MAP_POPULATE / prefault path holds."
