// SPDX-License-Identifier: Apache-2.0
// axon — NvBufSurface pool backend for PoolBackend::NvBufSurface (Jetson).
//
// Wraps the proven instrumentation/jetson/nvbuf_probe.cpp path: NvBufSurfaceAllocate
// with NVBUF_MEM_SURFACE_ARRAY + NVBUF_LAYOUT_PITCH, then extract bufferDesc, pitch,
// and dataSize from surfaceList[0]. Compiled only with -DAXON_ENABLE_JETSON.

#include "axon/detail/nvbuf.h"

#include <cerrno>

#include <linux/videodev2.h>

#include "nvbufsurface.h"

namespace axon::detail {

bool nvbuf_alloc(std::uint32_t width, std::uint32_t height, std::uint32_t fourcc, NvBuf* out) {
    if (!out || width == 0 || height == 0) { errno = EINVAL; return false; }

    NvBufSurfaceColorFormat color;
    switch (fourcc) {
        case V4L2_PIX_FMT_YUYV:   color = NVBUF_COLOR_FORMAT_YUYV;  break;
        case V4L2_PIX_FMT_GREY:   color = NVBUF_COLOR_FORMAT_GRAY8; break;
        case V4L2_PIX_FMT_RGBA32: color = NVBUF_COLOR_FORMAT_RGBA;  break;
        default: errno = EINVAL; return false;   // multi-planar (NV12, ...) not yet
    }

    NvBufSurfaceAllocateParams ap{};
    ap.params.width       = width;
    ap.params.height      = height;
    ap.params.colorFormat = color;
    ap.params.layout      = NVBUF_LAYOUT_PITCH;
    ap.params.memType     = NVBUF_MEM_SURFACE_ARRAY;
    ap.memtag             = NvBufSurfaceTag_CAMERA;

    NvBufSurface* surf = nullptr;
    if (NvBufSurfaceAllocate(&surf, 1, &ap) != 0) { errno = ENOMEM; return false; }
    surf->numFilled = 1;

    const auto& s = surf->surfaceList[0];
    out->surf  = surf;
    out->fd    = static_cast<int>(s.bufferDesc);
    out->size  = s.dataSize;
    out->pitch = s.pitch;
    return true;
}

void nvbuf_free(NvBuf* b) {
    if (!b || !b->surf) return;
    NvBufSurfaceDestroy(static_cast<NvBufSurface*>(b->surf));
    *b = NvBuf{};
}

}  // namespace axon::detail
