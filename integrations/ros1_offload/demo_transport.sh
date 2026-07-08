#!/bin/bash
# In-container demo for the M2 "axon" image_transport plugin. A standard
# image_transport publisher offers the "axon" transport; a standard subscriber
# picks it with TransportHints("axon"). The pixels move cross-process through the
# dma-buf sidecar (never serialized on the wire); only a small AxonImage
# descriptor rides /camera/image/axon.
set -e
source /opt/ros/noetic/setup.bash
source /catkin_ws/devel/setup.bash

WIDTH="${WIDTH:-640}"
HEIGHT="${HEIGHT:-480}"
RATE="${RATE:-30}"

roscore >/tmp/roscore.log 2>&1 & sleep 3

echo "=== 'axon' transport is registered with image_transport ==="
rosrun image_transport list_transports 2>/dev/null | sed 's/^/[list_transports] /' || true

rosrun axon_ros1 image_pub_node _width:="$WIDTH" _height:="$HEIGHT" _rate_hz:="$RATE" \
    >/tmp/image_pub.log 2>&1 &
sleep 2

echo "=== the axon sub-topic carries only the descriptor (not pixels) ==="
( timeout 3 rostopic hz /camera/image/axon 2>&1 | sed 's/^/[rostopic hz] /' ) &
echo "  AxonImage fields (no 'data' array — pixels are in the dma-buf):"
rostopic type /camera/image/axon 2>/dev/null | xargs -r rosmsg show 2>/dev/null | sed 's/^/    /' || true

echo "=== subscriber reads frames zero-copy via transport 'axon' (8s) ==="
timeout --signal=INT 8 rosrun axon_ros1 image_sub_node || true

echo "=== publisher log tail ==="; tail -3 /tmp/image_pub.log 2>/dev/null || true
kill -INT %1 2>/dev/null || true
