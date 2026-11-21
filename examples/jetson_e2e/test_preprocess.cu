// SPDX-License-Identifier: Apache-2.0
// Checks the pre-processing kernels against a CPU reference on random pitched images.
// Build and run on the Jetson (no camera needed):
//   nvcc -std=c++17 -arch=sm_72 test_preprocess.cu preprocess.cu -o test_preprocess && ./test_preprocess

#include "preprocess.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

namespace {

void rgb_to_out(float y, float u, float v, float out[3]) {
    const float r = std::fmin(std::fmax(y + 1.402f * v, 0.0f), 255.0f);
    const float g = std::fmin(std::fmax(y - 0.344f * u - 0.714f * v, 0.0f), 255.0f);
    const float b = std::fmin(std::fmax(y + 1.772f * u, 0.0f), 255.0f);
    out[0] = (r / 255.0f - 0.485f) / 0.229f;
    out[1] = (g / 255.0f - 0.456f) / 0.224f;
    out[2] = (b / 255.0f - 0.406f) / 0.225f;
}

// Nearest source pixel: sx = w*px/dw, sy = h*py/dh (integer division), as in the kernels.
std::vector<float> ref_yuyv(const std::vector<uint8_t>& img, uint32_t w, uint32_t h, uint32_t pitch, int dw, int dh) {
    std::vector<float> out(3u * dw * dh);
    for (int py = 0; py < dh; ++py)
        for (int px = 0; px < dw; ++px) {
            const uint32_t sx = w * px / dw, sy = h * py / dh;
            const uint8_t* pair = &img[sy * pitch + (sx & ~1u) * 2];
            float o[3];
            rgb_to_out(pair[(sx & 1u) ? 2 : 0], pair[1] - 128.0f, pair[3] - 128.0f, o);
            for (int c = 0; c < 3; ++c) out[c * dw * dh + py * dw + px] = o[c];
        }
    return out;
}

// NV12: Y plane h rows of `pitch` bytes; interleaved UV plane (U first) at uv_offset, h/2 rows,
// one U,V pair per 2x2 block of pixels.
std::vector<float> ref_nv12(const std::vector<uint8_t>& img, uint32_t w, uint32_t h, uint32_t pitch,
                            uint32_t uv_offset, int dw, int dh) {
    std::vector<float> out(3u * dw * dh);
    for (int py = 0; py < dh; ++py)
        for (int px = 0; px < dw; ++px) {
            const uint32_t sx = w * px / dw, sy = h * py / dh;
            const uint8_t* uv = &img[uv_offset + (sy / 2) * pitch + (sx & ~1u)];
            float o[3];
            rgb_to_out(img[sy * pitch + sx], uv[0] - 128.0f, uv[1] - 128.0f, o);
            for (int c = 0; c < 3; ++c) out[c * dw * dh + py * dw + px] = o[c];
        }
    return out;
}

int fails = 0;

void compare(const char* name, const std::vector<float>& want, const float* d_out) {
    std::vector<float> got(want.size());
    cudaMemcpy(got.data(), d_out, got.size() * sizeof(float), cudaMemcpyDeviceToHost);
    double worst = 0;
    size_t at = 0;
    for (size_t i = 0; i < want.size(); ++i)
        if (std::fabs(got[i] - want[i]) > worst) { worst = std::fabs(got[i] - want[i]); at = i; }
    const bool ok = worst < 1e-3;
    printf("%-40s max|diff| %.2e at %zu  %s\n", name, worst, at, ok ? "OK" : "FAIL");
    fails += !ok;
}

}  // namespace

int main() {
    std::mt19937 rng(7);
    std::uniform_int_distribution<int> byte(0, 255);
    struct Case { uint32_t w, h, pitch; int dw, dh; };
    const Case cases[] = {{640, 480, 1280, 224, 224}, {1280, 800, 1280, 224, 224},
                          {1280, 800, 1536, 224, 224}, {642, 362, 704, 101, 77},
                          {641, 361, 704, 101, 77}};   // odd sizes: must work, not be rejected
    for (const auto& c : cases) {
        float* d_out;
        cudaMalloc(&d_out, 3u * c.dw * c.dh * sizeof(float));
        char name[96];

        // YUYV: rows of 2*w bytes inside a pitch of at least 2*w.
        {
            const uint32_t pitch = c.pitch >= 2 * c.w ? c.pitch : 2 * c.w + 64;
            std::vector<uint8_t> img(pitch * c.h);
            for (auto& b : img) b = static_cast<uint8_t>(byte(rng));
            uint8_t* d_img;
            cudaMalloc(&d_img, img.size());
            cudaMemcpy(d_img, img.data(), img.size(), cudaMemcpyHostToDevice);
            cudaMemset(d_out, 0, 3u * c.dw * c.dh * sizeof(float));
            const cudaError_t e = launch_yuyv_to_nchw(d_img, c.w, c.h, pitch, d_out, c.dw, c.dh, 0);
            cudaDeviceSynchronize();
            snprintf(name, sizeof name, "yuyv %ux%u pitch %u -> %dx%d", c.w, c.h, pitch, c.dw, c.dh);
            if (e != cudaSuccess) { printf("%s launch error %s\n", name, cudaGetErrorString(e)); ++fails; }
            else compare(name, ref_yuyv(img, c.w, c.h, pitch, c.dw, c.dh), d_out);
            cudaFree(d_img);
        }
        // NV12: UV plane placed after the Y plane plus a gap, to catch a hard-coded h*pitch offset.
        {
            const uint32_t uv_offset = c.pitch * c.h + 4096;
            std::vector<uint8_t> img(uv_offset + c.pitch * (c.h / 2));
            for (auto& b : img) b = static_cast<uint8_t>(byte(rng));
            uint8_t* d_img;
            cudaMalloc(&d_img, img.size());
            cudaMemcpy(d_img, img.data(), img.size(), cudaMemcpyHostToDevice);
            cudaMemset(d_out, 0, 3u * c.dw * c.dh * sizeof(float));
            const cudaError_t e = launch_nv12_to_nchw(d_img, c.w, c.h, c.pitch, uv_offset, d_out, c.dw, c.dh, 0);
            cudaDeviceSynchronize();
            snprintf(name, sizeof name, "nv12 %ux%u pitch %u -> %dx%d", c.w, c.h, c.pitch, c.dw, c.dh);
            if (e != cudaSuccess) { printf("%s launch error %s\n", name, cudaGetErrorString(e)); ++fails; }
            else compare(name, ref_nv12(img, c.w, c.h, c.pitch, uv_offset, c.dw, c.dh), d_out);
            cudaFree(d_img);
        }
        cudaFree(d_out);
    }
    printf(fails ? "FAILED: %d\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
