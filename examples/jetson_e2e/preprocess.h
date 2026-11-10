// SPDX-License-Identifier: Apache-2.0
// axon — YUYV → ImageNet-normalised NCHW float, one CUDA kernel.

#pragma once

#include <cstdint>

#include <cuda_runtime.h>

// src: device pointer to a YUYV image, row_pitch bytes per row. dst: [3, dst_h, dst_w] floats.
cudaError_t launch_yuyv_to_nchw(const uint8_t* src, uint32_t src_w, uint32_t src_h,
                                uint32_t row_pitch, float* dst, int dst_w, int dst_h,
                                cudaStream_t s);
