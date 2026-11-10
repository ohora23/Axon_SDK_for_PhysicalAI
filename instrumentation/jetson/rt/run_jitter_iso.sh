#!/bin/bash
# Run on Xavier as in the Orin report: MAXN (set it first: nvpmodel -m 0) + jetson_clocks + CPUs 4-7
# isolated + RT throttling off; cyclictest on 4-7 only. Clocks and throttling are restored on exit.
# usage (root): run_jitter_iso.sh <tag> <seconds>
set -u
cd "$(dirname "$0")"; mkdir -p results; cd results
tag=$1; dur=$2
{ uname -a; nvpmodel -q | head -1; cat /proc/cmdline; echo "isolated=$(cat /sys/devices/system/cpu/isolated) nohz_full=$(cat /sys/devices/system/cpu/nohz_full 2>/dev/null)"; } > env_$tag.txt
rt0=$(cat /proc/sys/kernel/sched_rt_runtime_us); jc=$(mktemp)
jetson_clocks --store "$jc"
trap 'echo "$rt0" > /proc/sys/kernel/sched_rt_runtime_us; jetson_clocks --restore "$jc"; rm -f "$jc"' EXIT
nvpmodel -q | grep -q MAXN || echo "warning: power mode is not MAXN" >&2
jetson_clocks; jetson_clocks --show >> env_$tag.txt 2>&1
echo -1 > /proc/sys/kernel/sched_rt_runtime_us
ct() { cyclictest -m -p 95 -t 4 -a 4-7 -i 1000 -h 1000 -D "$dur" -q > "ct_${tag}_$1.txt" 2>&1; }
ct idle
stress-ng --cpu 4 --io 2 --vm 2 --vm-bytes 512M --timeout "$((dur + 10))s" >/dev/null 2>&1 &
sleep 3; ct load; wait
chown "${SUDO_USER:-root}" ct_${tag}_* env_$tag.txt
echo "[done] results/ct_${tag}_{idle,load}.txt"
