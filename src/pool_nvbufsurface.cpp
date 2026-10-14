// SPDX-License-Identifier: Apache-2.0
// axon — NvBufSurface pool backend for PoolBackend::NvBufSurface (Jetson).
//
// Wraps the proven instrumentation/jetson/nvbuf_probe.cpp path: NvBufSurfaceAllocate
// with NVBUF_MEM_SURFACE_ARRAY + NVBUF_LAYOUT_PITCH, then extract bufferDesc, pitch,
// and dataSize from surfaceList[0]. Compiled only with -DAXON_ENABLE_JETSON.

#include "axon/detail/nvbuf.h"

#include <cerrno>
#include <cstring>

#include <linux/videodev2.h>
#include <unistd.h>

#include "nvbufsurface.h"

#if AXON_WITH_CUDA
#include <cuda.h>
#include <cudaEGL.h>
#include "axon/detail/accelerator.h"
#endif

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

std::size_t nvbuf_import_params_size() { return sizeof(NvBufSurfaceMapParams); }

bool nvbuf_get_import_params(const NvBuf& b, void* out) {
    if (!b.surf || !out) return false;
    return NvBufSurfaceGetMapParams(static_cast<const NvBufSurface*>(b.surf), 0,
                                    static_cast<NvBufSurfaceMapParams*>(out)) == 0;
}

bool nvbuf_import(int fd, const void* params, NvBuf* out) {
    if (fd < 0 || !params || !out) { errno = EINVAL; return false; }
    int own = ::dup(fd);
    if (own < 0) return false;
    NvBufSurfaceMapParams mp;
    std::memcpy(&mp, params, sizeof mp);
    mp.fd = static_cast<std::uint64_t>(own);   // the producer's FD number is meaningless here
    NvBufSurface* surf = nullptr;
    if (NvBufSurfaceImport(&surf, &mp) != 0) { ::close(own); errno = EPROTO; return false; }
    surf->numFilled = 1;   // Import leaves it 0, and SyncForCpu/Map then reject index 0

    const auto& s = surf->surfaceList[0];
    out->surf  = surf;
    out->fd    = static_cast<int>(s.bufferDesc);
    out->size  = s.dataSize;
    out->pitch = s.pitch;
    return true;
}

#if AXON_WITH_CUDA
bool nvbuf_cuda_map(NvBuf& b, NvBufCuda* out) {
    if (!b.surf || !out || !accel_init()) return false;
    auto* surf = static_cast<NvBufSurface*>(b.surf);
    if (NvBufSurfaceMapEglImage(surf, 0) != 0) return false;

    CUgraphicsResource res = nullptr;
    CUeglFrame f;
    if (cuGraphicsEGLRegisterImage(&res, surf->surfaceList[0].mappedAddr.eglImage,
                                   CU_GRAPHICS_MAP_RESOURCE_FLAGS_NONE) != CUDA_SUCCESS) {
        NvBufSurfaceUnMapEglImage(surf, 0);
        return false;
    }
    if (cuGraphicsResourceGetMappedEglFrame(&f, res, 0, 0) != CUDA_SUCCESS ||
        f.frameType != CU_EGL_FRAME_TYPE_PITCH) {
        cuGraphicsUnregisterResource(res);
        NvBufSurfaceUnMapEglImage(surf, 0);
        return false;
    }
    out->resource = res;
    out->device_ptr = f.frame.pPitch[0];
    return true;
}

void nvbuf_cuda_unmap(NvBuf& b, NvBufCuda* c) {
    if (!c || !c->resource) return;
    accel_init();   // make the context current on this thread before unregistering
    cuGraphicsUnregisterResource(static_cast<CUgraphicsResource>(c->resource));
    if (b.surf) NvBufSurfaceUnMapEglImage(static_cast<NvBufSurface*>(b.surf), 0);
    *c = NvBufCuda{};
}
#else
bool nvbuf_cuda_map(NvBuf&, NvBufCuda*) { return false; }
void nvbuf_cuda_unmap(NvBuf&, NvBufCuda*) {}
#endif

}  // namespace axon::detail
