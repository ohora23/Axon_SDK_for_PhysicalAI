# ADR 0004: Latest-value-wins RT consumer (poll); inference off the RT path

- **Status**: Accepted
- **Date**: 2026-07-04
- **Related**: DesignFiles/detailed_design_doc.md §3, §4.3, §5 · PR #5, #6, #11 · `src/subscriber.cpp`, `src/rt.cpp`

## Context

A 1 kHz control loop must be deterministic. Two things threaten that: (1) inference latency
has a long, thermal-dependent tail (P99.99) that cannot be bounded, and (2) inside the loop,
any syscall or page fault injects non-deterministic latency. An event-driven "process every
message" consumer also couples the control loop to producer rate and queue depth.

## Decision

- **Split RT from non-RT.** Inference runs as a non-RT, best-effort worker. The RT loop only
  reads the **latest** descriptor via a seqlock (latest-value-wins: no queue, always the
  freshest frame) and applies a fallback policy (`LastKnownGood` / `ZeroCommand` / ...) when
  a read is stale. The RT loop guarantees only the **staleness bound** of the inference
  result, not inference timing.
- **Make the RT read syscall-free and fault-free.** FDs are attached and buffers mmap'd with
  `MAP_POPULATE` + prefault on the non-RT side; `mlockall(MCL_CURRENT|MCL_FUTURE|MCL_ONFAULT)`
  wires pages. The RT read is then a pure memory load — no `close(2)`, no allocation.

## Consequences

- **Positive**: control-loop determinism is decoupled from inference tail latency; staleness
  is bounded and measurable; the read path adds ~0 jitter.
- **Negative / cost**: the consumer **polls** (spends CPU even when idle) where a callback
  model would not — but a control loop already runs at a fixed rate, so the read is
  effectively free, and the CPU/copy savings dominate (measured below). Intermediate frames
  may be skipped by design (fine for control; a recording pipeline would use a different
  policy).

## Evidence (measured)

- **0 page faults per frame**: running the C++ demo at 100 vs 2000 frames, minor page-faults
  stayed at **2191** (identical at 20× the frames) and major (disk) faults were **0** — the
  RT loop adds no faults. Directly validates the `T_view ≈ 0` term of the staleness formula.
- **~0 syscalls per frame**: 16 transport syscalls total across ~964 frames (the one-time
  handshake), each frame being a seqlock store/load in shared memory.
- **Seqlock retries ~0**: MockSystem seqlock-retry distribution measured p50/p99/max = 0 in
  steady state.
- **Net effect**: despite paying for a 1 kHz polling loop that ROS2's event model skips,
  dczc still used **3.3× less CPU** at 295 MB/s and dropped **0** frames.

Detail: [docs/hardware-verification.md](../hardware-verification.md) §2–§3, [benchmarks/mock](../../benchmarks/mock/README.md).
