// SPDX-License-Identifier: Apache-2.0
// Wire-format ABI + descriptor validation.

#include "dczc/tensor_descriptor.h"
#include "dczc/types.h"
#include "dczc/detail/descriptor_util.h"
#include "dczc_test.h"

using namespace dczc;

DCZC_TEST(abi_layout) {
    // Fixed-size POD invariants the wire format depends on.
    CHECK(sizeof(TensorDescriptor) % 8 == 0);
    CHECK(sizeof(TensorDescriptor) <= 256);
    CHECK(alignof(TensorDescriptor) == 8);
    CHECK(kWireVersion == 1);
}

DCZC_TEST(dtype_sizes) {
    CHECK(dtype_size(DType::U8) == 1);
    CHECK(dtype_size(DType::F16) == 2);
    CHECK(dtype_size(DType::BF16) == 2);
    CHECK(dtype_size(DType::F32) == 4);
    CHECK(dtype_size(DType::I32) == 4);
    CHECK(dtype_size(DType::F64) == 8);
    CHECK(dtype_size(DType::I64) == 8);
}

DCZC_TEST(element_and_byte_counts) {
    TensorDescriptor d {};
    d.rank = 3;
    d.shape[0] = 1; d.shape[1] = 3; d.shape[2] = 224;
    d.dtype = DType::F32;
    CHECK(detail::element_count(d) == 1u * 3u * 224u);
    CHECK(detail::implied_bytes(d) == 1u * 3u * 224u * 4u);
}

DCZC_TEST(validity_bounds) {
    TensorDescriptor d {};
    d.rank = 1;
    d.shape[0] = 16;
    d.dtype = DType::U8;
    d.offset = 0;
    d.size = 16;
    CHECK(detail::descriptor_is_valid(d, 4096));

    // size overruns the buffer
    d.size = 5000;
    CHECK(!detail::descriptor_is_valid(d, 4096));

    // offset past the end
    d.size = 16;
    d.offset = 5000;
    CHECK(!detail::descriptor_is_valid(d, 4096));

    // rank out of range
    d.offset = 0;
    d.rank = 0;
    CHECK(!detail::descriptor_is_valid(d, 4096));
    d.rank = kMaxRank + 1;
    CHECK(!detail::descriptor_is_valid(d, 4096));
}

DCZC_TEST_MAIN("descriptor")
