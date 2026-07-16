# axon ROS2 offload

**English** | [한국어](README.ko.md)

A ROS2 (Jazzy) integration that keeps the tensor **payload in a shared dma-buf**
while only a fixed-size **descriptor** rides the ROS2 topic. The payload is
delivered once through the axon `SCM_RIGHTS` sidecar; the DDS layer carries just
the metadata — so `ros2 topic`, `ros2 bag`, and per-message watchdogs keep working
while the tensor moves cross-process with **zero copies**.

The pool + mmap + sidecar bootstrap is the **same ROS-agnostic `axon_bridge.h`** the
ROS1 wrapper uses — the nodes here are only the rclcpp glue over it.

```
 producer_node (rclcpp)                     consumer_node (rclcpp)
 ┌───────────────────────┐                  ┌────────────────────────────┐
 │ write seqno into       │  TensorDescriptor│ read payload from shared    │
 │ axon dma-buf pool      │   on DDS topic   │ dma-buf (zero copy)         │
 │ publish descriptor     │ ───────────────► │ check integrity + staleness │
 └───────────────────────┘   (payload stays  └────────────────────────────┘
      FDs once via sidecar     in dma-buf)
```

## Why (vs plain DDS / rmw_iceoryx)

For **host** shared memory, ROS2 already has `rmw_iceoryx`. axon's angle here is:

- **Bounded staleness on every message** — `now − producer_publish_ts` is carried and
  measured per frame (mean ~160 µs, max ~330 µs on this box). That RT-freshness
  guarantee is what plain DDS does not give, and it is directly usable in safety
  logic.
- **The foundation for the GPU path** — the same descriptor-offload extends to an
  Accelerator (CUDA VMM) pool so a ROS2 consumer receives a GPU buffer with no host
  copy (next step; see the roadmap).

The QoS is **keep-last(1), best-effort** on both ends, deliberately mirroring axon's
**latest-value-wins** delivery: the consumer always processes the freshest
descriptor and never builds a stale backlog that would read an already-overwritten
pool slot.

## Requirements

- ROS2 (tested on **Jazzy**) — native, **no Docker**.
- The axon core static lib. The runner builds it for you, or:
  `cmake -S . -B build && cmake --build build -j`

## Run

```bash
integrations/ros2_offload/run_demo.sh          # builds core + package, runs the demo
FRAMES=200 integrations/ros2_offload/run_demo.sh
```

Expected tail:

```
────── axon ROS2 offload — consumer summary ──────
  frames read:      90
  payload errors:   0   (must be 0 — cross-process zero-copy)
  staleness:        mean=160us  max=332us
  payload path:     shared dma-buf (never serialized through DDS)
──────────────────────────────────────────────────
```

Or by hand (two terminals, after `source install/setup.bash` in `ros2_ws`):

```bash
ros2 run axon_ros2 producer_node
ros2 run axon_ros2 consumer_node --ros-args -p frames:=90
```

## Layout

- `ros2_ws/src/axon_ros2/include/axon_ros2/axon_bridge.h` — the reusable ROS-free
  bridge (verbatim copy of the ROS1 header).
- `msg/TensorDescriptor.msg` — the metadata-only message (no payload).
- `src/producer_node.cpp`, `src/consumer_node.cpp` — rclcpp nodes.
- `run_demo.sh` — build + run + verdict.

## Sidecar buffer limit

The pool defaults to **32** buffers — the sidecar delivers all pool FDs in one
`SCM_RIGHTS` message, capped at 32 FDs. Keep `n_buffers ≤ 32`, and deep enough that
the consumer can't lap a slot mid-read (see the sizing note in
[`docs/usage.md`](../../docs/usage.md)).
