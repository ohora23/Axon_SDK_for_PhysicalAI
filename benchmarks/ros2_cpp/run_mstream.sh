#!/bin/bash
# mstream runner: the benchmarks/mock multi-stream workload in C++, per scale and transport.
#   run_mstream.sh [seconds] [scales]      e.g. run_mstream.sh 5 "1 2 3 4"
#   PERF=1 run_mstream.sh ...              also count cache-misses/instructions over the measured window
#                                          (perf_event_open in-process; needs kernel.perf_event_paranoid <= 1)
#   MSTREAM_STAGGER=1 run_mstream.sh ...   spread the streams' first deadlines (no simultaneous bursts)

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/.." && cd .. && pwd)"

RUN_SECONDS="${1:-5}"
SCALES="${2:-1 2 3 4}"

ROS_SETUP="/opt/ros/jazzy/setup.bash"
[ -n "$ROS_SETUP_ENV" ] && ROS_SETUP="$ROS_SETUP_ENV"

# Build axon core with CUDA if missing
if [ ! -f "$REPO/build-cuda/libaxon.a" ]; then
    echo "[run] Building axon core with CUDA..."
    cmake -S "$REPO" -B "$REPO/build-cuda" -DCMAKE_BUILD_TYPE=Release -DAXON_WITH_CUDA=ON
    cmake --build "$REPO/build-cuda" -j"$(nproc)"
fi

# Source ROS
source "$ROS_SETUP"

# Build the benchmark package
mkdir -p "$HERE/results"
colcon --log-base "$HERE/log" build --base-paths "$HERE" --build-base "$HERE/build" --install-base "$HERE/install" \
    --cmake-args -DCMAKE_BUILD_TYPE=Release

source "$HERE/install/setup.bash"

export ROS_DISABLE_LOANED_MESSAGES=0
export RMW_IMPLEMENTATION=rmw_fastrtps_cpp
export ROS_DOMAIN_ID=77
export FASTRTPS_DEFAULT_PROFILES_FILE="$HERE/fastdds_zero_copy.xml"
export RMW_FASTRTPS_USE_QOS_FROM_XML=1

MSTREAM="$HERE/install/axon_bench/lib/axon_bench/mstream"
JSONL="$HERE/results/ms_$(date +%F).jsonl"
rm -f "$JSONL"
cleanup() { pkill -9 -f "$MSTREAM" 2>/dev/null; }
trap cleanup EXIT

[ "${PERF:-0}" = 1 ] && export MSTREAM_PERF=1

run() {  # run <role> <transport> <scale> -> results/ms_<transport>_<scale>_<role>.{out,err}
    local out="$HERE/results/ms_$2_$3_$1"
    "$MSTREAM" "$1" "$2" "$3" "$RUN_SECONDS" > "$out.out" 2> "$out.err"
}

for scale in $SCALES; do
    for transport in ros_dyn ros_loan axon axon_wait; do
        echo "[mstream] scale=$scale transport=$transport"
        run sub "$transport" "$scale" & SUB=$!
        sleep 1
        run pub "$transport" "$scale" || echo "[FAIL] pub $transport scale=$scale (see results/ms_${transport}_${scale}_pub.err)"
        wait $SUB || echo "[FAIL] sub $transport scale=$scale (see results/ms_${transport}_${scale}_sub.err)"
        for role in pub sub; do
            f="$HERE/results/ms_${transport}_${scale}_${role}"
            line=$(grep '^{' "$f.out" | tail -1)
            [ -n "$line" ] && echo "$line" >> "$JSONL"
        done
    done
done

python3 -I - "$JSONL" <<'PY'
import json, sys
rows = {}
for l in open(sys.argv[1]):
    o = json.loads(l); rows.setdefault((o["scale"], o["transport"]), {})[o["role"]] = o
order = {"ros_dyn": 0, "ros_loan": 1, "axon": 2, "axon_wait": 3}
perf = any(r.get("pub", {}).get("cache_misses", -1) >= 0 for r in rows.values())
print("| scale | offered MB/s | transport | cores (pub + sub) | delivery (worst stream) | torn | cam p50 / p99 µs |" + (" cache-misses | instructions |" if perf else ""))
print("|---|---|---|---|---|---|---|" + ("---|---|" if perf else ""))
for (sc, tr) in sorted(rows, key=lambda k: (k[0], order[k[1]])):
    r = rows[(sc, tr)]; p, s = r.get("pub"), r.get("sub")
    if not (p and s): print(f"| {sc} | | {tr} | missing | | | |"); continue
    pc, sc_ = p["cpu_s"] / p["wall_s"], s["cpu_s"] / s["wall_s"]
    line = (f"| {sc} | {p['offered_MBps']:.0f} | {tr} | {pc + sc_:.3f} ({pc:.3f} + {sc_:.3f}) | "
            f"{s['worst_stream_delivery']:.3f} | {s['torn']} | {s['cam_p50_us']:.0f} / {s['cam_p99_us']:.0f} |")
    if perf:
        cm = max(p.get("cache_misses", 0), 0) + max(s.get("cache_misses", 0), 0)
        ins = max(p.get("instructions", 0), 0) + max(s.get("instructions", 0), 0)
        line += f" {cm/1e6:.1f} M | {ins/1e9:.2f} B |"
    print(line)
PY
echo "[done] $JSONL"
