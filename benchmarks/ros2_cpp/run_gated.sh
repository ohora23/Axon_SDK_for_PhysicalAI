#!/bin/bash
# Produces the committed results of the same-process, fan-out and sync-cost sections, one unit at a
# time (one rep of run_intra.sh, one fan-out cell, ...). A sampler watches the machine while a
# unit runs. When another process uses the GPU's SMs, or more than 30 % of a CPU core, the unit
# is discarded and run again.
#   run_gated.sh            default C-states: intra_rep, intra_axon_ref, fanout_rep, sync_cost_rep
#   run_gated.sh pmqos      deep C-states held off (PM QoS 0): intra_qos_rep, fanout_qos_rep
#   run_gated.sh gpu        GPU -> GPU, consumer copy vs consumer kernel, without and with CUDA MPS:
#                           consumer_kernel_rep.jsonl (cudaipc, axon, ros_loan, rosidl_buffer, intra)
#   run_gated.sh sizes      64 B - 64 MiB, host (axon, ros_loan, iox2) and GPU 16/64 MiB (axon, ros_loan):
#                           sizes_rep.jsonl; iox2 needs iox2_bench built (iceoryx2 C++ bindings)
#   run_gated.sh rmw        host cells over rmw_zenoh_cpp (shared memory on, and off at 1 MiB) and
#                           rmw_cyclonedds_cpp + iceoryx (loaned), with Fast DDS and axon at 1 MiB in the
#                           same session: rmw_rep.jsonl. Needs ros-jazzy-rmw-zenoh-cpp, ros-jazzy-rmw-cyclonedds-cpp
HERE="$(cd "$(dirname "$0")" && pwd)"
MODE="${1:-default}"; REPS="${REPS:-3}"
# Scratch outside the repo: an IDE watching the tree reacts to every file written there.
R="$HERE/results"; T="${GATED_TMP:-$HOME/.cache/axon-gated}"; mkdir -p "$T"; FLAG="$T/foreign.flag"
source /opt/ros/jazzy/setup.bash; source "$HERE/install/setup.bash" || { echo "build first (run.sh)"; exit 1; }
export PATH=/usr/local/cuda/bin:$PATH RMW_IMPLEMENTATION=rmw_fastrtps_cpp ROS_DOMAIN_ID=77 AXON_BENCH_NBUF=4

# Our own processes; anything else above 30 % of a core, or on the SMs, dirties the unit.
OWN='bench|intra|sync_cost|mstream|cudaipc_bench|ipc_event_probe|rosidl_buffer|docker|containerd|colcon|python3|bash|top|awk|nvidia-smi|sed|grep|sleep|ros2|claude|node|rmw_zenohd|iox-roudi'
# On the GPU only the benchmark binaries are ours: a python3 or node process on the SMs is foreign.
# pmon truncates names, so this is a prefix match (a foreign process named bench* would pass), and under
# MPS other clients' work shows up as nvidia-cuda-mps-server.
GPU_OWN='^(bench|intra|sync_cost|mstream|cudaipc_bench|ipc_event_probe|rosidl_buffer|nvidia-cuda-mps)'
gpu_busy() { nvidia-smi pmon -c 1 -s u | awk -v own="$GPU_OWN" '!/^#/ && $3 ~ /C/ && $4 ~ /^[0-9]+$/ && $4 > 0 && $NF !~ own' | grep -c .; }
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

if [ "$MODE" = gpu ]; then
  CUDAIPC_WS="${CUDAIPC_WS:-$HOME/repositories/ros2_cuda_ipc}"
  source "$CUDAIPC_WS/install/setup.bash" && source "$HERE/install/setup.bash"
  OUT="$R/consumer_kernel_rep.jsonl"; meta consumer_kernel > "$T/ck.acc"
  export CUDA_MPS_PIPE_DIRECTORY="$T/mps-pipe" CUDA_MPS_LOG_DIRECTORY="$T/mps-log"
  mkdir -p "$CUDA_MPS_PIPE_DIRECTORY" "$CUDA_MPS_LOG_DIRECTORY"
  # Tag rep and mps; consumer_kernel is printed by the consumers themselves.
  tag() { sed "s/^{\"rep\":[0-9]*,/{\"rep\":$3,\"mps\":$1,/; t; s/^{/{\"rep\":$3,\"mps\":$1,/"; }
  intra_gpu() { for b in 150528 1048576 4194304; do "$HERE/install/axon_bench/lib/axon_bench/intra" $b 500 100 1 poll; done > "$T/intra_gpu.out" 2>/dev/null; }
  lyrical() {
    local u=(); [ "$1" = 1 ] && u=(--user "$(id -u):$(id -g)" -e HOME=/tmp -v "$CUDA_MPS_PIPE_DIRECTORY:/mps-pipe" -e CUDA_MPS_PIPE_DIRECTORY=/mps-pipe)
    docker run --rm --runtime=nvidia -e NVIDIA_VISIBLE_DEVICES=all -e NVIDIA_DRIVER_CAPABILITIES=compute,utility \
      --ipc=host --pid=host "${u[@]}" -e REPS=1 -e BENCH_CONSUMER_KERNEL="$2" axon-bench-lyrical > "$T/lyrical.out" 2>/dev/null
  }
  for mps in 0 1; do
    [ $mps = 1 ] && { nvidia-cuda-mps-control -d || exit 1; trap 'echo quit | nvidia-cuda-mps-control; kill $SP $QP 2>/dev/null' EXIT; }
    for k in 0 1; do
      export BENCH_CONSUMER_KERNEL=$k
      for rep in $(seq 1 "$REPS"); do
        CUDAIPC_EXTRAS=0 NO_BUILD=1 RESULTS_DIR="$T/cudaipc" unit "cudaipc+axon+ros_loan mps=$mps kernel=$k rep=$rep" "$HERE/run_cudaipc.sh" 500 100 1
        grep -v '"role":"meta"' "$(ls -t "$T"/cudaipc/cudaipc_*.jsonl | head -1)" | tag $mps $k $rep >> "$T/ck.acc"  # newest, in case the date rolled over
        unit "intra mps=$mps kernel=$k rep=$rep" intra_gpu
        grep '^{' "$T/intra_gpu.out" | tag $mps $k $rep >> "$T/ck.acc"
        unit "rosidl_buffer mps=$mps kernel=$k rep=$rep" lyrical $mps $k
        grep '"role":"sub"' "$T/lyrical.out" | tag $mps $k $rep >> "$T/ck.acc"
      done
    done
    [ $mps = 1 ] && echo quit | nvidia-cuda-mps-control
  done
  mv "$T/ck.acc" "$OUT"; echo "== done $(date +%T)"; exit 0
fi

if [ "$MODE" = sizes ] || [ "$MODE" = rmw ]; then
  export ROS_DISABLE_LOANED_MESSAGES=0 FASTRTPS_DEFAULT_PROFILES_FILE="$HERE/fastdds_zero_copy.xml" RMW_FASTRTPS_USE_QOS_FROM_XML=1
  BIN="$HERE/install/axon_bench/lib/axon_bench"
  rate_of() { case $1 in 64|8640) echo "2000 1000";; 16777216|67108864) echo "300 30";; *) echo "500 100";; esac; }
  cells() {  # cells <gpu> <transports> <sizes...>: one consumer + producer per cell, into $T/sizes.out
    local gpu=$1 trs=$2; shift 2; : > "$T/sizes.out"
    for b in "$@"; do for tr in $trs; do
      read -r n hz < <(rate_of $b)
      if [ $tr = iox2 ]; then sub=("$BIN/iox2_bench" sub $b $n $hz); pub=("$BIN/iox2_bench" pub $b $n $hz)
      else sub=("$BIN/bench" sub $tr $b $n $hz $gpu); pub=("$BIN/bench" pub $tr $b $n $hz $gpu); fi
      "${sub[@]}" > "$T/c.sub" 2> "$T/c.err" & local sp=$!
      sleep 1; "${pub[@]}" > "$T/c.pub" 2>> "$T/c.err"; wait $sp
      [ -s "$T/c.sub" ] || echo "{\"role\":\"sub\",\"transport\":\"$tr\",\"bytes\":$b,\"gpu\":$gpu,\"failed\":1}" > "$T/c.sub"
      cat "$T/c.pub" "$T/c.sub" | grep '^{' >> "$T/sizes.out"
    done; done
  }
  if [ "$MODE" = rmw ]; then
    # Each unit starts its own router / RouDi and tags lines with the RMW and its shared-memory setting.
    with_rmw() {  # with_rmw <zenoh|zenoh_noshm|cyclone|fastdds> <transports> <sizes...>
      local which=$1; shift; local bg=
      case $which in
        zenoh|zenoh_noshm) export RMW_IMPLEMENTATION=rmw_zenoh_cpp
          if [ $which = zenoh ]; then export ZENOH_SESSION_CONFIG_URI="$HERE/zenoh_shm_session.json5"; else unset ZENOH_SESSION_CONFIG_URI; fi
          ros2 run rmw_zenoh_cpp rmw_zenohd > "$T/router.log" 2>&1 & bg=$!; sleep 2 ;;
        cyclone) export RMW_IMPLEMENTATION=rmw_cyclonedds_cpp CYCLONEDDS_URI="file://$HERE/cyclonedds_shm.xml"
          iox-roudi -c "$HERE/iox_roudi_mempools.toml" > "$T/router.log" 2>&1 & bg=$!; sleep 2 ;;
        fastdds) export RMW_IMPLEMENTATION=rmw_fastrtps_cpp ;;
      esac
      cells 0 "$@"
      sed -i "s/^{/{\"rmw\":\"$which\",/" "$T/sizes.out"
      if [ -n "$bg" ]; then pkill -P $bg 2>/dev/null; kill $bg 2>/dev/null; wait $bg 2>/dev/null; fi
      unset CYCLONEDDS_URI ZENOH_SESSION_CONFIG_URI; export RMW_IMPLEMENTATION=rmw_fastrtps_cpp
    }
    meta rmw > "$T/rmw.acc"
    for rep in $(seq 1 "$REPS"); do
      for u in "zenoh|ros_dyn|64 8640 150528" "zenoh|ros_dyn|1048576 4194304" "zenoh|ros_dyn|16777216" "zenoh_noshm|ros_dyn|1048576" \
               "cyclone|ros_loan|64 8640 150528" "cyclone|ros_loan ros_dyn|1048576" "cyclone|ros_loan|4194304" "fastdds|ros_loan ros_dyn axon|1048576"; do
        IFS='|' read -r w trs sz <<< "$u"
        unit "rmw $w $trs $sz rep=$rep" with_rmw $w "$trs" $sz
        sed "s/^{/{\"rep\":$rep,/" "$T/sizes.out" >> "$T/rmw.acc"
      done
    done
    mv "$T/rmw.acc" "$R/rmw_rep.jsonl"; echo "== done $(date +%T)"; exit 0
  fi
  meta sizes > "$T/sizes.acc"
  for rep in $(seq 1 "$REPS"); do
    for u in "0|ros_loan axon iox2|64 8640" "0|ros_loan axon iox2|150528 1048576 4194304" "0|ros_loan axon iox2|16777216" \
             "0|ros_loan axon iox2|67108864" "1|ros_loan axon|16777216 67108864"; do
      IFS='|' read -r g trs sz <<< "$u"
      unit "sizes gpu=$g $sz rep=$rep" cells $g "$trs" $sz
      sed "s/^{/{\"rep\":$rep,/" "$T/sizes.out" >> "$T/sizes.acc"
    done
  done
  mv "$T/sizes.acc" "$R/sizes_rep.jsonl"; echo "== done $(date +%T)"; exit 0
fi

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
