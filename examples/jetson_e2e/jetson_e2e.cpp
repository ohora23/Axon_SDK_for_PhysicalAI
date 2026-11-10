// SPDX-License-Identifier: Apache-2.0
// axon Jetson demo — camera → TensorRT inference, end to end, zero-copy against a copy path.
//
//   producer zerocopy /dev/videoN [frames]: the driver writes each frame into an NvBufSurface pool
//            slot (as in jetson_camera; no user-space copy); the consumer gets its CUDA pointer.
//   producer copy /dev/videoN [frames]: V4L2 MMAP buffers, memcpy'd into a host (memfd) pool;
//            the consumer copies the frame to the GPU (pageable H2D) before using it.
//   consumer zerocopy|copy <engine.plan> <frames> <out.csv>: YUYV → normalised NCHW on the GPU,
//            then TensorRT; one CSV row per frame and a per-stage summary. E2E_WARM_GPU=1 runs a
//            small kernel before each frame is taken (outside the timed stages), so the first
//            timed GPU step does not also pay for waking the GPU between frames.
//
// All timestamps are CLOCK_MONOTONIC_RAW (axon::rt_now_ns). The capture time is the V4L2 buffer
// timestamp (CLOCK_MONOTONIC) moved to MONOTONIC_RAW with the offset read at DQBUF.

#include "axon/pool.h"
#include "axon/publisher.h"
#include "axon/rt.h"
#include "axon/subscriber.h"
#include "preprocess.h"

#include <NvInfer.h>
#include <cuda_runtime.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>
#include <memory>
#include <vector>

using namespace axon;

namespace {

constexpr std::uint32_t kW = 640, kH = 480, kFrameBytes = kW * kH * 2;
constexpr std::size_t kBuffers = 6;     // pool slots (as jetson_camera)
constexpr std::uint32_t kMmapBuffers = 4;
const char* kService = "jetson/e2e";

int xioctl(int fd, unsigned long req, void* arg) {
    int r;
    do r = ioctl(fd, req, arg); while (r < 0 && errno == EINTR);
    return r;
}

std::uint64_t clock_ns(clockid_t c) {
    timespec ts;
    clock_gettime(c, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<std::uint64_t>(ts.tv_nsec);
}

// The V4L2 timestamp of a dequeued buffer, on CLOCK_MONOTONIC_RAW.
std::uint64_t capture_raw_ns(const v4l2_buffer& b) {
    if ((b.flags & V4L2_BUF_FLAG_TIMESTAMP_MASK) != V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC) {
        static bool warned = false;
        if (!warned) { fprintf(stderr, "producer: buffer timestamps are not CLOCK_MONOTONIC, using DQBUF time\n"); warned = true; }
        return rt_now_ns();
    }
    const std::int64_t off = static_cast<std::int64_t>(clock_ns(CLOCK_MONOTONIC)) -
                             static_cast<std::int64_t>(clock_ns(CLOCK_MONOTONIC_RAW));
    const std::int64_t mono = static_cast<std::int64_t>(b.timestamp.tv_sec) * 1000000000ll +
                              static_cast<std::int64_t>(b.timestamp.tv_usec) * 1000ll;
    return static_cast<std::uint64_t>(mono - off);
}

int set_format(int v, std::uint32_t pitch) {
    v4l2_format fmt{};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = kW;
    fmt.fmt.pix.height = kH;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    fmt.fmt.pix.bytesperline = pitch;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    if (xioctl(v, VIDIOC_S_FMT, &fmt) < 0 || fmt.fmt.pix.bytesperline != pitch ||
        fmt.fmt.pix.width != kW || fmt.fmt.pix.height != kH) {
        fprintf(stderr, "camera can't do %ux%u YUYV at pitch %u\n", kW, kH, pitch);
        return -1;
    }
    return 0;
}

void fill(AcquiredDescriptor& a, std::uint32_t pitch, const v4l2_buffer& b) {
    a.desc->rank = 2;
    a.desc->shape[0] = kH;
    a.desc->shape[1] = kW * 2;
    a.desc->dtype = DType::U8;
    a.desc->layout = TensorLayout::HW;
    a.desc->offset = 0;
    a.desc->size = static_cast<std::uint64_t>(pitch) * kH;
    a.desc->row_pitch = pitch;
    a.desc->capture_ts_ns = capture_raw_ns(b);
    a.desc->capture_clock = CaptureClock::MonotonicRaw;
}

int qbuf_dmabuf(int v, const TensorPool& pool, int index) {
    v4l2_buffer b{};
    b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    b.memory = V4L2_MEMORY_DMABUF;
    b.index = static_cast<std::uint32_t>(index);
    b.m.fd = pool.dma_buf_fds()[static_cast<std::size_t>(index)];
    b.length = static_cast<std::uint32_t>(pool.buffer_size());
    return xioctl(v, VIDIOC_QBUF, &b);
}

// Camera → NvBufSurface pool, no copy (the jetson_camera producer plus capture timestamps).
int producer_zerocopy(const char* dev, long frames) {
    TensorPoolConfig cfg{kBuffers, 0, PoolBackend::NvBufSurface, nullptr};
    cfg.width = kW;
    cfg.height = kH;
    cfg.fourcc = V4L2_PIX_FMT_YUYV;
    auto pool = TensorPool::create(cfg);
    if (!pool) { perror("NvBufSurface pool"); return 1; }
    int v = open(dev, O_RDWR);
    if (v < 0) { perror(dev); return 1; }
    if (set_format(v, pool->row_pitch()) < 0) return 1;
    v4l2_requestbuffers rb{};
    rb.count = kBuffers;
    rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    rb.memory = V4L2_MEMORY_DMABUF;
    if (xioctl(v, VIDIOC_REQBUFS, &rb) < 0 || rb.count != kBuffers) { perror("REQBUFS(DMABUF)"); return 1; }
    for (std::size_t i = 0; i + 1 < kBuffers; ++i)   // one slot held back: the frame being read
        if (qbuf_dmabuf(v, *pool, static_cast<int>(i)) < 0) { perror("QBUF"); return 1; }
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(v, VIDIOC_STREAMON, &type) < 0) { perror("STREAMON"); return 1; }

    auto pub = TensorPublisher::create(kService, *pool);
    if (!pub) { perror("publisher"); return 1; }
    printf("producer(zerocopy): %zu NvBufSurface buffers, pitch %u; waiting for a consumer\n", kBuffers,
           pool->row_pitch());
    pub->handshake_pool();

    int held = static_cast<int>(kBuffers - 1);
    for (long n = 1; frames <= 0 || n <= frames; ++n) {
        v4l2_buffer b{};
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_DMABUF;
        if (xioctl(v, VIDIOC_DQBUF, &b) < 0) { perror("DQBUF"); break; }
        if (b.flags & V4L2_BUF_FLAG_ERROR) fprintf(stderr, "producer: frame %ld flagged corrupt by the driver\n", n);
        AcquiredDescriptor a = pub->acquire_descriptor();
        a.buffer_index = static_cast<int>(b.index);   // the camera picked the slot
        fill(a, pool->row_pitch(), b);
        if (pub->publish(std::move(a)) < 0) perror("publish");
        if (qbuf_dmabuf(v, *pool, held) < 0) { perror("re-QBUF"); break; }
        held = static_cast<int>(b.index);
    }
    xioctl(v, VIDIOC_STREAMOFF, &type);
    close(v);
    return 0;
}

// Camera → driver MMAP buffer → memcpy into a host pool slot (the usual copying pipeline).
int producer_copy(const char* dev, long frames) {
    auto pool = TensorPool::create(TensorPoolConfig{kBuffers, kFrameBytes, PoolBackend::Custom, nullptr});
    if (!pool) { perror("host pool"); return 1; }
    int v = open(dev, O_RDWR);
    if (v < 0) { perror(dev); return 1; }
    if (set_format(v, kW * 2) < 0) return 1;
    v4l2_requestbuffers rb{};
    rb.count = kMmapBuffers;
    rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    rb.memory = V4L2_MEMORY_MMAP;
    if (xioctl(v, VIDIOC_REQBUFS, &rb) < 0 || rb.count < 2) { perror("REQBUFS(MMAP)"); return 1; }
    std::vector<const void*> maps(rb.count);
    for (std::uint32_t i = 0; i < rb.count; ++i) {
        v4l2_buffer b{};
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = i;
        if (xioctl(v, VIDIOC_QUERYBUF, &b) < 0) { perror("QUERYBUF"); return 1; }
        void* p = mmap(nullptr, b.length, PROT_READ, MAP_SHARED, v, b.m.offset);
        if (p == MAP_FAILED) { perror("mmap"); return 1; }
        maps[i] = p;
        if (xioctl(v, VIDIOC_QBUF, &b) < 0) { perror("QBUF"); return 1; }
    }
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(v, VIDIOC_STREAMON, &type) < 0) { perror("STREAMON"); return 1; }

    auto pub = TensorPublisher::create(kService, *pool);
    if (!pub) { perror("publisher"); return 1; }
    printf("producer(copy): %u MMAP buffers memcpy'd into a host pool; waiting for a consumer\n", rb.count);
    pub->handshake_pool();

    for (long n = 1; frames <= 0 || n <= frames; ++n) {
        v4l2_buffer b{};
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        if (xioctl(v, VIDIOC_DQBUF, &b) < 0) { perror("DQBUF"); break; }
        if (b.flags & V4L2_BUF_FLAG_ERROR) fprintf(stderr, "producer: frame %ld flagged corrupt by the driver\n", n);
        AcquiredDescriptor a = pub->acquire_descriptor();   // the pool picks the slot
        std::memcpy(a.host_view, maps[b.index], kFrameBytes);
        fill(a, kW * 2, b);
        if (pub->publish(std::move(a)) < 0) perror("publish");
        if (xioctl(v, VIDIOC_QBUF, &b) < 0) { perror("re-QBUF"); break; }
    }
    xioctl(v, VIDIOC_STREAMOFF, &type);
    close(v);
    return 0;
}

class Logger : public nvinfer1::ILogger {
    void log(Severity s, const char* msg) noexcept override {
        if (s <= Severity::kWARNING) fprintf(stderr, "[TRT] %s\n", msg);
    }
} g_logger;

#define CK(call)                                                                              \
    do {                                                                                      \
        cudaError_t e_ = (call);                                                              \
        if (e_ != cudaSuccess) { fprintf(stderr, "%s: %s\n", #call, cudaGetErrorString(e_)); return 1; } \
    } while (0)

std::int64_t volume(const nvinfer1::Dims& d) {
    std::int64_t n = 1;
    for (int i = 0; i < d.nbDims; ++i) n *= d.d[i];
    return n;
}

void summary(const char* label, std::vector<double> us) {
    if (us.empty()) return;
    std::sort(us.begin(), us.end());
    auto at = [&](double p) { return us[std::min(us.size() - 1, static_cast<std::size_t>(us.size() * p))]; };
    printf("  %-12s p50 %8.1f  p90 %8.1f  p99 %8.1f  max %8.1f us\n", label, at(0.5), at(0.9), at(0.99), us.back());
}

int consumer(bool zerocopy, const char* engine_path, long frames, const char* out_csv) {
    CK(cudaSetDevice(0));
    CK(cudaFree(nullptr));   // bind the primary context (axon's CUDA import uses it too)
    cudaStream_t stream;
    CK(cudaStreamCreate(&stream));

    std::ifstream in(engine_path, std::ios::binary);
    std::vector<char> plan((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (plan.empty()) { fprintf(stderr, "cannot read %s\n", engine_path); return 1; }
    std::unique_ptr<nvinfer1::IRuntime> runtime(nvinfer1::createInferRuntime(g_logger));
    std::unique_ptr<nvinfer1::ICudaEngine> engine(
        runtime ? runtime->deserializeCudaEngine(plan.data(), plan.size()) : nullptr);
    std::unique_ptr<nvinfer1::IExecutionContext> ctx(engine ? engine->createExecutionContext() : nullptr);
    if (!ctx) { fprintf(stderr, "TensorRT engine/context failed\n"); return 1; }

    const char* in_name = nullptr;
    const char* out_name = nullptr;
    if (engine->getNbIOTensors() != 2) { fprintf(stderr, "engine needs exactly one input and one output\n"); return 1; }
    for (int i = 0; i < 2; ++i) {
        const char* n = engine->getIOTensorName(i);
        if (engine->getTensorDataType(n) != nvinfer1::DataType::kFLOAT) { fprintf(stderr, "%s is not FP32\n", n); return 1; }
        (engine->getTensorIOMode(n) == nvinfer1::TensorIOMode::kINPUT ? in_name : out_name) = n;
    }
    if (!in_name || !out_name) { fprintf(stderr, "engine needs one input and one output\n"); return 1; }
    const nvinfer1::Dims din = engine->getTensorShape(in_name), dout = engine->getTensorShape(out_name);
    if (din.nbDims != 4 || din.d[0] != 1 || din.d[1] != 3) { fprintf(stderr, "input must be [1,3,H,W]\n"); return 1; }
    const int net_h = din.d[2], net_w = din.d[3];
    const std::int64_t n_out = volume(dout);

    float* d_in = nullptr;
    float* d_out = nullptr;
    void* d_stage = nullptr;
    CK(cudaMalloc(&d_in, sizeof(float) * 3 * net_h * net_w));
    CK(cudaMalloc(&d_out, sizeof(float) * n_out));
    if (!zerocopy) CK(cudaMalloc(&d_stage, kFrameBytes));
    if (!ctx->setTensorAddress(in_name, d_in) || !ctx->setTensorAddress(out_name, d_out)) {
        fprintf(stderr, "setTensorAddress failed\n");
        return 1;
    }
    const bool warm_gpu = getenv("E2E_WARM_GPU") && atoi(getenv("E2E_WARM_GPU"));
    std::vector<float> scores(static_cast<std::size_t>(n_out));
    printf("consumer(%s): engine %s [1,3,%d,%d] -> %lld\n", zerocopy ? "zerocopy" : "copy", engine_path, net_h,
           net_w, static_cast<long long>(n_out));

    auto sub = TensorSubscriber::create(kService);
    if (!sub || sub->wait_handshake(15000) != 0) { perror("handshake"); return 1; }

    FILE* csv = fopen(out_csv, "w");
    if (!csv) { perror(out_csv); return 1; }
    fprintf(csv, "seq,t_cap,t_pub,t_view,t_gpu,t_pre,t_inf,top1,seqlock_retries\n");
    std::vector<double> cap_pub, pub_view, view_gpu, gpu_pre, pre_inf, cap_inf;
    const long warmup = 30;
    long seen = 0, recorded = 0, skipped = 0;
    std::uint64_t last = 0;
    int idle_ms = 0;
    while (recorded < frames) {
        const int w = sub->wait_for_update(1000);
        if (w < 0) { perror("wait_for_update"); return 1; }
        if (w == 0) {
            if ((idle_ms += 1000) >= 5000) { fprintf(stderr, "no frame for 5 s: producer gone?\n"); return 1; }
            continue;
        }
        idle_ms = 0;
        if (warm_gpu) {   // wake the GPU outside the timed stages (scratch input, result unused)
            CK(launch_yuyv_to_nchw(reinterpret_cast<const std::uint8_t*>(d_in), 2, 2, 4, d_out, 1, 1, stream));
            CK(cudaStreamSynchronize(stream));
        }
        auto v = sub->latest_view();
        if (!v || v->seqno == last) continue;
        const std::uint64_t t_view = rt_now_ns();
        if (last && v->seqno > last + 1 && seen >= warmup) skipped += static_cast<long>(v->seqno - last - 1);
        last = v->seqno;

        const std::uint8_t* src;
        if (zerocopy) {
            src = static_cast<const std::uint8_t*>(v->device_ptr);
            if (!src) { fprintf(stderr, "no CUDA view of the frame\n"); return 1; }
        } else {
            CK(cudaMemcpyAsync(d_stage, v->data, kFrameBytes, cudaMemcpyHostToDevice, stream));
            CK(cudaStreamSynchronize(stream));
            src = static_cast<const std::uint8_t*>(d_stage);
        }
        const std::uint64_t t_gpu = rt_now_ns();
        CK(launch_yuyv_to_nchw(src, kW, kH, v->row_pitch, d_in, net_w, net_h, stream));
        CK(cudaStreamSynchronize(stream));
        const std::uint64_t t_pre = rt_now_ns();
        if (!ctx->enqueueV3(stream)) { fprintf(stderr, "enqueueV3 failed\n"); return 1; }
        CK(cudaStreamSynchronize(stream));
        const std::uint64_t t_inf = rt_now_ns();

        if (++seen <= warmup) continue;
        CK(cudaMemcpy(scores.data(), d_out, sizeof(float) * n_out, cudaMemcpyDeviceToHost));   // not timed
        const int top1 = static_cast<int>(std::max_element(scores.begin(), scores.end()) - scores.begin());
        const std::uint64_t t_cap = v->capture_ts_ns, t_pub = t_view - v->staleness_ns;
        fprintf(csv, "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%d,%d\n", static_cast<unsigned long long>(v->seqno),
                static_cast<unsigned long long>(t_cap), static_cast<unsigned long long>(t_pub),
                static_cast<unsigned long long>(t_view), static_cast<unsigned long long>(t_gpu),
                static_cast<unsigned long long>(t_pre), static_cast<unsigned long long>(t_inf), top1,
                v->seqlock_retries);
        auto us = [](std::uint64_t a, std::uint64_t b) { return static_cast<double>(static_cast<std::int64_t>(b - a)) / 1e3; };
        cap_pub.push_back(us(t_cap, t_pub));
        pub_view.push_back(us(t_pub, t_view));
        view_gpu.push_back(us(t_view, t_gpu));
        gpu_pre.push_back(us(t_gpu, t_pre));
        pre_inf.push_back(us(t_pre, t_inf));
        cap_inf.push_back(us(t_cap, t_inf));
        ++recorded;
    }
    fclose(csv);
    printf("consumer(%s%s): %ld frames -> %s, %ld frames skipped (consumer behind)\n", zerocopy ? "zerocopy" : "copy",
           warm_gpu ? ", GPU pre-warmed" : "", recorded, out_csv, skipped);
    summary("cap_to_pub", cap_pub);
    summary("pub_to_view", pub_view);
    summary("view_to_gpu", view_gpu);
    summary("gpu_to_pre", gpu_pre);
    summary("pre_to_inf", pre_inf);
    summary("cap_to_inf", cap_inf);
    cudaFree(d_in);
    cudaFree(d_out);
    if (d_stage) cudaFree(d_stage);
    cudaStreamDestroy(stream);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    if (argc >= 4 && !strcmp(argv[1], "producer") && (!strcmp(argv[2], "zerocopy") || !strcmp(argv[2], "copy"))) {
        const long frames = argc > 4 ? atol(argv[4]) : 0;
        return !strcmp(argv[2], "zerocopy") ? producer_zerocopy(argv[3], frames) : producer_copy(argv[3], frames);
    }
    if (argc >= 6 && !strcmp(argv[1], "consumer") && (!strcmp(argv[2], "zerocopy") || !strcmp(argv[2], "copy")))
        return consumer(!strcmp(argv[2], "zerocopy"), argv[3], atol(argv[4]), argv[5]);
    fprintf(stderr,
            "usage: %s producer zerocopy|copy /dev/videoN [frames]\n"
            "       %s consumer zerocopy|copy <engine.plan> <frames> <out.csv>\n",
            argv[0], argv[0]);
    return 2;
}
