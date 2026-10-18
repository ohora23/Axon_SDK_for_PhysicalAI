#!/bin/bash
# sync_cost runner: what axon's host-sync before publish costs a GPU producer, and the
# GPU-side cost of waiting on a CUDA IPC event instead. Needs an idle GPU: another process
# on the GPU time-slices it and inflates every number here.
HERE="$(cd "$(dirname "$0")" && pwd)"
FRAMES="${1:-2000}"
REPS="${2:-3}"
BIN="$HERE/install/axon_bench/lib/axon_bench/sync_cost"
[ -x "$BIN" ] || { echo "build first (run.sh or colcon build)"; exit 1; }
# Another compute process using the SMs (an idle resident model is fine) invalidates the run.
others=$(nvidia-smi pmon -c 1 -s u | awk '!/^#/ && $3 ~ /C/ && $4 ~ /^[0-9]+$/ && $4 > 0' | grep -c .)
[ "$others" -eq 0 ] || { echo "another process is using the GPU; refusing to measure"; exit 1; }

JSONL="$HERE/results/sync_cost_rep.jsonl"
echo "{\"meta\":\"sync_cost\",\"date\":\"$(date -Is)\",\"cpus_online\":\"$(cat /sys/devices/system/cpu/online)\",\"kernel\":\"$(uname -r)\",\"gpu\":\"$(nvidia-smi --query-gpu=name,driver_version --format=csv,noheader)\"}" > "$JSONL"
for rep in $(seq 1 "$REPS"); do
    for k in 50 200 1000 5000; do
        for c in 0 "$k"; do
            for mode in sync_naive sync_pipelined event; do
                n=$FRAMES; [ "$k" -ge 1000 ] && n=$((FRAMES / 5))  # keep each run under ~2 s
                "$BIN" pipeline "$mode" "$k" "$c" "$n" | sed "s/^{/{\"rep\":$rep,/" >> "$JSONL"
            done
        done
    done
    "$BIN" ipcwait 300 | sed "s/^{/{\"rep\":$rep,/" >> "$JSONL"
done

python3 - "$JSONL" <<'EOF'
import json, sys, statistics as st
rows = [json.loads(l) for l in open(sys.argv[1]) if '"probe"' in l]
med = lambda xs: st.median(xs)
print("| kernel µs | CPU µs | mode | frames/s | GPU busy | GPU idle per frame µs |")
print("|---|---|---|---|---|---|")
keys = sorted({(r["kernel_us"], r["cpu_us"], r["mode"]) for r in rows if r["probe"] == "pipeline"},
              key=lambda k: (k[0], k[1], ["sync_naive", "sync_pipelined", "event"].index(k[2])))
for k in keys:
    g = [r for r in rows if r["probe"] == "pipeline" and (r["kernel_us"], r["cpu_us"], r["mode"]) == k]
    print(f"| {k[0]} | {k[1]} | {k[2]} | {med([r['fps'] for r in g]):.0f} | {med([r['gpu_busy'] for r in g]):.3f} | {med([r['gpu_idle_per_frame_us'] for r in g]):.1f} |")
w = [r for r in rows if r["probe"] == "ipcwait"]
for f in ("ipc_wait_enqueue_p50_us", "ipc_gpu_delay_p50_us", "ipc_gpu_delay_p90_us", "local_wait_enqueue_p50_us", "local_gpu_delay_p50_us",
          "hostsync_gpu_delay_p50_us", "hostsync_gpu_delay_p90_us", "local_hostsync_gpu_delay_p50_us"):
    print(f"{f}: {med([r[f] for r in w]):.2f} ({', '.join(str(r[f]) for r in w)})")
EOF
