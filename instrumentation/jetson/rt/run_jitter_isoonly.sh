#!/bin/bash
# Isolation only: power mode as set (30W), no jetson_clocks, RT throttling default; cyclictest on isolated CPUs 4-7.
# usage (root): run_jitter_isoonly.sh <tag> <seconds>
set -u
cd "$(dirname "$0")"; mkdir -p results; cd results
tag=$1; dur=$2
{ uname -a; nvpmodel -q | head -1; cat /proc/cmdline; echo "isolated=$(cat /sys/devices/system/cpu/isolated) rt_runtime=$(cat /proc/sys/kernel/sched_rt_runtime_us)"; jetson_clocks --show 2>&1 | grep -E "^cpu4|^EMC|^GPU"; } > env_$tag.txt
ct() { cyclictest -m -p 95 -t 4 -a 4-7 -i 1000 -h 1000 -D "$dur" -q > "ct_${tag}_$1.txt" 2>&1; }
ct idle
stress-ng --cpu 4 --io 2 --vm 2 --vm-bytes 512M --timeout "$((dur + 10))s" >/dev/null 2>&1 &
sleep 3; ct load; wait
chown "${SUDO_USER:-root}" ct_${tag}_* env_$tag.txt
echo "[done] results/ct_${tag}_{idle,load}.txt"
