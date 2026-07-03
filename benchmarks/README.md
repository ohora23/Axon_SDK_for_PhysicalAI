# dczc vs ROS2 latency benchmark (design doc §8.2)

Quantifies the project's central thesis: ROS2's DDS path serializes and copies the
**entire payload** on every frame, while dczc sends only a fixed-size descriptor
through its metadata queue and keeps the tensor in a shared dma-buf. So dczc
latency should be both **lower** and **roughly flat in payload size**, where DDS
latency **grows with payload**.

## What is measured

One-way, publish→observe latency: the publisher stamps `CLOCK_MONOTONIC_RAW` into
the first bytes of each frame; the consumer computes `now - stamp`. Same clock,
same host, same payload size, same rate — directly comparable.

- **dczc** — `bench_dczc.py`: producer/consumer processes over the real sidecar +
  seqlock + Custom dma-buf pool. Only the descriptor crosses the metadata queue.
- **ROS2** — `bench_ros2.py`: `UInt8MultiArray` over the default Fast-RTPS DDS, QoS
  `sensor_data` (best-effort, keep-last depth 1) to mirror dczc's latest-value
  seqlock semantics. Publisher and subscriber are spawned as separate processes.

## Run

```bash
# Build the dczc Python module first
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DDCZC_BUILD_PYTHON=ON
cmake --build build -j

# Compare (auto-locates the module; runs ROS2 in a sourced subshell)
python3 benchmarks/compare.py --frames 500 --rate-hz 200 --bytes 150528
```

`compare.py` skips either side gracefully if its runtime is missing, so it is
useful even with only one of the two installed. The individual harnesses can also
be run directly (`bench_dczc.py`, `bench_ros2.py` — the latter needs
`source /opt/ros/<distro>/setup.bash`).

## Example results

Dev machine (Ubuntu 24.04, ROS2 Jazzy / Fast-RTPS, non-RT kernel, no CPU
isolation). Numbers are machine-specific — reproduce on your target. Latency in µs.

**147 KiB payload (≈224×224×3), 500 frames @ 200 Hz**

| transport | p50 | p90 | p99 | max |
|---|---|---|---|---|
| dczc (zero-copy) | 18.4 | 30.6 | 48.2 | 583 |
| ROS2 (Fast-RTPS) | 165.2 | 257.3 | 571.5 | 615 |
| **speedup** | **9.0×** | | **11.8×** | |

**1 MiB payload, 300 frames @ 120 Hz**

| transport | p50 | p90 | p99 | max | frames delivered |
|---|---|---|---|---|---|
| dczc (zero-copy) | 46.4 | 76.3 | 237.9 | 4981 | 300 / 300 |
| ROS2 (Fast-RTPS) | 970.6 | 1103.0 | 1272.5 | 1290 | 291 / 300 |
| **speedup** | **20.9×** | | **5.3×** | | |

### Takeaways

1. **dczc is ~9–20× lower p50 latency** across payload sizes.
2. **Payload scaling is the story.** From 147 KiB → 1 MiB, dczc's p50 grew ~2.5×
   (just the producer's write into the dma-buf), while ROS2's grew ~6× — it pays
   to serialize and copy the whole buffer every frame. The gap widens with size.
3. **Under load ROS2 dropped frames** (291/300, best-effort DDS); dczc delivered
   every frame — its metadata queue is O(1) in payload size.

These are the "sensor → command" and "inter-device copies" rows of the design
doc §8.1 table. The next step (with hardware) adds `cyclictest` jitter and eBPF
`copy_to_user` counts to the same harness.
