#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Build the axon ROS1 image and run a demo in one container. No robot, no special
# privileges — the Custom (memfd) pool + SCM_RIGHTS sidecar run entirely
# in-container.
#
#   ./run.sh              # M1: descriptor-topic offload (producer/consumer nodes)
#   DEMO=transport ./run.sh   # M2: the drop-in "axon" image_transport plugin
set -euo pipefail
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO"

echo "[run] building axon-ros1 image (axon + catkin, first build ~a few min)..."
docker build -f integrations/ros1_offload/Dockerfile -t axon-ros1 .

if [ "${DEMO:-m1}" = "transport" ]; then
    echo "[run] M2 demo: drop-in 'axon' image_transport plugin (pixels via dma-buf)..."
    docker run --rm -e WIDTH="${WIDTH:-640}" -e HEIGHT="${HEIGHT:-480}" -e RATE="${RATE:-30}" \
        axon-ros1 /demo_transport.sh
else
    echo "[run] M1 demo (payload zero-copy via dma-buf; descriptor on ROS1 topic)..."
    docker run --rm -e BYTES="${BYTES:-1048576}" -e RATE="${RATE:-30}" axon-ros1
fi
