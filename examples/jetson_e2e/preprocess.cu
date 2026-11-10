// SPDX-License-Identifier: Apache-2.0
// axon — YUYV (pitched) → ImageNet-normalised planar RGB for a [1,3,H,W] network input.

#include "preprocess.h"

namespace {

// One thread per output pixel: nearest source pixel, BT.601 full-range coefficients (the UVC
// camera reports limited range; this only shifts colours, not timing), (x/255 - mean)/std.
__global__ void yuyv_to_nchw_kernel(const uint8_t* src, uint32_t src_w, uint32_t src_h,
                                    uint32_t row_pitch, float* dst, int dst_w, int dst_h) {
    const int px = blockIdx.x * blockDim.x + threadIdx.x;
    const int py = blockIdx.y * blockDim.y + threadIdx.y;
    if (px >= dst_w || py >= dst_h) return;

    const uint32_t sx = src_w * static_cast<uint32_t>(px) / static_cast<uint32_t>(dst_w);
    const uint32_t sy = src_h * static_cast<uint32_t>(py) / static_cast<uint32_t>(dst_h);
    const uint8_t* pair = src + sy * row_pitch + (sx & ~1u) * 2;   // Y0 U Y1 V for pixels sx&~1, sx|1
    const float y = pair[(sx & 1u) ? 2 : 0];
    const float u = pair[1] - 128.0f;
    const float v = pair[3] - 128.0f;

    const float r = fminf(fmaxf(y + 1.402f * v, 0.0f), 255.0f);
    const float g = fminf(fmaxf(y - 0.344f * u - 0.714f * v, 0.0f), 255.0f);
    const float b = fminf(fmaxf(y + 1.772f * u, 0.0f), 255.0f);

    const int plane = dst_w * dst_h, i = py * dst_w + px;
    dst[i]             = (r / 255.0f - 0.485f) / 0.229f;
    dst[i + plane]     = (g / 255.0f - 0.456f) / 0.224f;
    dst[i + 2 * plane] = (b / 255.0f - 0.406f) / 0.225f;
}

}  // namespace

cudaError_t launch_yuyv_to_nchw(const uint8_t* src, uint32_t src_w, uint32_t src_h,
                                uint32_t row_pitch, float* dst, int dst_w, int dst_h,
                                cudaStream_t s) {
    const dim3 block(16, 16);
    const dim3 grid((dst_w + 15) / 16, (dst_h + 15) / 16);
    yuyv_to_nchw_kernel<<<grid, block, 0, s>>>(src, src_w, src_h, row_pitch, dst, dst_w, dst_h);
    return cudaGetLastError();
}
