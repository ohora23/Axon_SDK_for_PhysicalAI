# ADR 0003: Pluggable metadata backend — seqlock default, Iceoryx2 optional

- **Status**: Accepted
- **Date**: 2026-07-04
- **Related**: DesignFiles/detailed_design_doc.md §1.1, §3.3 · PR #9 · `src/metadata_channel*.cpp`, `docs/metadata-backends.md`

## Context

The design names Iceoryx2's lock-free SHM queue as the metadata plane. But (a) Iceoryx2 is a
heavy external dependency (Rust core + C++ bindings, built from source), and (b) for the
first build's single-host, single-producer, latest-value access pattern, a full queue is
more than the RT consumer needs — it wants "the newest descriptor, now."

## Decision

Make the metadata plane an **abstract interface** (`axon::detail::MetadataChannel`) with two
interchangeable backends:

- **Seqlock** (`src/metadata_channel.cpp`) — a single-slot, seqlock-protected
  `TensorDescriptor` in a POSIX shared-memory object. Zero dependencies, always built,
  default.
- **Iceoryx2** (`src/metadata_channel_iox2.cpp`, `-DAXON_WITH_ICEORYX2=ON`) — the real
  lock-free `publish_subscribe<TensorDescriptor>` queue.

Backend chosen at runtime via `AXON_METADATA_BACKEND`; Iceoryx2 init failure falls back to
seqlock. This makes the design-doc promise — "swapping in Iceoryx2 changes only this file" —
literally true.

## Consequences

- **Positive**: the tree builds and is fully testable with zero external deps; Iceoryx2 is a
  drop-in when its strengths (service discovery, QoS/history, multi-process fan-out,
  cross-language) are needed; `TensorPublisher`/`TensorSubscriber` are unchanged either way.
- **Negative / cost**: two implementations to keep behavior-compatible (covered by the same
  `tests/test_seqlock.cpp` contract, which pins the seqlock backend explicitly).

## Evidence (measured — Iceoryx2 v0.9.2)

- **Both backends verified**: default and `-DAXON_WITH_ICEORYX2=ON` builds each pass 7/7
  ctest, warning-clean; the e2e demo runs over Iceoryx2 (confirmed by the iox2 SHM node
  segment in `/dev/shm`), 0 payload errors.
- **Seqlock is the better default here**: in a local A/B run the seqlock backend showed lower
  p50 end-to-end staleness than Iceoryx2 for the single-host latest-value pattern (Iceoryx2's
  queue + `receive()` adds buffering); seqlock also survives 110,993 concurrent writes vs
  200,000 reads with **0 torn reads**.

Detail: [docs/metadata-backends.md](../metadata-backends.md).
