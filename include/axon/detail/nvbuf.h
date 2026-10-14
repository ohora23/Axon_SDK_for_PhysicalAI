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

}  // namespace axon::detail
