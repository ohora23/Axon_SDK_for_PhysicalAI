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
    CHECK(sizeof(TensorDescriptor) == 144);   // wire v2 pinned size
    CHECK(alignof(TensorDescriptor) == 8);
    CHECK(kWireVersion == 2);                  // v2: imaging/depth metadata
}

DCZC_TEST(v2_depth_metadata_roundtrip) {
    // A padded U16 depth frame: row_pitch > packed row width.
    TensorDescriptor d {};
    d.rank = 2;
    d.shape[0] = 240;      // H
    d.shape[1] = 316;      // W  (packed row = 632 bytes)
    d.dtype = DType::U16;
    d.row_pitch = 640;     // padded to 640
    d.depth_scale = 0.001f;
    d.invalid_value = 0;
    d.sample_units = SampleUnits::Millimeters;
    d.capture_clock = CaptureClock::MonotonicRaw;
    d.layout = TensorLayout::HW;
    d.offset = 0;
    d.size = 640ull * 240;  // image_bytes with padding

    CHECK(detail::row_stride_bytes(d) == 640);
    CHECK(detail::image_bytes(d) == 640ull * 240);
    CHECK(detail::implied_bytes(d) == 240ull * 316 * 2);  // packed, ignores pitch
    CHECK(detail::descriptor_is_valid(d, 640ull * 240));

    // pitch smaller than a packed row is invalid.
    d.row_pitch = 600;
    CHECK(!detail::descriptor_is_valid(d, 640ull * 240));

    // size that doesn't cover the padded footprint is invalid.
    d.row_pitch = 640;
    d.size = 632ull * 240;   // < image_bytes(640*240)
    CHECK(!detail::descriptor_is_valid(d, 640ull * 240));
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
