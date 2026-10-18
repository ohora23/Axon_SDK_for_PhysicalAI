#!/bin/bash
# Produces the committed results of the same-process, fan-out and sync-cost sections, one unit at a
# time (one rep of run_intra.sh, one fan-out cell, ...). A sampler watches the machine while a
# unit runs. When another process uses the GPU's SMs, or more than 30 % of a CPU core, the unit
# is discarded and run again.
#   run_gated.sh            default C-states: intra_rep, intra_axon_ref, fanout_rep, sync_cost_rep
#   run_gated.sh pmqos      deep C-states held off (PM QoS 0): intra_qos_rep, fanout_qos_rep
HERE="$(cd "$(dirname "$0")" && pwd)"
MODE="${1:-default}"; REPS="${REPS:-3}"
R="$HERE/results"; T="$R/gated_tmp"; mkdir -p "$T"; FLAG="$T/foreign.flag"
source /opt/ros/jazzy/setup.bash; source "$HERE/install/setup.bash" || { echo "build first (run.sh)"; exit 1; }
export PATH=/usr/local/cuda/bin:$PATH RMW_IMPLEMENTATION=rmw_fastrtps_cpp ROS_DOMAIN_ID=77 AXON_BENCH_NBUF=4

# Our own processes; anything else above 30 % of a core, or on the SMs, dirties the unit.
OWN='bench|intra|sync_cost|mstream|colcon|python3|bash|top|awk|nvidia-smi|sed|grep|sleep|ros2|claude|node'
gpu_busy() { nvidia-smi pmon -c 1 -s u | awk -v own="$OWN" '!/^#/ && $3 ~ /C/ && $4 ~ /^[0-9]+$/ && $4 > 0 && $NF !~ own' | grep -c .; }
cpu_busy() { top -b -d 0.5 -n 2 -w 200 | awk -v own="$OWN" '/^top -/{n++} n==2 && $1 ~ /^[0-9]+$/ && $9+0 > 30 && $12 !~ own {print $12}' | head -3; }
sampler() { while :; do
  [ "$(gpu_busy)" -gt 0 ] && echo "$(date +%T) gpu" >> "$FLAG"
  c=$(cpu_busy); [ -n "$c" ] && echo "$(date +%T) cpu: $c" >> "$FLAG"
done; }
sampler & SP=$!
QP=
if [ "$MODE" = pmqos ]; then  # /dev/cpu_dma_latency is group plugdev on Ubuntu; released on exit
  python3 -c "import struct,time; f=open('/dev/cpu_dma_latency','wb',buffering=0); f.write(struct.pack('i',0)); time.sleep(1e9)" & QP=$!
fi
# The runners write intra_rep.jsonl / fanout_rep.jsonl; a pmqos pass must not clobber the default set.
for f in intra_rep fanout_rep; do [ -f "$R/$f.jsonl" ] && cp "$R/$f.jsonl" "$T/$f.keep"; done
restore() { [ "$MODE" = pmqos ] && for f in intra_rep fanout_rep; do [ -f "$T/$f.keep" ] && cp "$T/$f.keep" "$R/$f.jsonl"; done; }
trap 'kill $SP $QP 2>/dev/null; restore' EXIT

unit() {  # unit <label> <cmd...>: run until a clean pass
  local label=$1; shift
  while :; do
    while [ "$(gpu_busy)" -gt 0 ] || [ -n "$(cpu_busy)" ]; do sleep 20; done
    rm -f "$FLAG"; echo "== $label $(date +%T)"
    "$@" > "$T/unit.log" 2>&1
    [ -s "$FLAG" ] || return 0
    echo "   dirty: $(head -2 "$FLAG" | tr '\n' ' ')"
  done
}
append() {  # append <runner output> <acc> <rep>: drop the meta line, renumber rep 1 -> rep
  grep -v '"meta"' "$1" | sed "s/^{\"rep\":1,/{\"rep\":$3,/" >> "$2"
}
axon_ref() {  # the axon cross-process cells of the intra table, busy-polling consumer (bench.cpp default)
  local rep=$1 B="$HERE/install/axon_bench/lib/axon_bench/bench"; : > "$T/axref.out"
  for gpu in 0 1; do for b in 150528 1048576 4194304; do
    "$B" sub axon $b 500 100 $gpu > "$T/axref.sub" 2>/dev/null & local sp=$!
    sleep 1; "$B" pub axon $b 500 100 $gpu >/dev/null 2>&1; wait $sp
    grep '^{' "$T/axref.sub" | sed "s/^{/{\"rep\":$rep,/" >> "$T/axref.out"
  done; done
}
meta() { echo "{\"meta\":\"$1\",\"pm_qos_us\":$([ "$MODE" = pmqos ] && echo 0 || echo null),\"date\":\"$(date -Is)\",\"cpus_online\":\"$(cat /sys/devices/system/cpu/online)\",\"kernel\":\"$(uname -r)\",\"frames\":500,\"rate_hz\":100}"; }

sfx=$([ "$MODE" = pmqos ] && echo _qos)
meta intra > "$T/intra.acc"; meta fanout > "$T/fanout.acc"; meta axon_ref > "$T/axref.acc"
for rep in $(seq 1 "$REPS"); do
  unit "intra rep=$rep" "$HERE/run_intra.sh" 500 100 1; append "$R/intra_rep.jsonl" "$T/intra.acc" $rep
  if [ "$MODE" = default ]; then unit "axon_ref rep=$rep" axon_ref $rep; cat "$T/axref.out" >> "$T/axref.acc"; fi
  for cell in 0:150528 0:1048576 0:4194304 1:1048576; do
    FANOUT_CELL=$cell unit "fanout $cell rep=$rep" "$HERE/run_fanout.sh" 500 100 1
    append "$R/fanout_rep.jsonl" "$T/fanout.acc" $rep
  done
done
mv "$T/intra.acc" "$R/intra${sfx}_rep.jsonl"; mv "$T/fanout.acc" "$R/fanout${sfx}_rep.jsonl"
if [ "$MODE" = default ]; then
  mv "$T/axref.acc" "$R/intra_axon_ref.jsonl"
  unit "sync_cost" "$HERE/run_sync_cost.sh" 2000 3; cp "$T/unit.log" "$R/sync_cost_summary.txt"
fi
echo "== done $(date +%T)"
