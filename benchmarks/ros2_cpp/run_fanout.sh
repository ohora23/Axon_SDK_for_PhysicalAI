#!/bin/bash
# axon_bench fan-out runner: one publisher, N subscriber processes (event-driven consumers)

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/.." && cd .. && pwd)"

FRAMES=${1:-500}
RATE=${2:-100}
REPS=${3:-3}

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
# axon ring depth: 4, like the few buffers a keep-last(1) ROS 2 writer reuses
export AXON_BENCH_NBUF="${AXON_BENCH_NBUF:-4}"

BENCH="$HERE/install/axon_bench/lib/axon_bench/bench"

# Fan-out cells: (gpu=0, bytes) and (gpu=1, bytes=1048576)
GPUS=(0 1)
CELLS_0=(150528 1048576 4194304)
CELLS_1=(1048576)
# FANOUT_CELL=<gpu>:<bytes> runs that one cell only (short runs fit between interruptions)
if [ -n "$FANOUT_CELL" ]; then
    GPUS=("${FANOUT_CELL%%:*}"); CELLS_0=("${FANOUT_CELL#*:}"); CELLS_1=("${FANOUT_CELL#*:}")
fi
TRANSPORTS=(ros_loan axon)
NSUBS=(1 2 4 8)

FANTDIR="$HERE/results/fanout_tmp"
rm -rf "${FANTDIR:?}"
mkdir -p "$FANTDIR"

JSONL="$HERE/results/fanout_rep.jsonl"
truncate -s 0 "$JSONL"

# Meta line
echo "{\"meta\":\"fanout\",\"date\":\"$(date -Is)\",\"cpus_online\":\"$(cat /sys/devices/system/cpu/online)\",\"frames\":$FRAMES,\"rate_hz\":$RATE,\"kernel\":\"$(uname -r)\",\"wait\":1}" >> "$JSONL"

PIDS=()
SUB_PIDS_ALL=()

cleanup() {
    for p in "${PIDS[@]}"; do
        kill "$p" 2>/dev/null
        wait "$p" 2>/dev/null
    done
    for p in "${SUB_PIDS_ALL[@]}"; do
        kill "$p" 2>/dev/null
        wait "$p" 2>/dev/null
    done
}
trap cleanup EXIT

FAIL_COUNT=0
ALL_RESULTS=""

for gpu in "${GPUS[@]}"; do
    if [ "$gpu" -eq 0 ]; then
        CELLS=("${CELLS_0[@]}")
    else
        CELLS=("${CELLS_1[@]}")
    fi

    for nbytes in "${CELLS[@]}"; do
        for transport in "${TRANSPORTS[@]}"; do
            for N in "${NSUBS[@]}"; do
                for rep in $(seq 1 $REPS); do
                    export BENCH_WAIT=1
                    export BENCH_NSUBS=$N

                    echo "[fanout] gpu=$gpu bytes=$nbytes transport=$transport N=$N rep=$rep/$REPS"

                    # Clean tmp dir for this run
                    rm -f "${FANTDIR:?}"/*

                    SUB_PIDS=()
                    SUB_FILES=()
                    SUB_ERRS=()

                    # Start N subscribers in the background
                    for k in $(seq 0 $((N - 1))); do
                        SUB_PID_FILE="$FANTDIR/sub_${k}.sub"
                        SUB_ERR_FILE="$FANTDIR/sub_${k}.err"
                        export BENCH_SUB_ID=$k

                        "$BENCH" sub "$transport" "$nbytes" "$FRAMES" "$RATE" "$gpu" \
                            > "$SUB_PID_FILE" 2> "$SUB_ERR_FILE" &
                        SUB_PID=$!
                        SUB_PIDS+=($SUB_PID)
                        SUB_PIDS_ALL+=($SUB_PID)
                        SUB_FILES+=("$SUB_PID_FILE")
                        SUB_ERRS+=("$SUB_ERR_FILE")
                    done

                    # Wait for subscribers to be ready
                    sleep 2

                    # Run publisher in foreground
                    PUB_FILE="$FANTDIR/pub.json"
                    PUB_ERR="$FANTDIR/pub.err"

                    "$BENCH" pub "$transport" "$nbytes" "$FRAMES" "$RATE" "$gpu" \
                        > "$PUB_FILE" 2> "$PUB_ERR"

                    # Capture publisher JSON
                    PUB_LINE=$(grep '^{' "$PUB_FILE" 2>/dev/null || true)
                    if [ -n "$PUB_LINE" ]; then
                        # Insert rep and n after opening brace
                        PUB_MOD=$(echo "$PUB_LINE" | sed "s/{/{\"rep\":$rep,\"fanout_n\":$N,/")
                        echo "$PUB_MOD" >> "$JSONL"
                        ALL_RESULTS="${ALL_RESULTS}${PUB_MOD}
"
                    fi

                    # Wait for every subscriber pid and check results
                    SUB_FAIL=0
                    for k in $(seq 0 $((N - 1))); do
                        wait ${SUB_PIDS[$k]}
                        SUB_RC=$?
                        SUB_JSON=$(grep '^{' "${SUB_FILES[$k]}" 2>/dev/null || true)

                        if [ $SUB_RC -ne 0 ] || [ -z "$SUB_JSON" ]; then
                            echo "[FAIL] subscriber $k exited $SUB_RC or no JSON (err: ${SUB_ERRS[$k]})"
                            echo "[FAIL] ${SUB_ERRS[$k]}"
                            SUB_FAIL=1
                            FAIL_COUNT=$((FAIL_COUNT + 1))
                        fi

                        if [ -n "$SUB_JSON" ]; then
                            # Insert rep and n after opening brace
                            SUB_MOD=$(echo "$SUB_JSON" | sed "s/{/{\"rep\":$rep,\"fanout_n\":$N,/")
                            echo "$SUB_MOD" >> "$JSONL"
                            ALL_RESULTS="${ALL_RESULTS}${SUB_MOD}
"
                        fi
                    done
                done
            done
        done
    done
done

# Kill any leftover children
pkill -P $$ 2>/dev/null || true

echo ""
echo "## Fan-out Results"
echo ""
python3 -c "
import json, os, sys, statistics

jsonl_path = '$JSONL'
if not os.path.exists(jsonl_path):
    print('No results file found', file=sys.stderr)
    sys.exit(0)

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
        if obj.get('meta') == 'fanout':
            continue
        if obj.get('role') not in ('pub', 'sub'):
            continue
        rows.append(obj)

# Group by (gpu, bytes, transport, n)
groups = {}
for r in rows:
    key = (r.get('gpu',0), r.get('bytes',0), r.get('transport',''), r.get('fanout_n',0))
    if key not in groups:
        groups[key] = {'pubs': [], 'subs': []}
    if r.get('role') == 'pub':
        groups[key]['pubs'].append(r)
    elif r.get('role') == 'sub':
        groups[key]['subs'].append(r)

print('| gpu | bytes | transport | n | worst_p50 | worst_p99 | min_frames | pub_cpu_s | sum_cpu_window_s |')
print('|-----|-------|-----------|---|-----------|-----------|------------|-----------|------------------|')

for key in sorted(groups.keys()):
    gpu, nbytes, tr, n = key
    data = groups[key]
    pubs = data['pubs']
    subs = data['subs']

    if not subs:
        continue

    # Collect p50 and p99 per subscriber per rep
    # Group subs by rep
    rep_subs = {}
    for s in subs:
        rep = s.get('rep', 1)
        if rep not in rep_subs:
            rep_subs[rep] = []
        rep_subs[rep].append(s)

    worst_p50_reps = []
    worst_p99_reps = []
    min_frames_reps = []

    for rep, rep_sub_list in rep_subs.items():
        p50s = [s.get('p50_us', 0) for s in rep_sub_list]
        p99s = [s.get('p99_us', 0) for s in rep_sub_list]
        frames = [s.get('frames_seen', 0) for s in rep_sub_list]
        if p50s:
            worst_p50_reps.append(max(p50s))
        if p99s:
            worst_p99_reps.append(max(p99s))
        if frames:
            min_frames_reps.append(min(frames))

    # Median over reps
    if worst_p50_reps:
        worst_p50 = statistics.median(worst_p50_reps)
    else:
        worst_p50 = 0
    if worst_p99_reps:
        worst_p99 = statistics.median(worst_p99_reps)
    else:
        worst_p99 = 0
    if min_frames_reps:
        min_frames = statistics.median(min_frames_reps)
    else:
        min_frames = 0

    pub_cpu = statistics.median([p.get('cpu_s', 0) for p in pubs]) if pubs else 0

    sum_cpu_window = sum(s.get('cpu_window_s', 0) for s in subs) / len(rep_subs) if rep_subs else 0

    print(f'| {gpu} | {nbytes} | {tr} | {n} | {worst_p50:.1f} | {worst_p99:.1f} | {min_frames} | {pub_cpu:.4f} | {sum_cpu_window:.4f} |')
"

echo ""
echo "[done] Results written to $JSONL"
if [ $FAIL_COUNT -gt 0 ]; then
    echo "[WARN] $FAIL_COUNT failure(s) detected"
    exit 1
fi
