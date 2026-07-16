#!/usr/bin/env bash
# axon ROS2 offload demo runner. Builds the axon core static lib + the ROS2
# package, then launches the producer and consumer nodes and reports the
# consumer's verdict (0 iff frames were read with no payload errors).
#
# Native ROS2 (tested on Jazzy) — no Docker needed.
# (no `set -u`: ROS2 setup.bash references unbound vars.)

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
WS="$HERE/ros2_ws"
FRAMES="${FRAMES:-90}"
ROS_SETUP="${ROS_SETUP:-/opt/ros/jazzy/setup.bash}"

# 1. axon core static lib (libaxon.a) — the ROS2 package links it.
if [ ! -f "$REPO/build/libaxon.a" ]; then
  echo "== building axon core =="
  cmake -S "$REPO" -B "$REPO/build" -DCMAKE_BUILD_TYPE=Release >/dev/null
  cmake --build "$REPO/build" -j"$(nproc)" >/dev/null
fi

# 2. build the ROS2 package.
# shellcheck disable=SC1090
source "$ROS_SETUP"
( cd "$WS" && colcon build --packages-select axon_ros2 ) || exit 1
# shellcheck disable=SC1091
source "$WS/install/setup.bash"

echo "== axon ROS2 offload demo (descriptor on DDS, payload zero-copy via sidecar) =="

# 3. run producer (continuous) + consumer (exits after FRAMES).
ros2 run axon_ros2 producer_node >/tmp/axon_ros2_producer.log 2>&1 &
cleanup() { pkill -9 -x producer_node 2>/dev/null; }
trap cleanup EXIT
sleep 3

ros2 run axon_ros2 consumer_node --ros-args -p frames:="$FRAMES"
RC=$?

cleanup
echo "== consumer exit code: $RC =="
exit "$RC"
