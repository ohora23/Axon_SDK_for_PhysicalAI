#!/usr/bin/env bash
# Camera → TensorRT inference end to end on the Jetson, zero-copy against the copy path,
# alternating modes per rep. Consumer on CPU 4 (isolated on hermes-head), producer on 0-3.
#   run_e2e.sh <build_dir> <engine.plan> [frames=600] [reps=2]  ->  results/e2e_<mode>_r<rep>.{csv,txt}
set -u
BUILD=${1:?usage: run_e2e.sh <build_dir> <engine.plan> [frames] [reps]}
ENGINE=${2:?engine.plan}
FRAMES=${3:-600}
REPS=${4:-2}
BIN=$BUILD/examples/jetson_e2e/axon_jetson_e2e
OUT=$(cd "$(dirname "$0")" && pwd)/results
mkdir -p "$OUT"
[ -x "$BIN" ] || { echo "no $BIN (build with AXON_ENABLE_JETSON=ON AXON_WITH_CUDA=ON)" >&2; exit 1; }

{ uname -a; nvpmodel -q 2>/dev/null | head -1; echo "isolated=$(cat /sys/devices/system/cpu/isolated)"
  v4l2-ctl -d /dev/video0 --get-fmt-video 2>/dev/null; echo "engine=$ENGINE frames=$FRAMES reps=$REPS"; } > "$OUT/env.txt"

PP=""
trap '[ -n "$PP" ] && kill "$PP" 2>/dev/null' EXIT
for rep in $(seq 1 "$REPS"); do
  for mode in zerocopy copy; do
    taskset -c 0-3 "$BIN" producer "$mode" /dev/video0 > "$OUT/producer_${mode}_r$rep.log" 2>&1 & PP=$!
    sleep 1
    taskset -c 4 "$BIN" consumer "$mode" "$ENGINE" "$FRAMES" "$OUT/e2e_${mode}_r$rep.csv" | tee "$OUT/e2e_${mode}_r$rep.txt"
    rc=${PIPESTATUS[0]}
    kill "$PP" 2>/dev/null; wait "$PP" 2>/dev/null; PP=""
    [ "$rc" = 0 ] || { echo "consumer failed ($mode rep $rep), producer log:" >&2; tail -5 "$OUT/producer_${mode}_r$rep.log" >&2; exit 1; }
    sleep 2   # let the camera close before the other mode opens it
  done
done
echo "[done] $OUT"
