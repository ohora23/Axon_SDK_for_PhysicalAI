// SPDX-License-Identifier: Apache-2.0
// gpu_read_kernel.h — the "consumer kernel" variant of the GPU cells (BENCH_CONSUMER_KERNEL=1).
// Instead of copying the 16 B header and 8 B tail back with two DtoH copies, the consumer
// launches one tiny kernel on its stream (the legacy default stream for axon, ROS 2 loaned and
// intra-process; the event-waiting stream for cudaipc and rosidl_buffer) that reads them from the frame on the GPU and
// writes them to mapped host memory; the clock stops after the stream sync. That is what a
// consumer that runs GPU work on the frame pays, including any switch between the producer's
// and the consumer's GPU contexts. Driver API + inline PTX, so no nvcc is needed; usable from
// runtime-API programs (cudaSetDevice makes the primary context current first).
#pragma once
#include <cuda.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace axon_bench {

inline bool consumer_kernel_enabled() {
    const char* e = std::getenv("BENCH_CONSUMER_KERNEL");
    return e && std::atoi(e);
}

class GpuReadKernel {
public:
    // dst[0] = src[0] (t0), dst[1] = src[8] (head seqno), dst[2] = src[tail_off] (tail seqno)
    void init() {
        static const char kPtx[] = R"(
.version 7.5
.target sm_75
.address_size 64
.visible .entry read3(.param .u64 src, .param .u64 tail, .param .u64 dst)
{
  .reg .b64 %rd<8>;
  ld.param.u64 %rd1, [src];
  ld.param.u64 %rd2, [tail];
  ld.param.u64 %rd3, [dst];
  cvta.to.global.u64 %rd1, %rd1;
  cvta.to.global.u64 %rd3, %rd3;
  ld.global.volatile.u64 %rd4, [%rd1];
  ld.global.volatile.u64 %rd5, [%rd1+8];
  add.s64 %rd6, %rd1, %rd2;
  ld.global.volatile.u64 %rd7, [%rd6];
  st.global.volatile.u64 [%rd3], %rd4;
  st.global.volatile.u64 [%rd3+8], %rd5;
  st.global.volatile.u64 [%rd3+16], %rd7;
  ret;
}
)";
        check(cuModuleLoadData(&mod_, kPtx), "cuModuleLoadData");
        check(cuModuleGetFunction(&fn_, mod_, "read3"), "cuModuleGetFunction");
        check(cuMemHostAlloc(reinterpret_cast<void**>(&host_), 3 * sizeof(std::uint64_t), CU_MEMHOSTALLOC_DEVICEMAP),
              "cuMemHostAlloc");
        check(cuMemHostGetDevicePointer(&dev_, host_, 0), "cuMemHostGetDevicePointer");
    }

    // Enqueue on `stream` (0 = the legacy default stream); the caller syncs, then reads result().
    void launch(CUdeviceptr frame, std::size_t bytes, CUstream stream) {
        std::uint64_t tail = bytes - 8;
        void* args[] = {&frame, &tail, &dev_};
        check(cuLaunchKernel(fn_, 1, 1, 1, 1, 1, 1, 0, stream, args, nullptr), "cuLaunchKernel read3");
    }

    const volatile std::uint64_t* result() const { return host_; }  // [t0, head, tail]

private:
    static void check(CUresult r, const char* what) {
        if (r != CUDA_SUCCESS) {
            const char* msg = nullptr;
            cuGetErrorName(r, &msg);
            std::fprintf(stderr, "CUDA error: %s: %s\n", what, msg ? msg : "?");
            std::exit(1);
        }
    }
    CUmodule mod_ = nullptr;
    CUfunction fn_ = nullptr;
    std::uint64_t* host_ = nullptr;
    CUdeviceptr dev_ = 0;
};

}  // namespace axon_bench
