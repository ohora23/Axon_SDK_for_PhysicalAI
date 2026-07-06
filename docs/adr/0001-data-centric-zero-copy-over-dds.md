# ADR 0001: Data-centric zero-copy instead of ROS2/DDS serialization

- **Status**: Accepted
- **Date**: 2026-07-04
- **Related**: DesignFiles/detailed_design_doc.md §0–§1, §5 · PR #5, #8, #10, #11

## Context

Physical AI on a robot needs high-bandwidth sensor data (multiple cameras, LiDAR) to reach
an accelerator and then a 1 kHz control loop with **low, bounded** latency. The standard
stack — ROS2 on DDS — serializes and copies the **entire payload on every message, on both
the publish and receive side**. That cost scales with payload size, so as sensor resolution
and stream count grow, latency, CPU, and memory bandwidth all grow with them, and the
staleness of any given frame becomes non-deterministic (depends on queue depth and load).

## Decision

Keep the tensor **in place** in a shared `dma-buf` and move only a fixed-size
`TensorDescriptor` (≤ 256 B POD) through the metadata queue. Consumers reference the same
physical buffer; nothing re-serializes or re-copies the payload per frame. Decompose the
data path into five planes (metadata / FD / memory / time / sync), each mapped to a proven
Linux primitive. Transport cost becomes **O(1) in payload size**.

## Consequences

- **Positive**: cost is flat in bandwidth; freed CPU/memory is available for perception and
  control; staleness is a sum of explicit, measurable terms (design §5), usable in safety
  analysis.
- **Negative / cost**: more moving parts than a single DDS dependency (FD sidecar, seqlock
  slot, pool lifecycle); Linux-only (dma-buf, V4L2, SCM_RIGHTS); the RT consumer polls
  rather than being event-driven (see [ADR 0004](0004-latest-value-rt-consumer.md)).

## Evidence (measured — RTX 5080 / Ryzen 9800X3D vs ROS2/Fast-RTPS)

- **Latency**: single-stream 1 MiB — 46 µs vs 971 µs (**20.9×**); the gap widens with payload.
- **CPU flat vs bandwidth**: 74 → 295 MB/s moved axon 0.26 → 0.30 cores vs ROS2 0.77 → 0.98
  (**3.3×** at 295 MB/s), with axon dropping **0** frames vs ROS2's ~96.7 % worst stream.
- **Memory**: same delivered bytes → ROS2 spends **8.6×** the cache-misses, **5×** the
  instructions.

Detail: [benchmarks/mock](../../benchmarks/mock/README.md), [docs/hardware-verification.md](../hardware-verification.md).
