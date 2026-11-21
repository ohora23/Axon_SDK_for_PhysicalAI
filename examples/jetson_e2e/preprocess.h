// SPDX-License-Identifier: Apache-2.0
// axon — YUYV / NV12 → ImageNet-normalised NCHW float, one CUDA kernel each.

#pragma once

#include <cstdint>

#include <cuda_runtime.h>

// src: device pointer to a YUYV image, row_pitch bytes per row. dst: [3, dst_h, dst_w] floats.
cudaError_t launch_yuyv_to_nchw(const uint8_t* src, uint32_t src_w, uint32_t src_h,
                                uint32_t row_pitch, float* dst, int dst_w, int dst_h,
                                cudaStream_t s);

// src: device pointer to an NV12 image. Y plane = src_h rows of row_pitch bytes.
// uv_offset = byte offset of the interleaved UV plane after the Y plane.
// dst: [3, dst_h, dst_w] floats.
cudaError_t launch_nv12_to_nchw(const uint8_t* src, uint32_t src_w, uint32_t src_h,
                                uint32_t row_pitch, uint32_t uv_offset,
                                float* dst, int dst_w, int dst_h,
                                cudaStream_t s);
