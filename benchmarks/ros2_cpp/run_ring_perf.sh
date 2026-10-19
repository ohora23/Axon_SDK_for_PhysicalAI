#!/bin/bash
# Why axon's host latency grows with ring depth at large payloads: hardware counters on the
# producer (it writes the whole payload, which is most of the latency) for axon with
# AXON_BENCH_NBUF 1/2/4 and for iceoryx2, at 4, 16 and 64 MiB, 30 Hz. Needs
# `sudo sysctl kernel.perf_event_paranoid=1` and iox2_bench built. Counting starts 1.5 s in,
# after setup (pool creation, first touch of every buffer, discovery).
#   run_ring_perf.sh [frames] [reps]  ->  results/ring_perf_rep.jsonl
HERE="$(cd "$(dirname "$0")" && pwd)"
FRAMES="${1:-300}"; REPS="${2:-3}"
PERF="${PERF:-/usr/lib/linux-tools/7.0.0-38-generic/perf}"
[ "$(cat /proc/sys/kernel/perf_event_paranoid)" -le 1 ] || { echo "needs kernel.perf_event_paranoid <= 1"; exit 1; }
source /opt/ros/jazzy/setup.bash; source "$HERE/install/setup.bash"
export ROS_DOMAIN_ID=77
B="$HERE/install/axon_bench/lib/axon_bench"
T="${GATED_TMP:-$HOME/.cache/axon-gated}"; mkdir -p "$T"
EV=cycles,instructions,ls_any_fills_from_sys.local_l2,ls_any_fills_from_sys.local_ccx,ls_any_fills_from_sys.dram_io_all,ls_l1_d_tlb_miss.all,ls_l1_d_tlb_miss.all_l2_miss,page-faults
OUT="$HERE/results/ring_perf_rep.jsonl"
echo "{\"meta\":\"ring_perf\",\"date\":\"$(date -Is)\",\"cpus_online\":\"$(cat /sys/devices/system/cpu/online)\",\"events\":\"$EV\",\"delay_ms\":1500,\"rate_hz\":30,\"frames\":$FRAMES}" > "$OUT"

one() {  # one <label> <nbuf> <bytes> <sub cmd...> -- <pub cmd...>
  local label=$1 nbuf=$2 bytes=$3 rep=$4; shift 4
  local sub=() pub=(); while [ "$1" != -- ]; do sub+=("$1"); shift; done; shift; pub=("$@")
  AXON_BENCH_NBUF=$nbuf "${sub[@]}" > "$T/rp.sub" 2>/dev/null & local sp=$!
  sleep 1
  local t0=$(date +%s%N)
  AXON_BENCH_NBUF=$nbuf "$PERF" stat -x, --delay 1500 -e "$EV" -o "$T/rp.perf" "${pub[@]}" > "$T/rp.pub" 2>/dev/null
  local el=$(( ($(date +%s%N) - t0) / 1000000 ))
  wait $sp
  python3 -I - "$label" "$nbuf" "$bytes" "$rep" "$T/rp.perf" "$T/rp.pub" "$T/rp.sub" "$el" >> "$OUT" <<'PY'
import json, sys
label, nbuf, nbytes, rep, perf, pub, sub, elapsed_ms = sys.argv[1:9]
c = {}
for line in open(perf):
    f = line.strip().split(",")
    if len(f) > 2 and f[0] and f[0][0].isdigit():
        c[f[2]] = int(f[0])
p = json.loads(next(l for l in open(pub) if l.startswith("{")))
s = json.loads(next(l for l in open(sub) if l.startswith("{")))
# Frames inside the counting window: process time minus the 1.5 s delay and, for iox2_bench, the
# 0.5 s idle tail after its last frame (bench.cpp's axon producer exits right away). 30 Hz.
tail = 0.5 if label == "iox2" else 0.0
frames = max(1.0, (int(elapsed_ms) / 1000 - 1.5 - tail) * 30)
print(json.dumps({"rep": int(rep), "case": label, "nbuf": int(nbuf), "bytes": int(nbytes),
                  "p50_us": s["p50_us"], "p99_us": s["p99_us"], "frames_seen": s["frames_seen"], "torn": s["torn"],
                  "frames_counted": round(frames), **{k + "_per_frame": round(v / frames, 1) for k, v in c.items()}}))
PY
}

for rep in $(seq 1 "$REPS"); do
  for bytes in 4194304 16777216 67108864; do
    for nbuf in 1 2 4; do
      one "axon" $nbuf $bytes $rep "$B/bench" sub axon $bytes "$FRAMES" 30 0 -- "$B/bench" pub axon $bytes "$FRAMES" 30 0
    done
    one "iox2" 0 $bytes $rep "$B/iox2_bench" sub $bytes "$FRAMES" 30 -- "$B/iox2_bench" pub $bytes "$FRAMES" 30
  done
done
echo "[done] $OUT"
