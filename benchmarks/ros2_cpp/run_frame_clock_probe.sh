#!/bin/bash
# Per-frame producer clock for a 30 Hz, 16 MiB host producer (axon, 1 buffer): pub on CPU 6, sub on
# CPU 7. Each frame logs the consumer latency (BENCH_SAMPLE_LOG), the producer's write time and
# cycles (BENCH_FRAME_LOG, perf_event_open: needs kernel.perf_event_paranoid <= 1) and the C1
# entries during the sleep before it. Settings, interleaved per round: no limit, CpuLatencyHold on
# CPU 6 at 20 / 1 / 0 µs, and waking early to spin (BENCH_PREWAKE_US) for 250 µs to 4 ms.
# Per-CPU holds need tools/udev/99-axon-cpu-latency.rules without root. Build first (run.sh): this
# script uses install/ as it is.
#   run_frame_clock_probe.sh [rounds=2]  ->  results/frame_clock.jsonl (one line per run)
HERE="$(cd "$(dirname "$0")" && pwd)"; cd "$HERE"
ROUNDS="${1:-2}"; OUT="${OUT:-results/frame_clock.jsonl}"
source /opt/ros/jazzy/setup.bash; source install/setup.bash; export ROS_DOMAIN_ID=77 AXON_BENCH_NBUF=1
B=install/axon_bench/lib/axon_bench; T="${GATED_TMP:-$HOME/.cache/axon-gated}/fc"; mkdir -p "$T"
Q=/sys/devices/system/cpu/cpu6/power/pm_qos_resume_latency_us
[ "$(cat "$Q")" = 0 ] || { echo "CPU 6 already has a wake-up limit ($(cat "$Q")): a leftover hold? write 0 first" >&2; exit 1; }
echo "{\"meta\":\"frame_clock\",\"date\":\"$(date -Is)\",\"cpus_online\":\"$(cat /sys/devices/system/cpu/online)\",\"driver\":\"$(cat /sys/devices/system/cpu/cpu6/cpufreq/scaling_driver)\",\"epp\":\"$(cat /sys/devices/system/cpu/cpu6/cpufreq/energy_performance_preference)\"}" > "$OUT"
sp=""
trap 'kill $sp 2>/dev/null' EXIT
trap 'kill $sp 2>/dev/null; echo 0 > "$Q" 2>/dev/null; exit 130' INT TERM  # a killed pub cannot restore its hold
one() { local label=$1 round=$2; shift 2
  rm -f "$T/p" "$T/s"
  BENCH_SAMPLE_LOG="$T/s" taskset -c 7 "$B/bench" sub axon 16777216 300 30 0 > /dev/null 2> "$T/sub.err" & sp=$!; sleep 1
  env "$@" BENCH_FRAME_LOG="$T/p" taskset -c 6 "$B/bench" pub axon 16777216 300 30 0 > /dev/null 2> "$T/pub.err" \
    || { echo "pub failed ($label):" >&2; cat "$T/pub.err" >&2; exit 1; }
  wait $sp || { echo "sub failed ($label):" >&2; cat "$T/sub.err" >&2; exit 1; }; sp=""
  [ "$(cat "$Q")" = 0 ] || { echo "hold on CPU 6 not restored after $label" >&2; exit 1; }
  [ -s "$T/p" ] && [ -s "$T/s" ] || { echo "no per-frame log from $label (stale install/?)" >&2; exit 1; }
  python3 -I - "$label" "$round" "$T/p" "$T/s" >> "$OUT" <<'PY'
import json, sys
label, rnd, pub, sub = sys.argv[1:5]
# pub line: seq write_ns cycles, then (usage time) per idle state, read after that frame's publish
P = {int(x[0]): [int(v) for v in x[1:]] for x in (l.split() for l in open(pub))}
S = {int(q): float(us) for q, us in (l.split() for l in open(sub))}
seqs = [q for q in sorted(S) if q in P and q - 1 in P]
print(json.dumps({"label": label, "round": int(rnd), "seq": seqs, "lat_us": [S[q] for q in seqs],
                  "write_ns": [P[q][0] for q in seqs], "cycles": [P[q][1] for q in seqs],
                  "c1_entries": [P[q][4] - P[q - 1][4] for q in seqs]}))
PY
}
for r in $(seq 1 "$ROUNDS"); do
  one none $r X=1
  one hold20 $r BENCH_CPU_LATENCY_US=20 BENCH_CPU_LATENCY_CPU=6
  one hold1 $r BENCH_CPU_LATENCY_US=1 BENCH_CPU_LATENCY_CPU=6
  one hold0 $r BENCH_CPU_LATENCY_US=0 BENCH_CPU_LATENCY_CPU=6
  for pw in 250 500 1000 2000 4000; do one prewake$pw $r BENCH_PREWAKE_US=$pw; done
  one hold1_prewake1000 $r BENCH_CPU_LATENCY_US=1 BENCH_CPU_LATENCY_CPU=6 BENCH_PREWAKE_US=1000
done
echo "[done] $OUT"
