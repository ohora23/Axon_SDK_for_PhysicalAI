#!/bin/bash
# Capture what holds a CPU when cyclictest latency exceeds 1 ms (stops tracing at the breach).
# usage (root): trace_break.sh <tag>  ->  results/ct_break_<tag>.txt, results/trace_break_<tag>.txt
set -u
tag=${1:?usage: trace_break.sh <tag>}
cd "$(dirname "$0")"; mkdir -p results
T=/sys/kernel/tracing; [ -e $T/tracing_on ] || T=/sys/kernel/debug/tracing
[ -e $T/tracing_on ] || mount -t tracefs nodev /sys/kernel/tracing
T=/sys/kernel/tracing; [ -e $T/tracing_on ] || T=/sys/kernel/debug/tracing
buf0=$(cat $T/buffer_size_kb)
EV="sched/sched_switch sched/sched_wakeup irq/irq_handler_entry irq/irq_handler_exit irq/softirq_entry irq/softirq_exit ipi"
SP=""
restore() {
  [ -n "$SP" ] && kill $SP 2>/dev/null
  echo 0 > $T/tracing_on
  for e in $EV; do [ -e $T/events/$e/enable ] && echo 0 > $T/events/$e/enable; done
  echo > $T/trace; echo "$buf0" > $T/buffer_size_kb
}
trap restore EXIT
echo 0 > $T/tracing_on; echo > $T/trace; echo nop > $T/current_tracer
echo 16384 > $T/buffer_size_kb
for e in $EV; do [ -e $T/events/$e/enable ] && echo 1 > $T/events/$e/enable; done
echo 1 > $T/tracing_on
stress-ng --cpu 8 --io 2 --vm 2 --vm-bytes 512M --timeout 200s >/dev/null 2>&1 & SP=$!
sleep 3
cyclictest -m -p 95 -t -a -i 1000 -d 0 -b 1000 -D 180 -q > results/ct_break_$tag.txt 2>&1
kill $SP 2>/dev/null; wait $SP 2>/dev/null; SP=""
echo 0 > $T/tracing_on
cp $T/trace results/trace_break_$tag.txt
chown "${SUDO_USER:-root}" results/ct_break_$tag.txt results/trace_break_$tag.txt
echo "[done] results/trace_break_$tag.txt"
