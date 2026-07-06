// SPDX-License-Identifier: Apache-2.0
// axon — TensorPool: dma-buf-backed buffer pool
//
// The producer pre-allocates N buffers at startup; capture, inference, and
// publish all acquire/release from the same pool. On retire, pool_generation
// increments and the sidecar handshake is re-issued.

#pragma once

#include <cstddef>
#include <memory>
#include <vector>
#include "axon/types.h"

namespace axon {

struct TensorPoolConfig {
    std::size_t n_buffers;     // Ring size (worst_case_inference_ms × rate × safety)
    std::size_t buffer_size;   // Bytes per buffer
    PoolBackend backend;
    const char* v4l2_device;   // Used when backend == V4L2 (e.g. "/dev/video0")
};

// dma-buf-backed buffer pool.
// V4L2 backend: VIDIOC_REQBUFS + VIDIOC_EXPBUF to extract FDs.
// UDMABUF backend: expose user-space memory as dma-buf via udmabuf.
class TensorPool {
public:
    static std::unique_ptr<TensorPool> create(const TensorPoolConfig& cfg);
    ~TensorPool();

    // All dma-buf FDs in the pool, used for the bulk sidecar handshake.
    // Returned FDs are owned by the pool — do not close them.
    const std::vector<int>& dma_buf_fds() const noexcept;

    PoolGeneration generation() const noexcept;

    // Page-aligned bytes per buffer (authoritative — the publisher uses this
    // instead of lseek(SEEK_END), which some real dma-buf exporters reject).
    std::size_t buffer_size() const noexcept;

    // Device pointer for an Accelerator-backed buffer (CUDA VMM). Returns
    // nullptr for host-mapped backends (Custom/UDMABUF/V4L2) or an out-of-range
    // index. Valid only in the process that owns the pool — consumers import
    // the corresponding exported FD from dma_buf_fds() instead.
    void* device_ptr(std::size_t index) const noexcept;

    // Acquire the next free buffer. Advances the ring by one slot.
    // The returned index maps into dma_buf_fds()[index].
    int acquire_next();

    // Release a buffer back to the pool after use.
    void release(int index);

    // Retire the pool — every consumer must detach before the producer closes
    // the FDs. generation++.
    void retire_and_reallocate(std::size_t new_buffer_size);

    // Non-copyable
    TensorPool(const TensorPool&) = delete;
    TensorPool& operator=(const TensorPool&) = delete;

private:
    TensorPool();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace axon
