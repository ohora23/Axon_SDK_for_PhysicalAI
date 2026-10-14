// SPDX-License-Identifier: Apache-2.0
// axon — Jetson NvBufSurface buffer allocation/destruction for PoolBackend::NvBufSurface.
//
// Wraps the proven instrumentation/jetson/nvbuf_probe.cpp path:
//   NvBufSurfaceAllocate → extract bufferDesc / pitch / dataSize → NvBufSurfaceDestroy.

#pragma once

#include <cstddef>
#include <cstdint>

namespace axon::detail {

struct NvBuf {
    void*         surf  = nullptr;  // NvBufSurface*, owns the FD
    int           fd    = -1;       // dma-buf FD (surfaceList[0].bufferDesc)
    std::size_t   size  = 0;        // surfaceList[0].dataSize
    std::uint32_t pitch = 0;        // surfaceList[0].pitch
};

// Allocate one pitch-linear SURFACE_ARRAY buffer. false (errno=EINVAL) for an
// unsupported fourcc, false on allocation failure.
bool nvbuf_alloc(std::uint32_t width, std::uint32_t height, std::uint32_t fourcc, NvBuf* out);

// Destroy the surface (this also closes its FD — never close fd separately). Safe on zeroed.
void nvbuf_free(NvBuf* b);

// ---- Cross-process import (NvBufSurfaceFromFd only knows in-process FDs) ----

// Size of the opaque per-buffer import blob (sizeof(NvBufSurfaceMapParams)).
std::size_t nvbuf_import_params_size();

// Producer: write `b`'s import blob (nvbuf_import_params_size() bytes) to `out`.
bool nvbuf_get_import_params(const NvBuf& b, void* out);

// Consumer: import a received FD using the producer's blob. The FD is dup()'d
// first because destroying an imported surface closes its FD — the caller keeps
// ownership of `fd`; release the import with nvbuf_free().
bool nvbuf_import(int fd, const void* params, NvBuf* out);

// ---- CUDA view (AXON_WITH_CUDA): EGLImage → cuGraphicsEGLRegisterImage ----

struct NvBufCuda {
    void* resource   = nullptr;  // CUgraphicsResource
    void* device_ptr = nullptr;  // pitch-linear plane 0, valid in THIS process
    bool  failed     = false;    // a map attempt failed — don't retry every frame
};

// Map `b` for CUDA (creates the primary context on first use). false if CUDA or
// EGL interop is unavailable — callers treat the device view as optional.
bool nvbuf_cuda_map(NvBuf& b, NvBufCuda* out);

// Unregister + unmap the EGLImage. Safe on a zeroed NvBufCuda.
void nvbuf_cuda_unmap(NvBuf& b, NvBufCuda* c);

}  // namespace axon::detail
