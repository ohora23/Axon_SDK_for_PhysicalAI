# axon ROS1 integration (M1 offload + M2 image_transport plugin)

Apply axon's zero-copy path to **ROS1** without rewriting to ROS2. ROS1's default
TCPROS serializes and copies **every** message, even between nodes on the same
host, so high-bandwidth topics (camera/LiDAR) are expensive. Two methods from the
expansion analysis, both built on the reusable, ROS-agnostic `axon_bridge`
(`include/axon_ros1/axon_bridge.h` — the pool + mmap + SCM_RIGHTS bootstrap):

- **M1 — descriptor-topic offload** (`producer_node` / `consumer_node`): you
  publish a small `TensorDescriptor` and read the payload by `bo_handle`.
- **M2 — a drop-in `axon` `image_transport` transport**: any standard
  `image_transport` publisher/subscriber gets zero-copy by selecting the `axon`
  transport — no axon-specific code in your nodes.

## Idea

```
ROS1 topic  ── TensorDescriptor (small: bo_handle, seqno, shape, ts...) ──►  metadata / liveness plane
axon sidecar ── dma-buf FDs via SCM_RIGHTS (once, at handshake) ──────────►  FD plane
shared dma-buf ── payload stays here, read zero-copy by bo_handle ────────►  memory plane
```

- The **descriptor topic is a normal ROS1 topic** → `rostopic hz`, `rosbag record`,
  rosgraph, and per-node **watchdog timers** all keep working (they watch the
  descriptor, which still flows at full rate).
- The **payload never serializes through ROS** — it lives in a axon dma-buf pool;
  the consumer maps the FDs once (delivered via the SCM_RIGHTS sidecar) and reads
  by `bo_handle`.
- Bonus: every descriptor carries `producer_publish_ts_ns`, so the consumer gets a
  **precise staleness** on each message (better than "did a message arrive in the
  last X ms").

## Run (this PC, no robot, no privileges)

```bash
integrations/ros1_offload/run.sh
```

This builds a `ros:noetic` image that compiles axon (g++-10, C++20) and a catkin
package (`axon_ros1`), then runs roscore + producer + consumer in one container.
`BYTES=` and `RATE=` override the payload size / publish rate.

## What the demo shows

- Consumer reads the seqno the producer stamped into the shared buffer →
  `payload_errors = 0` proves zero-copy across processes.
- `rostopic hz /axon/tensor_desc` and `rosbag record` work on the descriptor topic;
  the recorded bag is **tiny** (descriptors only) even though the payload is MiB-scale.
- A watchdog timer on the descriptor topic fires if the producer stalls (seqno
  stops advancing).

## M2 — drop-in `axon` image_transport plugin

```bash
DEMO=transport integrations/ros1_offload/run.sh
```

The test nodes use only the **standard** `image_transport` API — no axon code:

```cpp
// publisher: offers every transport, including "axon"
image_transport::Publisher pub = it.advertise("/camera/image", 1);
// subscriber: picks the axon transport → pixels arrive via the dma-buf
auto sub = it.subscribe("/camera/image", 5, cb, {}, image_transport::TransportHints("axon"));
```

Under the hood the `axon` transport publishes only an `AxonImage` descriptor on
`/camera/image/axon` (no `data` array — verify with `rosmsg show`) and moves the
pixels through the dma-buf sidecar. The service name is derived from the topic, so
publisher and subscriber rendezvous with no config. Verified in-container:
**232/232 frames delivered, 0 payload errors**, descriptor topic at full 30 Hz.

## Files

- `.../include/axon_ros1/axon_bridge.h` — reusable, ROS-agnostic pool+mmap+sidecar bootstrap (shared by M1 nodes and the M2 plugin; a future ROS2 wrapper reuses it verbatim)
- `.../msg/TensorDescriptor.msg`, `.../msg/AxonImage.msg` — the metadata-only messages
- `.../src/producer_node.cpp`, `.../src/consumer_node.cpp` — M1 nodes
- `.../src/axon_image_publisher.cpp`, `.../src/axon_image_subscriber.cpp` — M2 `image_transport` plugin (`axon` transport)
- `.../src/image_pub_node.cpp`, `.../src/image_sub_node.cpp` — M2 test nodes (standard image_transport API only)
- `.../axon_transport_plugins.xml` — pluginlib manifest
- `Dockerfile` / `demo.sh` (M1) / `demo_transport.sh` (M2) / `run.sh`

## Limits (see the analysis doc)

- **Single host only** — SCM_RIGHTS is same-host. Cross-machine links stay on TCPROS.
- **M2 boundary copies** — the plugin copies pixels into the pool (publisher) and
  out into the `sensor_msgs::Image` the callback expects (subscriber). The
  cross-process hop is still serialization-free via dma-buf; removing these last
  copies needs a custom allocator that aliases the dma-buf (future work). M1
  avoids them by having the producer write straight into the pool.
- ROS1 (Noetic) is EOL; the value is letting legacy ROS1 fleets offload hot topics
  without a ROS2 migration.
