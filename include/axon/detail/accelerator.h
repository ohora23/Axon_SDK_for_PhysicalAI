// SPDX-License-Identifier: Apache-2.0
// axon — CUDA VMM device-buffer backend for PoolBackend::Accelerator (R6).
//
// Promotes the proven instrumentation/gpu VMM alloc/export path into the
// library. Implemented in src/accelerator_cuda.cpp, which is compiled only
// with -DAXON_WITH_CUDA (CUDA driver API, links libcuda; no nvcc required).

#pragma once

#include <cstddef>

namespace axon::detail {

// One device buffer allocated via the CUDA virtual-memory-management API and
// exported as a POSIX file descriptor (SCM_RIGHTS-transferable through the
// sidecar, imported by a consumer with cuMemImportFromShareableHandle).
struct AccelBuffer {
    void*              device_ptr = nullptr;  // CUdeviceptr, valid in THIS process
    int                export_fd  = -1;       // POSIX shareable handle (dma_buf_fds())
    std::size_t        aligned    = 0;         // granularity-aligned allocation size
    unsigned long long handle     = 0;         // CUmemGenericAllocationHandle (for free)
};

// One-time driver init + primary context on device 0. false if CUDA absent.
bool accel_init();

// Allocate one device buffer >= size, mapped + exported. false on any failure.
bool accel_alloc(std::size_t size, AccelBuffer* out);

// Import a device buffer from a producer's POSIX shareable handle (the export FD
// received over the sidecar), mapping it into this process. out->export_fd is
// left at -1: the caller owns the received FD's lifetime, so accel_free() will
// not close it (avoids a double-close). false on any failure.
bool accel_import(int export_fd, std::size_t size, AccelBuffer* out);

// Unmap + free the buffer and close its export FD. Safe on a zeroed buffer.
void accel_free(AccelBuffer* b);

}  // namespace axon::detail
