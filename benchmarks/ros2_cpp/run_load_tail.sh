#!/usr/bin/env bash
# Tail-latency-under-load: measure p50/p90/p99/max for 3 transports × 2 sizes × 4 load conditions.
# Needs: stress-ng, iox2_bench built, per-CPU PM QoS writable via tools/udev/99-axon-cpu-latency.rules.
# Usage: run_load_tail.sh [reps=2]  ->  results/load_tail_rep.jsonl (OUT overrides)
# Pinned conditions give each process a whole physical core: producer CPUs 6,14 and consumer
# CPUs 7,15 (SMT siblings on this PC). A ROS 2 process runs executor and DDS threads besides the
# polling one; squeezed onto one logical CPU they wait behind it (603 us p50 at 1 MiB, idle), which
# would compare the pinning, not the transport. Load never runs on those four CPUs, and both
# producer CPUs hold a 0 us wake-up limit (no clock drop) while pinned.


HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

source /opt/ros/jazzy/setup.bash
source install/setup.bash
set -u  # after the ROS setup scripts, which read unset variables

export ROS_DOMAIN_ID=77
export AXON_BENCH_NBUF=4
export ROS_DISABLE_LOANED_MESSAGES=0
export FASTRTPS_DEFAULT_PROFILES_FILE="$HERE/fastdds_zero_copy.xml"
export RMW_FASTRTPS_USE_QOS_FROM_XML=1

BIN="$HERE/install/axon_bench/lib/axon_bench"

FRAMES="${FRAMES:-3000}"
HZ="${HZ:-100}"
REPS="${1:-2}"

T="${GATED_TMP:-$HOME/.cache/axon-gated}/lt"
mkdir -p "$T"

OUT="${OUT:-$HERE/results/load_tail_rep.jsonl}"

# --- PM QoS guard -----------------------------------------------------------
QS=(/sys/devices/system/cpu/cpu6/power/pm_qos_resume_latency_us /sys/devices/system/cpu/cpu14/power/pm_qos_resume_latency_us)
for Q in "${QS[@]}"; do
  [ "$(cat "$Q")" = 0 ] || { echo "$Q is $(cat "$Q"), not 0: a leftover hold? write 0 first" >&2; exit 1; }
done
set_q() { for Q in "${QS[@]}"; do echo "$1" > "$Q" || { echo "cannot write $Q (tools/udev/99-axon-cpu-latency.rules)" >&2; exit 1; }; done; }

# --- Parse online CPUs -------------------------------------------------------
parse_ranges() {
  # Convert "0-1,5-9,13-15" into "0 1 5 6 7 8 9 13 14 15"
  local ranges="$1" expanded=""
  IFS=',' read -ra parts <<< "$ranges"
  for part in "${parts[@]}"; do
    if [[ "$part" == *-* ]]; then
      IFS='-' read -r lo hi <<< "$part"
      for ((i=lo; i<=hi; i++)); do expanded="$expanded $i"; done
    else
      expanded="$expanded $part"
    fi
  done
  echo "$expanded"
}

read -ra ALL_CPUS <<< "$(parse_ranges "$(cat /sys/devices/system/cpu/online)")"
CPUS_ONLINE="$(cat /sys/devices/system/cpu/online)"
KERNEL="$(uname -r)"
PUB_CPUS=6,14; SUB_CPUS=7,15
PINS=(6 14 7 15)
OTHERS=()
for c in "${ALL_CPUS[@]}"; do
  skip=0
  for p in "${PINS[@]}"; do [ "$c" = "$p" ] && skip=1; done
  [ "$skip" -eq 0 ] && OTHERS+=("$c")
done

# --- Cleanup trap -------------------------------------------------------------
STRESS_PIDS=()
CONSUMPID=0

cleanup() {
  for pid in "${STRESS_PIDS[@]}"; do
    kill "$pid" 2>/dev/null || true
  done
  for pid in "${STRESS_PIDS[@]}"; do
    wait "$pid" 2>/dev/null || true
  done
  [ "$CONSUMPID" -gt 0 ] && kill "$CONSUMPID" 2>/dev/null || true
  [ "$CONSUMPID" -gt 0 ] && wait "$CONSUMPID" 2>/dev/null || true
  for Q in "${QS[@]}"; do echo 0 > "$Q" 2>/dev/null; done
  exit 130
}

trap cleanup INT TERM

trap 'for Q in "${QS[@]}"; do echo 0 > "$Q" 2>/dev/null; done' EXIT

# --- Helper: start/stop stress for a condition --------------------------------
start_stress_cpu() {
  for c in "${OTHERS[@]}"; do
    stress-ng --cpu 1 --taskset "$c" --quiet &
    STRESS_PIDS+=($!)
  done
}

start_stress_cpu_unpinned() {
  for c in "${ALL_CPUS[@]}"; do
    stress-ng --cpu 1 --taskset "$c" --quiet &
    STRESS_PIDS+=($!)
  done
}

start_stress_mem() {
  stress-ng --stream 1 --taskset 0 --quiet &
  STRESS_PIDS+=($!)
  stress-ng --stream 1 --taskset 1 --quiet &
  STRESS_PIDS+=($!)
}

stop_stress() {
  for pid in "${STRESS_PIDS[@]}"; do
    kill "$pid" 2>/dev/null || true
  done
  for pid in "${STRESS_PIDS[@]}"; do
    wait "$pid" 2>/dev/null || true
  done
  STRESS_PIDS=()
}

# --- Write meta line (overwrite) ----------------------------------------------
python3 -I - "$CPUS_ONLINE" "$KERNEL" "$FRAMES" "$HZ" <<'PY' > "$OUT"
import sys, subprocess
cpus_online = sys.argv[1]
kernel = sys.argv[2]
frames = int(sys.argv[3])
hz = int(sys.argv[4])
date = subprocess.check_output(["date", "-Is"]).decode().strip()
import json
obj = {
    "meta": "load_tail",
    "date": date,
    "cpus_online": cpus_online,
    "kernel": kernel,
    "frames": frames,
    "rate_hz": hz,
    "nbuf": 4,
    "pub_cpus": "6,14", "sub_cpus": "7,15"
}
print(json.dumps(obj))
PY

# --- Transports & sizes -------------------------------------------------------
TRANSPORTS="axon ros_loan iox2"
SIZES="1048576 4194304"
LOADS="idle cpu mem cpu_unpinned"

# --- Main loops ---------------------------------------------------------------
for rep in $(seq 1 "$REPS"); do
  for load in $LOADS; do

    # --- Pinned vs unpinned -------------------------------------------------
    if [[ "$load" == "cpu_unpinned" ]]; then
      PINNED=false
    else
      PINNED=true
    fi

    # --- Set up load --------------------------------------------------------
    case "$load" in
      cpu)
        start_stress_cpu
        set_q n/a
        ;;
      mem)
        start_stress_mem
        set_q n/a
        ;;
      cpu_unpinned)
        start_stress_cpu_unpinned
        ;;
      idle)
        set_q n/a
        ;;
    esac

    for b in $SIZES; do
      for tr in $TRANSPORTS; do

        # Build commands based on transport
        if [ "$tr" = "iox2" ]; then
          SUB=("$BIN/iox2_bench" sub "$b" "$FRAMES" "$HZ")
          PUB=("$BIN/iox2_bench" pub "$b" "$FRAMES" "$HZ")
        else
          SUB=("$BIN/bench" sub "$tr" "$b" "$FRAMES" "$HZ" 0)
          PUB=("$BIN/bench" pub "$tr" "$b" "$FRAMES" "$HZ" 0)
        fi

        # Apply taskset for pinned conditions
        if [ "$PINNED" = true ] && [ "$load" != "cpu_unpinned" ]; then
          SUB=(taskset -c "$SUB_CPUS" "${SUB[@]}")
          PUB=(taskset -c "$PUB_CPUS" "${PUB[@]}")
        fi

        # Run cell: consumer then producer
        "${SUB[@]}" > "$T/c.sub" 2> "$T/c.err" &
        CONSUMPID=$!

        sleep 1

        "${PUB[@]}" > "$T/c.pub" 2>> "$T/c.err"
        PEXIT=$?

        if [ "$PEXIT" -ne 0 ]; then
          echo "producer failed: $load $tr $b" >&2
          cat "$T/c.err" >&2
          stop_stress
          exit 1
        fi

        wait "$CONSUMPID"
        CWAIT=$?
        CONSUMPID=0

        if [ "$CWAIT" -ne 0 ]; then
          echo "consumer failed: $load $tr $b (exit $CWAIT)" >&2
          cat "$T/c.err" >&2
          stop_stress
          exit 1
        fi

        # Append enriched JSON line
        python3 -I - "$T/c.sub" "$rep" "$load" "$tr" "$b" "$PINNED" <<'PY' >> "$OUT"
import sys, json

csub_path = sys.argv[1]
rep = int(sys.argv[2])
load = sys.argv[3]
tr = sys.argv[4]
b = int(sys.argv[5])
pinned = sys.argv[6] == "true"

# Read first line starting with '{'
cjson_line = None
with open(csub_path, "r") as f:
    for line in f:
        stripped = line.strip()
        if stripped.startswith("{"):
            cjson_line = stripped
            break

if cjson_line is None:
    cjson_line = '{"failed":1}'

c = json.loads(cjson_line)
out = {
    "rep": rep,
    "load": load,
    "transport": tr,
    "bytes": b,
    "pinned": pinned,
    "p50_us": c.get("p50_us", 0),
    "p90_us": c.get("p90_us", 0),
    "p99_us": c.get("p99_us", 0),
    "max_us": c.get("max_us", 0),
    "frames_seen": c.get("frames_seen", 0),
    "torn": c.get("torn", 0),
    **({"failed": 1} if c.get("failed") else {}),
}
print(json.dumps(out))
PY

      done
    done

    # --- Teardown load ------------------------------------------------------
    stop_stress

    # Restore PM QoS after pinned conditions
    [ "$PINNED" = true ] && set_q 0

  done
done

echo "[done] $OUT"
