// SPDX-License-Identifier: Apache-2.0
// dczc — TensorDescriptor: payload of the Iceoryx2 SHM queue (metadata only)
//
// The actual tensor payload lives in an external dma-buf; only metadata flows
// through the message queue. The dma-buf FD itself is delivered through the
// SCM_RIGHTS / pidfd_getfd sidecar (sidecar.h).
//
// Fixed-size POD for determinism. No dynamic metadata region. max-rank 8.

#pragma once

#include <cstdint>
#include "dczc/types.h"

namespace dczc {

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

    std::uint8_t  rank;              // Actual tensor rank
    DType         dtype;
    SyncFenceKind sync_fence_kind;
    std::uint8_t  reserved[5];       // Pad to 8-byte alignment
};

// Wire-format ABI checks.
static_assert(sizeof(TensorDescriptor) % 8 == 0,
              "TensorDescriptor must be 8-byte aligned");
static_assert(sizeof(TensorDescriptor) <= 256,
              "TensorDescriptor must fit in <= 256B for Iceoryx2 fixed-size payload");

// Wire version — bump on any wire-format change. Mismatched versions refuse to
// communicate.
constexpr std::uint32_t kWireVersion = 1;

}  // namespace dczc
