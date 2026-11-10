#!/bin/bash
# Capture what holds a CPU when cyclictest latency exceeds 1 ms (stops tracing at the breach).
set -u
cd "$(dirname "$0")"
T=/sys/kernel/tracing; [ -e $T/tracing_on ] || T=/sys/kernel/debug/tracing
[ -e $T/tracing_on ] || mount -t tracefs nodev /sys/kernel/tracing
T=/sys/kernel/tracing; [ -e $T/tracing_on ] || T=/sys/kernel/debug/tracing
echo 0 > $T/tracing_on; echo > $T/trace; echo nop > $T/current_tracer
echo 16384 > $T/buffer_size_kb
for e in sched/sched_switch sched/sched_wakeup irq/irq_handler_entry irq/irq_handler_exit irq/softirq_entry irq/softirq_exit ipi; do
  [ -e $T/events/$e/enable ] && echo 1 > $T/events/$e/enable
done
echo 1 > $T/tracing_on
stress-ng --cpu 8 --io 2 --vm 2 --vm-bytes 512M --timeout 200s >/dev/null 2>&1 & SP=$!
sleep 3
cyclictest -m -p 95 -t -a -i 1000 -b 1000 -D 180 -q > ct_break.txt 2>&1
kill $SP 2>/dev/null; wait $SP 2>/dev/null
echo 0 > $T/tracing_on
cp $T/trace trace_break.txt
for e in sched/sched_switch sched/sched_wakeup irq/irq_handler_entry irq/irq_handler_exit irq/softirq_entry irq/softirq_exit ipi; do
  [ -e $T/events/$e/enable ] && echo 0 > $T/events/$e/enable
done
echo > $T/trace; echo 1408 > $T/buffer_size_kb
chown "${SUDO_USER:-root}" ct_break.txt trace_break.txt
echo done > trace_break.done
