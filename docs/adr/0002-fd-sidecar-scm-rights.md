# ADR 0002: Deliver FDs via a SCM_RIGHTS sidecar, separate from the metadata queue

- **Status**: Accepted
- **Date**: 2026-07-04
- **Related**: DesignFiles/detailed_design_doc.md §1.3, §2.3 · PR #5, #11 · `src/sidecar.cpp`

## Context

A shared-memory metadata queue (Iceoryx2, or the seqlock stand-in) shares **memory**, not
the **file-descriptor table**. Writing an integer FD into a shared slot is meaningless in
another process — FD tables are per-process. The dma-buf FD that names the payload buffer
therefore has to travel through a channel that actually transfers descriptors across the
process boundary.

## Decision

Deliver FDs out-of-band through a **Unix-domain-socket sidecar using `SCM_RIGHTS`** (the ①
FD plane), with `pidfd_getfd(2)` as a connectionless fallback. The whole buffer pool's FDs
are delivered **once** at the subscribe handshake; every subsequent frame carries only
metadata, so the sidecar cost is amortized to ~0 in steady state. The RT consumer never
`close(2)`s (a syscall with non-deterministic latency); the non-RT worker owns FD lifetime.

## Consequences

- **Positive**: correct cross-process buffer sharing with a one-time cost; the same
  primitive generalizes to **any** kernel-shareable handle, including GPU memory handles
  (design §2.3: `bo_handle` → GPU handle) — proven below.
- **Negative / cost**: requires a socket connection at handshake; a late-joining consumer
  must be re-handshaked (handled by `publish()` polling for new connections).

## Evidence (measured)

- **Cross-process integrity**: `tests/test_sidecar.cpp` passes an FD across a fork; the same
  kernel object arrives (verified by reading its contents).
- **Not per-frame**: under the MockSystem load, axon issued **16 transport syscalls total**
  (8 streams × one handshake `sendmsg`) across ~964 frames — **~0 per frame** — vs ROS2's
  10,187 total (25×).
- **Carries a real GPU handle**: the RTX 5080 demo exports CUDA VMM memory as a POSIX FD,
  ships it through `axon::detail::send_fds`, and the consumer GPU imports the *same physical
  memory* — 200/200 frames validated on-GPU, **0 host payload copies**, 1.68 GB moved
  zero-copy across the process boundary.

Detail: [docs/hardware-verification.md](../hardware-verification.md) §1, §3.
