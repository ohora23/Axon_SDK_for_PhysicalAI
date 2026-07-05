#!/bin/bash
# In-container demo: roscore + producer + consumer, all on one host so the dczc
# SCM_RIGHTS sidecar (Unix socket) works. Shows that (1) the payload moves
# zero-copy via dma-buf, (2) the descriptor topic still works with rostopic/
# rosbag/watchdog.
set -e
source /opt/ros/noetic/setup.bash
source /catkin_ws/devel/setup.bash

BYTES="${BYTES:-1048576}"       # 1 MiB/frame
RATE="${RATE:-30}"

roscore >/tmp/roscore.log 2>&1 & sleep 3

rosrun dczc_ros1 producer_node _bytes:="$BYTES" _rate_hz:="$RATE" \
    >/tmp/producer.log 2>&1 &
sleep 1.5

echo "=== descriptor topic is a normal ROS1 topic (watchdog/rosbag/rosgraph work) ==="
( timeout 4 rostopic hz /dczc/tensor_desc 2>&1 | sed 's/^/[rostopic hz] /' ) &
( timeout 3 rosbag record -O /tmp/desc.bag /dczc/tensor_desc >/dev/null 2>&1 ) &

echo "=== consumer: reads payload zero-copy per descriptor (8s) ==="
timeout --signal=INT 8 rosrun dczc_ros1 consumer_node || true

echo "=== rosbag of the DESCRIPTOR topic only (payload never serialized) ==="
ls -la /tmp/desc.bag* 2>/dev/null || echo "(no bag)"
rosbag info /tmp/desc.bag 2>/dev/null | grep -iE "size|messages|topics|dczc" || true

kill -INT %1 2>/dev/null || true
echo "=== producer log tail ==="; tail -3 /tmp/producer.log 2>/dev/null || true
