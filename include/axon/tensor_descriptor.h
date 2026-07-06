// SPDX-License-Identifier: Apache-2.0
// axon — TensorDescriptor: payload of the Iceoryx2 SHM queue (metadata only)
//
// The actual tensor payload lives in an external dma-buf; only metadata flows
// through the message queue. The dma-buf FD itself is delivered through the
// SCM_RIGHTS / pidfd_getfd sidecar (sidecar.h).
//
// Fixed-size POD for determinism. No dynamic metadata region. max-rank 8.

#pragma once

#include <cstdint>
#include "axon/types.h"

namespace axon {

// Wire format. ABI-stable — bump wire_version on change.
struct alignas(8) TensorDescriptor {
    // Producer-allocated id used by the receiver to identify the FD that
    // arrived through the sidecar.
    BoHandle bo_handle;

    // Monotonically increasing version (used for seqlock + ABA prevention).
    SeqNo seqno;

    // Used to detect buffer pool reallocation. The consumer compares this with
    // its cached pool generation; on mismatch it triggers a sidecar
    // re-handshake.
    PoolGeneration pool_generation;

    // V4L2 hardware timestamp when supported, otherwise CLOCK_MONOTONIC_RAW.
    std::uint64_t capture_ts_ns;

    // CLOCK_MONOTONIC_RAW captured immediately before publish.
    std::uint64_t producer_publish_ts_ns;

    // Tensor shape.
    std::uint32_t shape[kMaxRank];

    // Offset into the dma-buf where the tensor begins.
    std::uint64_t offset;

    // Tensor size in bytes.
    std::uint64_t size;

    // When sync_fence_kind == SyncFileViaSidecar, this is the producer-side id
    // that the consumer uses to look up the sync_file FD in its sidecar cache.
    std::uint64_t sync_fence_token;

    // ---- v2: imaging / depth metadata ----
    // Row stride in bytes. 0 = tightly packed (= width * dtype_size(dtype)).
    // Depth/camera frames often have bytesperline > width*bpp — without this a
    // consumer indexes rows wrong.
    std::uint32_t row_pitch;
    // Scale converting a raw sample to physical units (e.g. metres per unit for
    // depth). 0 = unset / not applicable.
    float         depth_scale;
    // No-data sentinel value, interpreted per dtype bits (e.g. 0 or 65535 for
    // U16 depth). Meaningful only when sample_units != Unknown.
    std::uint32_t invalid_value;
    // Pinhole intrinsics for deprojection (all 0 = unset), referenced to
    // intr_ref_width x intr_ref_height so a resized frame can rescale them.
    float         intr_fx, intr_fy, intr_cx, intr_cy;
    std::uint32_t intr_ref_width, intr_ref_height;

    std::uint8_t  rank;              // Actual tensor rank
    DType         dtype;
    SyncFenceKind sync_fence_kind;
    SampleUnits   sample_units;      // v2: physical meaning of a sample
    CaptureClock  capture_clock;     // v2: clock domain of capture_ts_ns
    TensorLayout  layout;            // v2: memory layout
    std::uint8_t  reserved[6];       // Pad to 8-byte alignment
};

// Wire-format ABI checks. The exact-size pin catches accidental field reorders
// that would silently break the wire format across a producer/consumer pair.
static_assert(sizeof(TensorDescriptor) % 8 == 0,
              "TensorDescriptor must be 8-byte aligned");
static_assert(sizeof(TensorDescriptor) <= 256,
              "TensorDescriptor must fit in <= 256B for Iceoryx2 fixed-size payload");
static_assert(sizeof(TensorDescriptor) == 144,
              "TensorDescriptor wire size changed — bump kWireVersion deliberately");

// Wire version — bump on any wire-format change. Mismatched versions refuse to
// communicate. v2 added the imaging/depth metadata block above.
constexpr std::uint32_t kWireVersion = 2;

}  // namespace axon
