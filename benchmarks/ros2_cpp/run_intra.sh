#!/bin/bash
# axon_bench intra-process runner — builds and runs the intra benchmark matrix

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/.." && cd .. && pwd)"

FRAMES="${1:-500}"
RATE_HZ="${2:-100}"
REPS="${3:-3}"

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

export RMW_IMPLEMENTATION=rmw_fastrtps_cpp
export ROS_DOMAIN_ID=77
export AXON_BENCH_NBUF="${AXON_BENCH_NBUF:-4}"

INTRA="$HERE/install/axon_bench/lib/axon_bench/intra"

JSONL="$HERE/results/intra_rep.jsonl"
rm -f "$JSONL"

# Meta line
echo "{\"meta\":\"intra\",\"date\":\"$(date -Is)\",\"cpus_online\":\"$(cat /sys/devices/system/cpu/online)\",\"frames\":$FRAMES,\"rate_hz\":$RATE_HZ,\"kernel\":\"$(uname -r)\"}" > "$JSONL"

GPUS=(0 1)
BYTES=(150528 1048576 4194304)
MODES=(poll wait)

for rep in $(seq 1 $REPS); do
    for gpu in "${GPUS[@]}"; do
        for nbytes in "${BYTES[@]}"; do
            for mode in "${MODES[@]}"; do
                echo "[run] rep=$rep gpu=$gpu bytes=$nbytes mode=$mode"
                OUT=$("$INTRA" "$nbytes" "$FRAMES" "$RATE_HZ" "$gpu" "$mode" 2>/dev/null)
                RC=$?
                LINE=$(grep '^{' <<<"$OUT")
                if [ $RC -ne 0 ]; then
                    echo "[FAIL] intra exited $RC for rep=$rep gpu=$gpu bytes=$nbytes mode=$mode"
                    LINE=""
                fi
                if [ -n "$LINE" ]; then
                    echo "$LINE" | sed "s/^{/{\"rep\":$rep,/" >> "$JSONL"
                fi
            done
        done
    done
done

# Print markdown table
echo ""
echo "## intra results"
echo ""
python3 -c "
import json, os, sys

jsonl_path = '$JSONL'
if not os.path.exists(jsonl_path):
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
        if obj.get('meta') == 'intra':
            continue
        rows.append(obj)

# Group by (gpu, bytes, transport)
groups = {}
for r in rows:
    key = (r.get('gpu',0), r.get('bytes',0), r.get('transport',''))
    if key not in groups:
        groups[key] = []
    groups[key].append(r)

print('| gpu | bytes | transport | rep | frames_seen | torn | p50 | p99 | copied/converted |')
print('|-----|-------|-----------|-----|-------------|------|-----|-----|------------------|')
for key in sorted(groups.keys()):
    gpu, nbytes, tr = key
    for r in groups[key]:
        gpu_v = r.get('gpu', '?')
        nbytes_v = r.get('bytes', '?')
        tr_v = r.get('transport', '?')
        rep_v = r.get('rep', '?')
        fs = r.get('frames_seen', 0)
        torn = r.get('torn', '?')
        p50 = r.get('p50_us', '?')
        p99 = r.get('p99_us', '?')
        if 'copied' in r:
            cc = 'copied=' + str(r['copied'])
        elif 'converted' in r:
            cc = 'converted=' + str(r['converted'])
        else:
            cc = '?'
        print(f'| {gpu_v} | {nbytes_v} | {tr_v} | {rep_v} | {fs} | {torn} | {p50} | {p99} | {cc} |')

print()
# Summary: per (gpu, bytes, transport) median of p50 and p99 over reps
print('### Summary (median p50/p99 per config)')
print()
print('| gpu | bytes | transport | median p50 | median p99 | min frames_seen | max torn | max copied/converted |')
print('|-----|-------|-----------|------------|------------|-----------------|----------|----------------------|')
for key in sorted(groups.keys()):
    gpu, nbytes, tr = key
    reps = groups[key]
    p50s = sorted([r.get('p50_us', 0) for r in reps])
    p99s = sorted([r.get('p99_us', 0) for r in reps])
    n = len(p50s)
    def median(v):
        if not v: return 0
        m = len(v) // 2
        if len(v) % 2 == 0:
            return (v[m-1] + v[m]) / 2
        return v[m]
    med_p50 = median(p50s)
    med_p99 = median(p99s)
    min_fs = min(r.get('frames_seen', 0) for r in reps)
    max_torn = max(r.get('torn', 0) for r in reps)
    if 'copied' in reps[0]:
        max_cc = max(r.get('copied', 0) for r in reps)
        cc_label = 'max_copied'
    elif 'converted' in reps[0]:
        max_cc = max(r.get('converted', 0) for r in reps)
        cc_label = 'max_converted'
    else:
        max_cc = '?'
        cc_label = '?'
    print(f'| {gpu} | {nbytes} | {tr} | {med_p50:.1f} | {med_p99:.1f} | {min_fs} | {max_torn} | {cc_label}={max_cc} |')
"

echo ""
echo "[done] Results written to $JSONL"
