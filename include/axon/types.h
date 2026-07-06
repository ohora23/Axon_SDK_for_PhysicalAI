// SPDX-License-Identifier: Apache-2.0
// axon — Data-centric zero-copy for Physical AI
//
// Common types and enums. Every other header includes this one.

#pragma once

#include <cstdint>

namespace axon {

// ---------- Data types ----------

enum class DType : std::uint8_t {
    U8   = 0,
    U16  = 1,
    I16  = 2,
    F16  = 3,
    BF16 = 4,
    F32  = 5,
    F64  = 6,
    I32  = 7,
    I64  = 8,
};

constexpr std::size_t dtype_size(DType t) noexcept {
    switch (t) {
        case DType::U8:                                              return 1;
        case DType::U16: case DType::I16: case DType::F16: case DType::BF16: return 2;
        case DType::F32: case DType::I32:                            return 4;
        case DType::F64: case DType::I64:                            return 8;
    }
    return 0;
}

// ---------- Synchronization mode ----------

enum class SyncFenceKind : std::uint8_t {
    None               = 0,  // Caller's responsibility
    DmaResvImplicit    = 1,  // Kernel orders work via dma_resv automatically
    SyncFileViaSidecar = 2,  // sync_file FD passed through the sidecar; consumer polls it
};

// ---------- v2: imaging / depth semantics ----------

// Physical meaning of a raw sample. Combined with TensorDescriptor::depth_scale
// (e.g. Millimeters means value * 1 mm, or use depth_scale for finer units).
enum class SampleUnits : std::uint8_t {
    Unknown     = 0,
    Raw         = 1,  // dimensionless / application-defined
    Millimeters = 2,  // depth in mm (or value * depth_scale mm)
    Meters      = 3,  // metric depth (value * depth_scale m, or float metres)
    Disparity   = 4,  // stereo disparity
};

// Clock domain of TensorDescriptor::capture_ts_ns, so consumers know whether
// timestamps from different sensors are comparable.
enum class CaptureClock : std::uint8_t {
    Unknown      = 0,
    MonotonicRaw = 1,  // CLOCK_MONOTONIC_RAW
    V4L2Hardware = 2,  // V4L2 hardware timestamp
};

// Memory layout of the tensor in the buffer.
enum class TensorLayout : std::uint8_t {
    Unknown = 0,
    HW      = 1,  // 2D image (rows x cols), possibly row_pitch-padded
    HWC     = 2,  // interleaved channels
    CHW     = 3,  // planar channels
};

// ---------- Pool backend ----------

enum class PoolBackend : std::uint8_t {
    V4L2        = 0,  // Export V4L2 capture buffers as dma-bufs (VIDIOC_EXPBUF)
    UDMABUF     = 1,  // Expose user-space buffers as dma-bufs via udmabuf
    Custom      = 2,  // User-defined (caller injects external dma-buf FDs)
    Accelerator = 3,  // CUDA VMM device buffers, POSIX-FD shareable (AXON_WITH_CUDA)
};

// ---------- Fallback policy ----------

enum class FallbackPolicy : std::uint8_t {
    LastKnownGood = 0,  // Reuse last valid view (visual servo, locomotion)
    ZeroCommand   = 1,  // Emit a safe-stop command
    UserCallback  = 2,  // Call a user-provided policy
    AbortLoop     = 3,  // Abort the RT loop and notify upstream (test/debug)
};

// ---------- Shape / rank ----------

constexpr std::uint8_t kMaxRank = 8;

struct Shape {
    std::uint32_t dims[kMaxRank];
    std::uint8_t  rank;

    constexpr std::uint64_t element_count() const noexcept {
        std::uint64_t n = 1;
        for (std::uint8_t i = 0; i < rank; ++i) n *= dims[i];
        return n;
    }
};

// ---------- Pool / handle types ----------

using BoHandle       = std::uint64_t;  // Producer-allocated id
using PoolGeneration = std::uint64_t;
using SeqNo          = std::uint64_t;

// ---------- Accelerator import handle (backend-specific opaque) ----------

struct AcceleratorHandle {
    void*       opaque;       // Backend-specific handle (cuMemImport ptr, XDNA BO, ...)
    PoolBackend backend;
    std::size_t size_bytes;
};

}  // namespace axon
