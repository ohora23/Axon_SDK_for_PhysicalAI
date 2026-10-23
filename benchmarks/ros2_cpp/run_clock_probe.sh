#!/bin/bash
# The clock a 30 Hz producer runs its 16 MiB write at, by placement: on the SMT sibling of the
# busy-polling consumer's core, on its own idle core, and on its own core with PM QoS 0 (no deep
# C-states). Average clock = cycles / task-clock of the producer. Needs kernel.perf_event_paranoid <= 1.
# CPU numbers are for this PC (online 0-1,5-9,13-15; siblings 6/14, 7/15): pass PUB_SIB PUB_OWN SUB.
#   run_clock_probe.sh [sibling_cpu=15] [own_cpu=6] [sub_cpu=7]  ->  results/clock_probe.txt
HERE="$(cd "$(dirname "$0")" && pwd)"; cd "$HERE"
SIB="${1:-15}"; OWN_CPU="${2:-6}"; SUB="${3:-7}"
source /opt/ros/jazzy/setup.bash; source install/setup.bash; export ROS_DOMAIN_ID=77 AXON_BENCH_NBUF=1
P="${PERF:-/usr/lib/linux-tools/7.0.0-38-generic/perf}"; B=install/axon_bench/lib/axon_bench
T="${GATED_TMP:-$HOME/.cache/axon-gated}"; mkdir -p "$T"; OUT=results/clock_probe.txt
echo "# $(date -Is) cpus_online=$(cat /sys/devices/system/cpu/online) 16 MiB host, axon 1 buffer, 150 frames at 30 Hz" > "$OUT"
one() { local pub=$1 label=$2
  taskset -c "$SUB" $B/bench sub axon 16777216 150 30 0 > "$T/f.sub" 2>/dev/null & local sp=$!; sleep 1
  taskset -c "$pub" "$P" stat -x, --delay 1500 -e task-clock,cycles -o "$T/f.perf" $B/bench pub axon 16777216 150 30 0 >/dev/null 2>&1; wait $sp
  python3 -I - "$label" "$T/f.perf" "$T/f.sub" <<'PY' | tee -a "$OUT"
import json, sys
c = {}
for l in open(sys.argv[2]):
    f = l.strip().split(",")
    if len(f) > 2 and f[0][:1].isdigit(): c[f[2]] = float(f[0])
s = json.loads(next(l for l in open(sys.argv[3]) if l.startswith("{")))
print(f"{sys.argv[1]:44s} p50 {s['p50_us']:6.1f} us  avg clock {c['cycles'] / (c['task-clock'] * 1e6):.2f} GHz")
PY
}
for r in 1 2; do
  one "$SIB" "producer on the consumer core's SMT sibling"
  one "$OWN_CPU" "producer on its own idle core"
  python3 -c "import struct,time; f=open('/dev/cpu_dma_latency','wb',buffering=0); f.write(struct.pack('i',0)); time.sleep(30)" & q=$!; sleep 0.5
  one "$OWN_CPU" "producer on its own core, PM QoS 0"
  kill $q
done
