#!/bin/bash
# axon_bench runner — builds and runs every cell of the benchmark matrix

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/.." && cd .. && pwd)"

FRAMES="${1:-500}"
RATE_HZ="${2:-100}"

ROS_SETUP="/opt/ros/jazzy/setup.bash"
[ -n "$ROS_SETUP_ENV" ] && ROS_SETUP="$ROS_SETUP_ENV"

# Build axon core with CUDA, every time: the bench compiles against include/ and links the static
# library, so a stale libaxon.a with an older class layout breaks at run time, not at link time.
[ -f "$REPO/build-cuda/CMakeCache.txt" ] || cmake -S "$REPO" -B "$REPO/build-cuda" -DCMAKE_BUILD_TYPE=Release -DAXON_WITH_CUDA=ON || exit 1
cmake --build "$REPO/build-cuda" -j"$(nproc)" || { echo "[run] axon core build failed" >&2; exit 1; }

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
# axon ring depth: 4, like the few buffers a keep-last(1) ROS 2 writer reuses. A 16-deep
# ring of 4 MiB buffers (64 MB) falls out of the 9800X3D L3 and costs ~2x at 4 MiB.
export AXON_BENCH_NBUF="${AXON_BENCH_NBUF:-4}"

BENCH="$HERE/install/axon_bench/lib/axon_bench/bench"

GPUS=(0 1)
BYTES=(150528 1048576 4194304)
TRANSPORTS=(ros_dyn ros_loan axon)

TODAY=$(date +%F)
JSONL="$HERE/results/$TODAY.jsonl"
rm -f "$JSONL"
PIDS=()

cleanup() {
    for p in "${PIDS[@]}"; do
        kill "$p" 2>/dev/null
        wait "$p" 2>/dev/null
    done
}
trap cleanup EXIT

for gpu in "${GPUS[@]}"; do
    for nbytes in "${BYTES[@]}"; do
        for transport in "${TRANSPORTS[@]}"; do
            SUBFILE="$HERE/results/${transport}_${nbytes}_${gpu}.sub"
            PUBFILE="$HERE/results/${transport}_${nbytes}_${gpu}.pub"
            ERRFILE="$HERE/results/${transport}_${nbytes}_${gpu}.err"

            echo "[run] gpu=$gpu bytes=$nbytes transport=$transport"

            # Both ROS 2 cells use the same Fast DDS profile: the documented zero-copy setup
            # (without it Fast DDS sends a 4 MiB plain type over the network transports).
            export FASTRTPS_DEFAULT_PROFILES_FILE="$HERE/fastdds_zero_copy.xml" RMW_FASTRTPS_USE_QOS_FROM_XML=1

            # Start subscriber first (background)
            "$BENCH" sub "$transport" "$nbytes" "$FRAMES" "$RATE_HZ" "$gpu" \
                > "$SUBFILE" 2> "$ERRFILE" &
            SUB_PID=$!
            PIDS+=($SUB_PID)
            sleep 1

            # Run publisher; halfway through, record whether Fast DDS data-sharing segments exist
            "$BENCH" pub "$transport" "$nbytes" "$FRAMES" "$RATE_HZ" "$gpu" \
                > "$PUBFILE" 2>> "$ERRFILE" &
            PUB_PID=$!
            sleep $(( 1 + FRAMES / RATE_HZ / 2 ))
            echo "datasharing_segments=$(ls /dev/shm | grep -c -i datasharing)" >> "$ERRFILE"
            wait $PUB_PID

            # Wait for subscriber
            wait $SUB_PID
            SUB_RC=$?

            if [ $SUB_RC -ne 0 ]; then
                echo "[FAIL] sub exited $SUB_RC for $transport $nbytes gpu=$gpu"
                cat "$ERRFILE"
            fi

            # Append both pub and sub JSON lines to jsonl
            grep '^{' "$PUBFILE" >> "$JSONL" 2>/dev/null || true
            grep '^{' "$SUBFILE" >> "$JSONL" 2>/dev/null || true
        done
    done
done

# Print markdown table
echo ""
echo "## Results ($TODAY)"
echo ""
python3 -c "
import json, os, sys

jsonl_path = '$JSONL'
if not os.path.exists(jsonl_path):
    sys.exit(0)

pubs = {}
rows = []
with open(jsonl_path) as f:
    for line in f:
        line = line.strip()
        if not line:
            continue
        try:
            obj = json.loads(line)
        except:
            continue
        key = (obj.get('transport',''), obj.get('bytes',0), obj.get('gpu',0))
        if obj.get('role') == 'pub':
            pubs[key] = obj
        elif obj.get('role') == 'sub':
            obj['pub_cpu_s'] = pubs.get(key, {}).get('cpu_s', '?')
            rows.append(obj)

rows.sort(key=lambda r: (r.get('gpu',0), r.get('bytes',0), r.get('transport','')))

print('| gpu | bytes | transport | frames_seen/frames_sent | torn | p50 | p99 | max | pub_cpu_s |')
print('|-----|-------|-----------|------------------------|------|-----|-----|-----|-----------|')
for r in rows:
    gpu = r.get('gpu', '?')
    nbytes = r.get('bytes', '?')
    tr = r.get('transport', '?')
    fs = r.get('frames_seen', 0)
    fsent = r.get('frames_sent', '?')
    torn = r.get('torn', '?')
    p50 = r.get('p50_us', '?')
    p99 = r.get('p99_us', '?')
    mx = r.get('max_us', '?')
    pub_cpu = r.get('pub_cpu_s', '?')
    print(f'| {gpu} | {nbytes} | {tr} | {fs}/{fsent} | {torn} | {p50} | {p99} | {mx} | {pub_cpu} |')
"

echo ""
echo "[done] Results written to $JSONL"
