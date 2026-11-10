#!/usr/bin/env bash
# Camera → TensorRT inference end to end on the Jetson, zero-copy against the copy path,
# alternating modes per rep. Consumer on CPU 4 (isolated on hermes-head), producer on 0-3.
#   run_e2e.sh <build_dir> <engine.plan> [frames=600] [reps=2] [tag=run]
#     ->  results/<tag>/e2e_<mode>_r<rep>.{csv,txt}, env.txt (with GPU clocks and E2E_WARM_GPU)
# E2E_WARM_GPU=1 is passed through to the consumer.
set -u
BUILD=${1:?usage: run_e2e.sh <build_dir> <engine.plan> [frames] [reps] [tag]}
ENGINE=${2:?engine.plan}
FRAMES=${3:-600}
REPS=${4:-2}
TAG=${5:-run_$(date +%Y%m%d_%H%M%S)}
BIN=$BUILD/examples/jetson_e2e/axon_jetson_e2e
OUT=$(cd "$(dirname "$0")" && pwd)/results/$TAG
mkdir -p "$OUT"
[ -x "$BIN" ] || { echo "no $BIN (build with AXON_ENABLE_JETSON=ON AXON_WITH_CUDA=ON)" >&2; exit 1; }

{ uname -a; nvpmodel -q 2>/dev/null | head -1; echo "isolated=$(cat /sys/devices/system/cpu/isolated)"
  v4l2-ctl -d /dev/video0 --get-fmt-video 2>/dev/null; echo "engine=$ENGINE frames=$FRAMES reps=$REPS E2E_WARM_GPU=${E2E_WARM_GPU:-0}"
  gpu=$(ls -d /sys/class/devfreq/*.gv11b /sys/class/devfreq/*.ga10b 2>/dev/null | head -1)   # Xavier, Orin
  [ -n "$gpu" ] && echo "gpu_freq cur=$(cat $gpu/cur_freq) min=$(cat $gpu/min_freq) max=$(cat $gpu/max_freq)" || echo "gpu_freq unknown"
  echo "cpu4 min=$(cat /sys/devices/system/cpu/cpu4/cpufreq/scaling_min_freq) max=$(cat /sys/devices/system/cpu/cpu4/cpufreq/scaling_max_freq)"
} > "$OUT/env.txt"

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
