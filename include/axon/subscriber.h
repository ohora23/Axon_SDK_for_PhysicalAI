// SPDX-License-Identifier: Apache-2.0
// axon — TensorSubscriber: reads metadata from Iceoryx2 + attaches FDs from the sidecar.
//
// The RT consumer only calls latest_view(); attach and lifecycle are handled
// on the non-RT side.

#pragma once

#include <memory>
#include <optional>
#include <string_view>
#include "axon/tensor_descriptor.h"
#include "axon/types.h"

namespace axon {

struct TensorView {
    void*             data;          // dma-buf host mmap region (UMA)
    AcceleratorHandle accel_handle;  // Accelerator import handle (relevant on NUMA)
    Shape             shape;
    DType             dtype;
    std::uint64_t     staleness_ns;  // Measured staleness (for RT safety analysis)
    SeqNo             seqno;
    int               sync_fd;       // Fence FD when SyncFileViaSidecar, else -1
    int               seqlock_retries; // RT diagnostics (track P99 distribution)

    // ---- v2: imaging / depth metadata (forwarded from the descriptor) ----
    std::uint32_t     row_pitch;     // 0 = packed; else bytes per row (padded)
    float             depth_scale;   // sample -> physical units (0 = unset)
    std::uint32_t     invalid_value; // no-data sentinel
    SampleUnits       sample_units;
    CaptureClock      capture_clock;
    TensorLayout      layout;
    std::uint64_t     capture_ts_ns; // sensor timestamp (see capture_clock)
    float             intr_fx, intr_fy, intr_cx, intr_cy;  // 0 = unset
    std::uint32_t     intr_ref_width, intr_ref_height;
};

class TensorSubscriber {
public:
    static std::unique_ptr<TensorSubscriber> create(std::string_view service_name);

    ~TensorSubscriber();

    // Connect to the sidecar and wait for the initial handshake. Call from a
    // non-RT context. Receives every dma-buf FD, registers them in the cache,
    // and performs accelerator import.
    int wait_handshake(int timeout_ms = 5000);

    // Call from the RT loop. Reads the most recent descriptor via seqlock.
    // Applies the fallback policy when max_retry is exceeded.
    //
    // **RT-safety**:
    //   - No dynamic allocation
    //   - No syscalls (assumes prior attach + mlockall + MAP_POPULATE)
    //   - Even with sync_fd, polling is non-blocking
    std::optional<TensorView> latest_view(int max_retry = 8) noexcept;

    void set_fallback_policy(FallbackPolicy p) noexcept;

    // Pool-generation mismatch counters — for monitoring.
    std::uint64_t pool_handshake_count() const noexcept;
    std::uint64_t fallback_invocation_count() const noexcept;

    TensorSubscriber(const TensorSubscriber&) = delete;
    TensorSubscriber& operator=(const TensorSubscriber&) = delete;

private:
    TensorSubscriber();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace axon
