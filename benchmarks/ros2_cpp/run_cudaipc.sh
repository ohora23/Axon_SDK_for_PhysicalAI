#!/bin/bash
# GPU -> GPU single-stream comparison with ros2_cuda_ipc (github.com/dskkato/ros2_cuda_ipc),
# next to the axon and ROS 2 zero-copy GPU cells of bench.cpp, in the same session.
#   CUDAIPC_WS=~/repositories/ros2_cuda_ipc run_cudaipc.sh [frames] [rate_hz] [reps]
# Needs that workspace built (colcon build) and this package built with it sourced.
HERE="$(cd "$(dirname "$0")" && pwd)"
FRAMES="${1:-500}"; RATE_HZ="${2:-100}"; REPS="${3:-3}"
CUDAIPC_WS="${CUDAIPC_WS:-$HOME/repositories/ros2_cuda_ipc}"
source /opt/ros/jazzy/setup.bash
source "$CUDAIPC_WS/install/setup.bash" || { echo "build $CUDAIPC_WS first"; exit 1; }
colcon --log-base "$HERE/log" build --base-paths "$HERE" --build-base "$HERE/build" --install-base "$HERE/install" \
    --cmake-args -DCMAKE_BUILD_TYPE=Release >/dev/null || exit 1
source "$HERE/install/setup.bash"
export ROS_DISABLE_LOANED_MESSAGES=0 RMW_IMPLEMENTATION=rmw_fastrtps_cpp ROS_DOMAIN_ID=77
export FASTRTPS_DEFAULT_PROFILES_FILE="$HERE/fastdds_zero_copy.xml" RMW_FASTRTPS_USE_QOS_FROM_XML=1
export AXON_BENCH_NBUF="${AXON_BENCH_NBUF:-4}"
BIN="$HERE/install/axon_bench/lib/axon_bench"
[ -x "$BIN/cudaipc_bench" ] || { echo "cudaipc_bench not built (is $CUDAIPC_WS sourced?)"; exit 1; }
mkdir -p "$HERE/results"; OUT="$HERE/results/cudaipc_$(date +%F).jsonl"; rm -f "$OUT"
# Record the machine state with the results (cores may be offlined on this PC, see README).
echo "{\"role\":\"meta\",\"cpus_online\":\"$(cat /sys/devices/system/cpu/online)\",\"nproc\":$(nproc),\"date\":\"$(date -Iseconds)\"}" >> "$OUT"
trap 'pkill -9 -f "$BIN/" 2>/dev/null' EXIT
for rep in $(seq 1 "$REPS"); do
  for nbytes in 150528 1048576 4194304; do
    for tr in cudaipc axon ros_loan; do
      f="$HERE/results/cudaipc_${tr}_${nbytes}_${rep}"
      if [ "$tr" = cudaipc ]; then sub=("$BIN/cudaipc_bench" sub "$nbytes" "$FRAMES" "$RATE_HZ"); pub=("$BIN/cudaipc_bench" pub "$nbytes" "$FRAMES" "$RATE_HZ")
      else sub=("$BIN/bench" sub "$tr" "$nbytes" "$FRAMES" "$RATE_HZ" 1); pub=("$BIN/bench" pub "$tr" "$nbytes" "$FRAMES" "$RATE_HZ" 1); fi
      "${sub[@]}" > "$f.sub" 2> "$f.err" & SUB=$!
      sleep 1
      "${pub[@]}" > "$f.pub" 2>> "$f.err" || echo "[FAIL] pub $tr $nbytes rep $rep"
      wait $SUB || echo "[FAIL] sub $tr $nbytes rep $rep"
      for r in pub sub; do grep '^{' "$f.$r" | sed "s/^{/{\"rep\":$rep,/" >> "$OUT"; done
    done
  done
done
# The event wait alone, and cudaipc with the producer host-synced first (event already done).
"$BIN/ipc_event_probe" >> "$OUT"
for nbytes in 150528 1048576 4194304; do
  f="$HERE/results/cudaipc_hostsync_${nbytes}"
  CUDAIPC_HOST_SYNC=1 "$BIN/cudaipc_bench" sub "$nbytes" "$FRAMES" "$RATE_HZ" > "$f.sub" 2> "$f.err" & SUB=$!
  sleep 1
  CUDAIPC_HOST_SYNC=1 "$BIN/cudaipc_bench" pub "$nbytes" "$FRAMES" "$RATE_HZ" > /dev/null 2>> "$f.err"
  wait $SUB
  grep '^{' "$f.sub" | sed 's/^{/{"rep":0,/' >> "$OUT"
done
python3 -I - "$OUT" <<'PY'
import json, statistics as st, sys
from collections import defaultdict
d = defaultdict(list)
for l in open(sys.argv[1]):
    o = json.loads(l)
    if o.get("role") == "sub" and not o.get("host_sync_producer"): d[(o["bytes"], o["transport"])].append(o)
print("| payload | transport | p50 µs (runs) | p99 µs | delivered | torn |")
print("|---|---|---|---|---|---|")
for k in sorted(d, key=lambda k: (k[0], ["ros_loan", "cudaipc", "axon"].index(k[1]))):
    r = d[k]; p50 = [x["p50_us"] for x in r]
    print(f"| {k[0]} | {k[1]} | {st.median(p50):.0f} ({'/'.join(f'{x:.0f}' for x in p50)}) | "
          f"{st.median([x['p99_us'] for x in r]):.0f} | {min(x['frames_seen'] for x in r)}/{r[0]['frames_sent']} | {sum(x['torn'] for x in r)} |")
PY
echo "[done] $OUT"
