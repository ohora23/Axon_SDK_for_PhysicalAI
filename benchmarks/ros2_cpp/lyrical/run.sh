#!/bin/bash
# Inside the axon-bench-lyrical container: the IPC event probe, then the rosidl::Buffer
# GPU -> GPU cell at three sizes, $REPS times. Prints one JSON line per consumer run.
FRAMES="${FRAMES:-500}"; RATE_HZ="${RATE_HZ:-100}"; REPS="${REPS:-3}"
source /opt/ros/lyrical/setup.bash
source /ws/install/setup.bash
export ROS_DOMAIN_ID=93 ROS_AUTOMATIC_DISCOVERY_RANGE=LOCALHOST RMW_IMPLEMENTATION=rmw_fastrtps_cpp  # as the backend's own tests
# The same Fast DDS profile as the host cells (../fastdds_zero_copy.xml).
export FASTRTPS_DEFAULT_PROFILES_FILE=/ws/fastdds_zero_copy.xml RMW_FASTRTPS_USE_QOS_FROM_XML=1
echo "{\"role\":\"meta\",\"cpus_online\":\"$(cat /sys/devices/system/cpu/online)\",\"cuda_buffer_backend\":\"$(dpkg-query -W -f '${Version}' ros-lyrical-cuda-buffer-backend)\",\"fastdds\":\"$(dpkg-query -W -f '${Version}' ros-lyrical-fastdds 2>/dev/null)\",\"date\":\"$(date -Iseconds)\"}"
/ws/ipc_event_probe
BIN=/ws/install/axon_bench_lyrical/lib/axon_bench_lyrical/rosidl_buffer_bench
for rep in $(seq 1 "$REPS"); do
  for nbytes in 150528 1048576 4194304; do
    $BIN sub $nbytes $FRAMES $RATE_HZ > /tmp/sub.json 2>/tmp/sub.err & SUB=$!
    sleep 1
    $BIN pub $nbytes $FRAMES $RATE_HZ 2>/tmp/pub.err || { echo "[FAIL] pub $nbytes"; tail -3 /tmp/pub.err; }
    wait $SUB || { echo "[FAIL] sub $nbytes"; tail -3 /tmp/sub.err; }
    sed "s/^{/{\"rep\":$rep,/" /tmp/sub.json
  done
done
