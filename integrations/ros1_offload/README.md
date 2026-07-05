# dczc ROS1 offload (E-M1)

Apply dczc's zero-copy path to **ROS1** without rewriting to ROS2 — the "M1"
method from the expansion analysis. ROS1's default TCPROS serializes and copies
**every** message, even between nodes on the same host, so high-bandwidth topics
(camera/LiDAR) are expensive. This PoC keeps ROS1 for the small stuff and moves
the payload zero-copy underneath.

## Idea

```
ROS1 topic  ── TensorDescriptor (small: bo_handle, seqno, shape, ts...) ──►  metadata / liveness plane
dczc sidecar ── dma-buf FDs via SCM_RIGHTS (once, at handshake) ──────────►  FD plane
shared dma-buf ── payload stays here, read zero-copy by bo_handle ────────►  memory plane
```

- The **descriptor topic is a normal ROS1 topic** → `rostopic hz`, `rosbag record`,
  rosgraph, and per-node **watchdog timers** all keep working (they watch the
  descriptor, which still flows at full rate).
- The **payload never serializes through ROS** — it lives in a dczc dma-buf pool;
  the consumer maps the FDs once (delivered via the SCM_RIGHTS sidecar) and reads
  by `bo_handle`.
- Bonus: every descriptor carries `producer_publish_ts_ns`, so the consumer gets a
  **precise staleness** on each message (better than "did a message arrive in the
  last X ms").

## Run (this PC, no robot, no privileges)

```bash
integrations/ros1_offload/run.sh
```

This builds a `ros:noetic` image that compiles dczc (g++-10, C++20) and a catkin
package (`dczc_ros1`), then runs roscore + producer + consumer in one container.
`BYTES=` and `RATE=` override the payload size / publish rate.

## What the demo shows

- Consumer reads the seqno the producer stamped into the shared buffer →
  `payload_errors = 0` proves zero-copy across processes.
- `rostopic hz /dczc/tensor_desc` and `rosbag record` work on the descriptor topic;
  the recorded bag is **tiny** (descriptors only) even though the payload is MiB-scale.
- A watchdog timer on the descriptor topic fires if the producer stalls (seqno
  stops advancing).

## Files

- `catkin_ws/src/dczc_ros1/msg/TensorDescriptor.msg` — the metadata-only message
- `.../src/producer_node.cpp` — dczc pool + `SidecarServer` + ROS publisher
- `.../src/consumer_node.cpp` — `SidecarClient` (FDs) + ROS subscriber (zero-copy read) + watchdog
- `Dockerfile` / `demo.sh` / `run.sh`

## Limits (see the analysis doc)

- **Single host only** — SCM_RIGHTS is same-host. Cross-machine links stay on TCPROS.
- **True zero-copy at the callback boundary** would need a custom allocator so a
  `sensor_msgs` pointer aliases the dma-buf; this PoC exposes the buffer via the
  descriptor's `bo_handle` instead (the M2 `image_transport` plugin is the drop-in path).
- ROS1 (Noetic) is EOL; the value is letting legacy ROS1 fleets offload hot topics
  without a ROS2 migration.
