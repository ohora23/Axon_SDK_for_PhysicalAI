// SPDX-License-Identifier: Apache-2.0
// axon — TensorPublisher: publishes metadata via Iceoryx2 + delivers FDs via the sidecar.

#pragma once

#include <memory>
#include <string_view>
#include "axon/tensor_descriptor.h"
#include "axon/pool.h"

namespace axon {

// View on a descriptor that the producer fills and then publishes.
// The pool slot is locked for as long as the AcquiredDescriptor is alive.
struct AcquiredDescriptor {
    TensorDescriptor* desc;
    int               buffer_index;     // Pool ring index
    void*             host_view;        // mmap'd host view (meaningful on UMA)
    AcceleratorHandle accel_handle;     // Cached accelerator import handle
};

class TensorPublisher {
public:
    // service_name is the Iceoryx2 service identifier; consumers attach using
    // the same name. Returns nullptr (errno set) on failure; errno = E2BIG when
    // the pool's backend import parameters exceed what one sidecar handshake can
    // carry (65535 bytes, i.e. ~126 NvBufSurface buffers), which would otherwise
    // leave consumers without a device view.
    static std::unique_ptr<TensorPublisher> create(
        std::string_view service_name,
        TensorPool& pool);

    ~TensorPublisher();

    // Bulk-deliver every dma-buf FD via SCM_RIGHTS over the sidecar
    // (Unix domain socket). Must be called before any consumer attaches.
    // Returns a negative value with errno set on failure.
    int handshake_pool();

    // Acquire the next available descriptor. Acquires a free slot from the
    // pool and initializes its fields. The caller fills the inference output
    // into the view, then calls publish().
    AcquiredDescriptor acquire_descriptor();

    // Publish metadata — writes into the Iceoryx2 SHM queue and stamps
    // producer_publish_ts_ns. When sync_fence_kind == SyncFileViaSidecar and
    // sync_fd is valid, the fence FD is delivered through the sidecar.
    //
    // Returns 0, or -1 when the descriptor is rejected and nothing is published:
    // errno = EMSGSIZE if [offset, offset + size) does not fit in the pool buffer,
    // EINVAL for another malformed field (rank 0 or > kMaxRank, unknown dtype,
    // size 0, row_pitch smaller than a packed row or not covered by size). A
    // rejected frame takes no seqno, so consumers see no gap; rejected_count()
    // counts them.
    int publish(AcquiredDescriptor&& d, int sync_fd = -1);

    // Frames publish() rejected so far — for monitoring.
    std::uint64_t rejected_count() const noexcept;

    // Re-issue the bulk handshake to every subscriber after a pool
    // reallocation. pool_generation increments automatically.
    int reannounce_pool();

    TensorPublisher(const TensorPublisher&) = delete;
    TensorPublisher& operator=(const TensorPublisher&) = delete;

private:
    TensorPublisher();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace axon
