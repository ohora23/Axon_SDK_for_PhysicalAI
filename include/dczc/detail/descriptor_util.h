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

// Bytes implied by shape × dtype.
inline std::uint64_t implied_bytes(const TensorDescriptor& d) noexcept {
    return element_count(d) * dtype_size(d.dtype);
}

// Structural validity: rank within bounds, dtype known, and [offset, offset+size)
// fits inside a buffer of `buffer_size`. Does not touch any FD.
inline bool descriptor_is_valid(const TensorDescriptor& d,
                                std::uint64_t buffer_size) noexcept {
    if (d.rank == 0 || d.rank > kMaxRank) return false;
    if (dtype_size(d.dtype) == 0) return false;
    if (d.size == 0) return false;
    if (d.offset > buffer_size) return false;
    if (d.size > buffer_size - d.offset) return false;
    return true;
}

}  // namespace dczc::detail
