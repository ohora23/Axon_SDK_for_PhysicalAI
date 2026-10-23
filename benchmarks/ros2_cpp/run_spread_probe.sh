#!/bin/bash
# Residual session-to-session spread at a held clock: 16 MiB host, axon 1 buffer, 30 Hz, pub CPU 6
# (CpuLatencyHold 1 us on CPU 6), sub CPU 7. Conditions interleaved per round:
#   A 300 frames, B 150 frames, C 300 frames + L3 thrasher on CPU 0 (copies TH_MB MiB, default 96,
#   into a second buffer, then sleeps TH_SLEEP s, default 0.005). CONDS picks conditions (default "A B C").
# Needs kernel.perf_event_paranoid <= 1 and per-CPU PM QoS writable (tools/udev/99-axon-cpu-latency.rules).
#   run_spread_probe.sh [rounds=6]  ->  results/spread_probe.jsonl (OUT overrides; APPEND=1 keeps it)
#   dose-response (results/spread_dose.jsonl):
#     for mb in 4 8 16 32 48; do APPEND=1 TH_MB=$mb TH_SLEEP=0.033 CONDS="A C" OUT=results/spread_dose.jsonl run_spread_probe.sh 2; done
# Per-frame counts divide by frames - 45 (the 1.5 s perf delay at 30 Hz); setup before the first
# frame makes that slightly low, more so at 150 frames. p50 comes from the consumer and is not affected.
HERE="$(cd "$(dirname "$0")" && pwd)"; cd "$HERE"
ROUNDS="${1:-6}"; OUT="${OUT:-results/spread_probe.jsonl}"
source /opt/ros/jazzy/setup.bash; source install/setup.bash; export ROS_DOMAIN_ID=77 AXON_BENCH_NBUF=1
P=/usr/lib/linux-tools/7.0.0-38-generic/perf; B=install/axon_bench/lib/axon_bench; T="${GATED_TMP:-$HOME/.cache/axon-gated}"; mkdir -p "$T"
EV=task-clock,cycles,instructions,ls_any_fills_from_sys.local_l2,ls_any_fills_from_sys.local_ccx,ls_any_fills_from_sys.dram_io_all,ls_l1_d_tlb_miss.all  # 6 hardware events: no multiplexing on Zen 5
TH_MB="${TH_MB:-96}"; TH_SLEEP="${TH_SLEEP:-0.005}"
[ "$APPEND" = 1 ] || : > "$OUT"
echo "{\"meta\":\"spread_probe\",\"date\":\"$(date -Is)\",\"cpus_online\":\"$(cat /sys/devices/system/cpu/online)\",\"thrasher_mb\":$TH_MB,\"thrasher_sleep_s\":$TH_SLEEP}" >> "$OUT"
trap 'kill $th $sp 2>/dev/null' EXIT INT TERM
busy() { awk '/^cpu /{print $2+$3+$4+$7+$8, $2+$3+$4+$5+$6+$7+$8}' /proc/stat; }
th=""; sp=""
one() { local cond=$1 frames=$2 round=$3; th=""
  if [ "$cond" = C ]; then
    taskset -c 0 python3 -c "
import time
a=bytearray($TH_MB<<20); b=bytearray($TH_MB<<20)
while True: b[:]=a; time.sleep($TH_SLEEP)" & th=$!; sleep 1; fi
  taskset -c 7 $B/bench sub axon 16777216 $frames 30 0 > $T/sp.sub 2>/dev/null & sp=$!; sleep 1
  read b0 t0 < <(busy)
  BENCH_CPU_LATENCY_US=1 BENCH_CPU_LATENCY_CPU=6 taskset -c 6 $P stat -x, --delay 1500 -e $EV -o $T/sp.perf \
    $B/bench pub axon 16777216 $frames 30 0 > /dev/null 2>&1
  read b1 t1 < <(busy); wait $sp; sp=""; [ -n "$th" ] && kill $th && wait $th 2>/dev/null; th=""
  python3 -I - "$cond" "$frames" "$round" $T/sp.perf $T/sp.sub $((b1-b0)) $((t1-t0)) $([ "$cond" = C ] && echo $TH_MB || echo 0) >> "$OUT" <<'PY'
import json, sys
cond, frames, rnd, perf, sub, db, dt, th_mb = sys.argv[1:9]
c = {}
for l in open(perf):
    f = l.strip().split(",")
    if len(f) > 2 and f[0][:1].isdigit(): c[f[2]] = float(f[0])
s = json.loads(next(l for l in open(sub) if l.startswith("{")))
n = max(1, int(frames) - 45)  # frames inside the counting window (1.5 s delay at 30 Hz)
print(json.dumps({"round": int(rnd), "cond": cond, "thrasher_mb": int(th_mb), "frames": int(frames), "p50_us": s["p50_us"], "p90_us": s["p90_us"],
  "seen": s["frames_seen"], "ghz": round(c["cycles"] / (c["task-clock"] * 1e6), 2), "sys_busy": round(int(db) / max(1, int(dt)), 3),
  **{k.split(".")[-1] + "_pf": round(v / n) for k, v in c.items() if k != "task-clock"}}))
PY
}
for r in $(seq 1 "$ROUNDS"); do for c in ${CONDS:-A B C}; do case $c in A) one A 300 $r;; B) one B 150 $r;; C) one C 300 $r;; esac; done; done
echo "[done] $OUT"
