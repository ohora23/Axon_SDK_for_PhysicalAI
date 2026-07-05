#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Build the dczc ROS1 offload image and run the demo (roscore + producer +
# consumer in one container). No robot, no special privileges — the Custom
# (memfd) pool + SCM_RIGHTS sidecar run entirely in-container.
set -euo pipefail
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO"

echo "[run] building dczc-ros1 image (dczc + catkin, first build ~a few min)..."
docker build -f integrations/ros1_offload/Dockerfile -t dczc-ros1 .

echo "[run] running demo (payload zero-copy via dma-buf; descriptor on ROS1 topic)..."
docker run --rm -e BYTES="${BYTES:-1048576}" -e RATE="${RATE:-30}" dczc-ros1
