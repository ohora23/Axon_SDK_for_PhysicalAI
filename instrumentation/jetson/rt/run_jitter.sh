#!/bin/bash
# 1 kHz cyclictest on every CPU, SCHED_FIFO 95, mlockall: first idle, then under stress-ng
# (cpu + io + vm). Writes ct_<tag>_idle.txt / ct_<tag>_load.txt with a 1 ms histogram.
#
# usage: run_jitter.sh <tag> <seconds per run>
# needs: rt-tests, stress-ng, and RT priority for the user:
#   /etc/security/limits.d: '<user> - rtprio 99' and '<user> - memlock unlimited'
#   on a kernel with CONFIG_RT_GROUP_SCHED=y (stock Jetson) also:
#   echo 950000 | sudo tee /sys/fs/cgroup/cpu,cpuacct/user.slice/cpu.rt_runtime_us
set -u
tag=$1; dur=$2
ct() { cyclictest -m -p 95 -t -a -i 1000 -h 1000 -D "$dur" -q > "ct_${tag}_$1.txt" 2>&1; }
ct idle
stress-ng --cpu "$(nproc)" --io 2 --vm 2 --vm-bytes 512M --timeout "$((dur + 10))s" >/dev/null 2>&1 &
sleep 3; ct load; wait
grep -H -E "^# (Avg|Max) Latencies|Overflows" "ct_${tag}"_*.txt
