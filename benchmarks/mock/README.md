# MockSystem — multi-stream robot workload (dczc vs ROS2)

Where `benchmarks/` (one stream) measures raw per-message latency, **MockSystem**
models the *concurrent sensor traffic of a real serving/mobile robot* and watches
how each transport behaves as the aggregate bandwidth scales. This is where the
project's thesis shows up as a curve, not a single number:

> ROS2/DDS serializes and copies the **whole payload every frame, on both sides**,
> so its cost scales with bandwidth. dczc sends only a fixed-size descriptor
> through the metadata plane and keeps the tensor in a shared dma-buf, so its cost
> is ~flat in payload size.

## The workload

A representative serving-robot sensor set (`mock_common.py` — a plausible default,
not any proprietary spec; edit freely):

| stream | size | rate | role |
|---|---|---|---|
| cam_front / left / right RGB | 640×480×2 (YUYV) | 30 Hz | perception |
| depth_front | 640×480×2 | 30 Hz | perception |
| lidar_2d | ~8.6 KB | 15 Hz | navigation |
| imu | 64 B | 200 Hz | proprioception |
| wheel_odom | 48 B | 100 Hz | proprioception |
| cmd_vel | 48 B | 50 Hz | control |

Base aggregate ≈ **74 MB/s**. The `--scale` knob replicates the heavy
camera/depth streams (scale 2 = twice the cameras) to sweep toward saturation.

- **dczc side** (`mock_dczc.py`): one publisher per stream; a single **1 kHz RT
  loop** reads the latest of every stream each tick (the dczc usage model, design
  doc §4.3). Payload stays in the dma-buf; only descriptors cross.
- **ROS2 side** (`mock_ros2.py`): one topic per stream (`UInt8MultiArray`,
  sensor-data QoS); an event-driven subscriber callback per message.

Both stamp a monotonic timestamp + seqno into each frame, so latency and delivery
(dropped frames) are measured identically.

## Metrics

- **Saturation** — achieved vs offered MB/s as scale rises.
- **Per-stream latency** under load (camera p50/p99).
- **Delivery ratio** — distinct frames observed / sent (drops).
- **CPU** — cores used for the same workload.

## Run

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DDCZC_BUILD_PYTHON=ON
cmake --build build -j
python3 benchmarks/mock/mock_compare.py --scales 1,2,3,4 --seconds 5
```

(`mock_compare.py` runs dczc against the built module and ROS2 in a sourced
subshell; it skips either side gracefully if its runtime is absent.)

## Example sweep

Dev machine (Ubuntu 24.04, ROS2 Jazzy / Fast-RTPS, non-RT kernel), 4 s per point.

| scale | offered | backend | achieved | delivery | worst stream | CPU (cores) | cam p50/p99 (µs) |
|---|---|---|---|---|---|---|---|
| 1 | 74 MB/s | dczc | 74 | 1.000 | 1.000 | **0.26** | 578 / 1073 |
| 1 | 74 MB/s | ROS2 | 73 | 0.998 | 0.992 | 0.77 | 636 / 851 |
| 2 | 148 MB/s | dczc | 148 | 1.000 | 1.000 | **0.28** | 548 / 1086 |
| 2 | 148 MB/s | ROS2 | 146 | 0.995 | 0.983 | 0.83 | 580 / 976 |
| 3 | 221 MB/s | dczc | 221 | 1.000 | 1.000 | **0.29** | 578 / 1084 |
| 3 | 221 MB/s | ROS2 | 218 | 0.993 | 0.967 | 0.88 | 471 / 784 |
| 4 | 295 MB/s | dczc | 295 | 1.000 | 1.000 | **0.30** | 597 / 1095 |
| 4 | 295 MB/s | ROS2 | 291 | 0.993 | 0.975 | 0.98 | 508 / 772 |

### Takeaways

1. **CPU: dczc is flat, ROS2 scales with bandwidth.** As offered load grew 4×
   (74 → 295 MB/s), dczc CPU barely moved (0.26 → 0.30 cores) while ROS2 climbed
   (0.77 → 0.98). At scale 4, **ROS2 uses 3.3× the CPU** for the same data — the
   cost of serializing and copying every payload on both sides. dczc wins *despite*
   paying for a 1 kHz polling loop that ROS2's event model doesn't.
2. **Delivery: dczc drops nothing; ROS2 drops under load.** dczc held 100%
   delivery at every scale; ROS2's worst-stream delivery fell to ~0.967 (frames
   lost to best-effort DDS under load).
3. **Latency is comparable** (~0.5–1.1 ms) — dczc's is dominated by the 1 kHz
   control tick, ROS2's by callback scheduling — so the win is in CPU headroom and
   zero drops, not tail latency here.

The headroom is the point: on a robot's shared compute, the CPU dczc *doesn't*
spend on data plumbing is CPU available for perception and control. The next step
(with hardware) adds eBPF `copy_to_user` counts and `cyclictest` jitter to this
same harness.
