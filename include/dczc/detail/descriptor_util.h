// SPDX-License-Identifier: Apache-2.0
// dczc — TensorDescriptor helpers (internal)
//
// Validation and convenience routines kept out of the public POD header so the
// wire struct stays a dependency-free POD.

#pragma once

#include "dczc/tensor_descriptor.h"
#include "dczc/types.h"

namespace dczc::detail {

// Zero a descriptor to a well-defined empty state.
inline void clear_descriptor(TensorDescriptor* d) noexcept {
    *d = TensorDescriptor{};
}

// Number of elements implied by rank/shape.
inline std::uint64_t element_count(const TensorDescriptor& d) noexcept {
    std::uint64_t n = 1;
    for (std::uint8_t i = 0; i < d.rank && i < kMaxRank; ++i) n *= d.shape[i];
    return n;
}

// Bytes implied by shape × dtype (tightly packed, ignores row_pitch).
inline std::uint64_t implied_bytes(const TensorDescriptor& d) noexcept {
    return element_count(d) * dtype_size(d.dtype);
}

// Bytes of one row: row_pitch when set (padded), else width × dtype_size.
inline std::uint64_t row_stride_bytes(const TensorDescriptor& d) noexcept {
    if (d.row_pitch != 0) return d.row_pitch;
    if (d.rank >= 1) return static_cast<std::uint64_t>(d.shape[d.rank - 1]) *
                            dtype_size(d.dtype);
    return 0;
}

// Physical footprint of an image accounting for row padding: rows × row_stride,
// where rows = product of all dims except the last (width).
inline std::uint64_t image_bytes(const TensorDescriptor& d) noexcept {
    if (d.rank == 0) return 0;
    std::uint64_t rows = 1;
    for (std::uint8_t i = 0; i + 1 < d.rank && i < kMaxRank; ++i) rows *= d.shape[i];
    return rows * row_stride_bytes(d);
}

// Structural validity: rank/dtype sane, [offset, offset+size) fits the buffer,
// row_pitch (if set) is at least a packed row, and size covers the pitched
// footprint. Does not touch any FD.
inline bool descriptor_is_valid(const TensorDescriptor& d,
                                std::uint64_t buffer_size) noexcept {
    if (d.rank == 0 || d.rank > kMaxRank) return false;
    if (dtype_size(d.dtype) == 0) return false;
    if (d.size == 0) return false;
    if (d.offset > buffer_size) return false;
    if (d.size > buffer_size - d.offset) return false;
    if (d.row_pitch != 0 && d.rank >= 1) {
        std::uint64_t packed_row =
            static_cast<std::uint64_t>(d.shape[d.rank - 1]) * dtype_size(d.dtype);
        if (d.row_pitch < packed_row) return false;      // pitch can't be < packed
        if (image_bytes(d) > d.size) return false;        // size must cover padding
    }
    return true;
}

}  // namespace dczc::detail
