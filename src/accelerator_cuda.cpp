// SPDX-License-Identifier: Apache-2.0
// axon — CUDA VMM device-buffer backend (R6). Driver API only (cuMem*), so it
// builds with g++ + -lcuda (no nvcc). Compiled only when AXON_WITH_CUDA is set;
// mirrors the vmm_alloc/export path validated in instrumentation/gpu.

#include "axon/detail/accelerator.h"

#include <cstdint>
#include <cuda.h>
#include <unistd.h>

namespace axon::detail {

namespace {

bool g_init = false;

inline bool ck(CUresult r) { return r == CUDA_SUCCESS; }

CUmemAllocationProp device_prop() {
    CUmemAllocationProp p = {};
    p.type = CU_MEM_ALLOCATION_TYPE_PINNED;
    p.location.type = CU_MEM_LOCATION_TYPE_DEVICE;
    p.location.id = 0;
    p.requestedHandleTypes = CU_MEM_HANDLE_TYPE_POSIX_FILE_DESCRIPTOR;
    return p;
}

// Round a byte size up to the device allocation granularity. 0 on failure.
// Deterministic on a given device, so a producer and importer that pass the
// same requested size derive the same mapping size.
std::size_t aligned_size(std::size_t size) {
    CUmemAllocationProp prop = device_prop();
    std::size_t gran = 0;
    if (!ck(cuMemGetAllocationGranularity(&gran, &prop, CU_MEM_ALLOC_GRANULARITY_MINIMUM)) ||
        gran == 0)
        return 0;
    return ((size + gran - 1) / gran) * gran;
}

// Reserve an address range, map `h` into it, and grant device RW access. On any
// failure, undoes its own steps and returns false; on success *out_ptr is the
// mapped device pointer. The caller owns `h` and releases it on failure.
bool map_rw(CUmemGenericAllocationHandle h, std::size_t sz, CUdeviceptr* out_ptr) {
    CUdeviceptr ptr = 0;
    if (!ck(cuMemAddressReserve(&ptr, sz, 0, 0, 0)) || !ck(cuMemMap(ptr, sz, 0, h, 0))) {
        if (ptr) cuMemAddressFree(ptr, sz);
        return false;
    }
    CUmemAccessDesc acc = {};
    acc.location.type = CU_MEM_LOCATION_TYPE_DEVICE;
    acc.location.id = 0;
    acc.flags = CU_MEM_ACCESS_FLAGS_PROT_READWRITE;
    if (!ck(cuMemSetAccess(ptr, sz, &acc, 1))) {
        cuMemUnmap(ptr, sz);
        cuMemAddressFree(ptr, sz);
        return false;
    }
    *out_ptr = ptr;
    return true;
}

}  // namespace

bool accel_init() {
    if (g_init) return true;
    CUdevice dev;
    CUcontext ctx;
    if (!ck(cuInit(0)) || !ck(cuDeviceGet(&dev, 0)) ||
        !ck(cuDevicePrimaryCtxRetain(&ctx, dev)) || !ck(cuCtxSetCurrent(ctx)))
        return false;
    g_init = true;
    return true;
}

bool accel_alloc(std::size_t size, AccelBuffer* out) {
    if (!out || size == 0 || !accel_init()) return false;

    std::size_t sz = aligned_size(size);
    if (sz == 0) return false;

    CUmemAllocationProp prop = device_prop();
    CUmemGenericAllocationHandle h = 0;
    if (!ck(cuMemCreate(&h, sz, &prop, 0))) return false;

    CUdeviceptr ptr = 0;
    if (!map_rw(h, sz, &ptr)) {
        cuMemRelease(h);
        return false;
    }

    int fd = -1;
    if (!ck(cuMemExportToShareableHandle(&fd, h, CU_MEM_HANDLE_TYPE_POSIX_FILE_DESCRIPTOR, 0))) {
        cuMemUnmap(ptr, sz);
        cuMemAddressFree(ptr, sz);
        cuMemRelease(h);
        return false;
    }

    out->device_ptr = reinterpret_cast<void*>(static_cast<std::uintptr_t>(ptr));
    out->export_fd = fd;
    out->aligned = sz;
    out->handle = static_cast<unsigned long long>(h);
    return true;
}

bool accel_import(int export_fd, std::size_t size, AccelBuffer* out) {
    if (!out || export_fd < 0 || size == 0 || !accel_init()) return false;

    std::size_t sz = aligned_size(size);
    if (sz == 0) return false;

    CUmemGenericAllocationHandle h = 0;
    if (!ck(cuMemImportFromShareableHandle(
            &h, reinterpret_cast<void*>(static_cast<std::intptr_t>(export_fd)),
            CU_MEM_HANDLE_TYPE_POSIX_FILE_DESCRIPTOR)))
        return false;

    CUdeviceptr ptr = 0;
    if (!map_rw(h, sz, &ptr)) {
        cuMemRelease(h);
        return false;
    }

    out->device_ptr = reinterpret_cast<void*>(static_cast<std::uintptr_t>(ptr));
    out->export_fd = -1;   // caller owns the received FD; accel_free won't close it
    out->aligned = sz;
    out->handle = static_cast<unsigned long long>(h);
    return true;
}

void accel_free(AccelBuffer* b) {
    if (!b || !b->device_ptr) return;
    CUdeviceptr ptr = static_cast<CUdeviceptr>(reinterpret_cast<std::uintptr_t>(b->device_ptr));
    cuMemUnmap(ptr, b->aligned);
    cuMemAddressFree(ptr, b->aligned);
    cuMemRelease(static_cast<CUmemGenericAllocationHandle>(b->handle));
    if (b->export_fd >= 0) ::close(b->export_fd);
    *b = AccelBuffer{};
}

}  // namespace axon::detail
