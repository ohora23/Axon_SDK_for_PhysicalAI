// SPDX-License-Identifier: Apache-2.0
// axon Jetson demo — camera → TensorRT inference, end to end, zero-copy against a copy path.
//
//   producer zerocopy|copy /dev/videoN [frames]: the driver writes each frame into a pool
//            (NvBufSurface for zerocopy, host for copy). E2E_FORMAT=YUYV|NV12 (default YUYV),
//            E2E_WIDTH=W (default 640), E2E_HEIGHT=H (default 480).
//            For NV12 a GREY pool W×(H*3/2) is used (Y plane + interleaved UV plane).
//   consumer zerocopy|copy <engine.plan> <frames> <out.csv>: YUYV or NV12 → normalised NCHW on
//            the GPU, then TensorRT; one CSV row per frame and a per-stage summary. E2E_WARM_GPU=1
//            runs a small kernel before each frame is taken (it lands in publish → received), so
//            the first timed GPU step does not also pay for waking the GPU between frames.
//            E2E_HOSTMEM (copy consumer only): pageable (default) copies the shared host frame with a
//            plain cudaMemcpyAsync; pinned page-locks each pool slot once (cudaHostRegister) so the H2D
//            copy is a DMA; mapped registers it as mapped memory and the kernel reads the host frame
//            directly (no H2D copy at all).
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
#include <cinttypes>
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

struct FrameConfig {
    unsigned w, h;
    unsigned rows;
    unsigned row_bytes;
    unsigned frame_bytes;
    unsigned fourcc;
    unsigned pool_height;
    unsigned pool_fourcc;
};

static std::int32_t parse_env(FrameConfig& cfg) {
    const char* fmt = getenv("E2E_FORMAT");
    if (!fmt) fmt = "YUYV";
    if (strcmp(fmt, "YUYV") != 0 && strcmp(fmt, "NV12") != 0) {
        fprintf(stderr, "E2E_FORMAT must be YUYV or NV12\n");
        return 2;
    }
    const bool is_nv12 = strcmp(fmt, "NV12") == 0;

    const char* wv = getenv("E2E_WIDTH");
    unsigned ew = 640;
    if (wv && strlen(wv) > 0) {
        char* e;
        const unsigned long v = strtoul(wv, &e, 10);   // full width first: no wrap before the range check
        if (*e != '\0' || v == 0 || v > 8192 || wv[0] == '-') {
            fprintf(stderr, "E2E_WIDTH must be an integer in 1..8192\n");
            return 2;
        }
        ew = static_cast<unsigned>(v);
    }

    const char* hv = getenv("E2E_HEIGHT");
    unsigned eh = 480;
    if (hv && strlen(hv) > 0) {
        char* e;
        const unsigned long v = strtoul(hv, &e, 10);
        if (*e != '\0' || v == 0 || v > 8192 || hv[0] == '-') {
            fprintf(stderr, "E2E_HEIGHT must be an integer in 1..8192\n");
            return 2;
        }
        eh = static_cast<unsigned>(v);
    }

    cfg.w = ew;
    cfg.h = eh;
    cfg.fourcc = is_nv12 ? V4L2_PIX_FMT_NV12 : V4L2_PIX_FMT_YUYV;
    cfg.row_bytes = is_nv12 ? ew : 2 * ew;
    cfg.rows = is_nv12 ? eh + (eh + 1) / 2 : eh;   // NV12: Y rows + ceil(H/2) UV rows
    cfg.frame_bytes = cfg.rows * cfg.row_bytes;
    cfg.pool_height = cfg.rows;
    cfg.pool_fourcc = is_nv12 ? V4L2_PIX_FMT_GREY : cfg.fourcc;
    return 0;
}

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

int set_format(int v, std::uint32_t pitch, const FrameConfig& cfg) {
    v4l2_format fmt{};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = cfg.w;
    fmt.fmt.pix.height = cfg.h;
    fmt.fmt.pix.pixelformat = cfg.fourcc;
    fmt.fmt.pix.bytesperline = pitch;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    if (xioctl(v, VIDIOC_S_FMT, &fmt) < 0 || fmt.fmt.pix.bytesperline != pitch ||
        fmt.fmt.pix.width != cfg.w || fmt.fmt.pix.height != cfg.h) {
        const char* fmt_name = (cfg.fourcc == V4L2_PIX_FMT_NV12) ? "NV12" : "YUYV";
        fprintf(stderr, "camera can't do %ux%u %s at pitch %u\n", cfg.w, cfg.h, fmt_name, pitch);
        return -1;
    }
    return 0;
}

void fill(AcquiredDescriptor& a, std::uint32_t pitch, const v4l2_buffer& b, const FrameConfig& cfg) {
    a.desc->rank = 2;
    a.desc->shape[0] = cfg.rows;
    a.desc->shape[1] = cfg.row_bytes;
    a.desc->dtype = DType::U8;
    a.desc->layout = TensorLayout::HW;
    a.desc->offset = 0;
    a.desc->row_pitch = pitch;
    a.desc->capture_ts_ns = capture_raw_ns(b);
    a.desc->capture_clock = CaptureClock::MonotonicRaw;
    a.desc->size = static_cast<std::uint64_t>(pitch) * cfg.rows;
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
int producer_zerocopy(const char* dev, long frames, const FrameConfig& cfg) {
    TensorPoolConfig tcfg{kBuffers, 0, PoolBackend::NvBufSurface, nullptr};
    tcfg.width = cfg.w;
    tcfg.height = cfg.pool_height;
    tcfg.fourcc = cfg.pool_fourcc;
    auto pool = TensorPool::create(tcfg);
    if (!pool) { perror("NvBufSurface pool"); return 1; }
    int v = open(dev, O_RDWR);
    if (v < 0) { perror(dev); return 1; }
    if (set_format(v, pool->row_pitch(), cfg) < 0) return 1;
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
        fill(a, pool->row_pitch(), b, cfg);
        if (pub->publish(std::move(a)) < 0) perror("publish");
        if (qbuf_dmabuf(v, *pool, held) < 0) { perror("re-QBUF"); break; }
        held = static_cast<int>(b.index);
    }
    xioctl(v, VIDIOC_STREAMOFF, &type);
    close(v);
    return 0;
}

// Camera → driver MMAP buffer → memcpy into a host pool slot (the usual copying pipeline).
int producer_copy(const char* dev, long frames, const FrameConfig& cfg) {
    auto pool = TensorPool::create(TensorPoolConfig{kBuffers, cfg.frame_bytes, PoolBackend::Custom, nullptr});
    if (!pool) { perror("host pool"); return 1; }
    int v = open(dev, O_RDWR);
    if (v < 0) { perror(dev); return 1; }
    if (set_format(v, cfg.row_bytes, cfg) < 0) return 1;
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
        if (b.bytesused < cfg.frame_bytes) fprintf(stderr, "producer: frame %ld short (%u of %u bytes)\n", n, b.bytesused, cfg.frame_bytes);
        AcquiredDescriptor a = pub->acquire_descriptor();   // the pool picks the slot
        std::memcpy(a.host_view, maps[b.index], cfg.frame_bytes);
        fill(a, cfg.row_bytes, b, cfg);
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

int consumer(bool zerocopy, const char* engine_path, long frames, const char* out_csv, const FrameConfig& cfg) {
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
    if (!zerocopy) CK(cudaMalloc(&d_stage, cfg.frame_bytes));
    if (!ctx->setTensorAddress(in_name, d_in) || !ctx->setTensorAddress(out_name, d_out)) {
        fprintf(stderr, "setTensorAddress failed\n");
        return 1;
    }
    const bool warm_gpu = getenv("E2E_WARM_GPU") && atoi(getenv("E2E_WARM_GPU"));
    enum class HostMem { Pageable, Pinned, Mapped } hostmem = HostMem::Pageable;
    if (const char* hm = getenv("E2E_HOSTMEM"); hm && *hm) {
        if (!strcmp(hm, "pinned")) hostmem = HostMem::Pinned;
        else if (!strcmp(hm, "mapped")) hostmem = HostMem::Mapped;
        else if (strcmp(hm, "pageable") != 0) { fprintf(stderr, "E2E_HOSTMEM must be pageable, pinned or mapped\n"); return 2; }
    }
    const char* hostmem_name = hostmem == HostMem::Pinned ? "pinned" : hostmem == HostMem::Mapped ? "mapped" : "pageable";
    // Pool slots are registered once, the first time each one is seen (within the 30 warm-up frames).
    std::vector<std::pair<const void*, const std::uint8_t*>> registered;   // host slot -> device pointer
    auto device_view = [&](const void* host) -> const std::uint8_t* {
        for (const auto& r : registered) if (r.first == host) return r.second;
        const unsigned flags = cudaHostRegisterReadOnly | (hostmem == HostMem::Mapped ? cudaHostRegisterMapped : 0u);
        if (cudaHostRegister(const_cast<void*>(host), cfg.frame_bytes, flags) != cudaSuccess) return nullptr;
        void* dev = nullptr;
        if (hostmem == HostMem::Mapped && cudaHostGetDevicePointer(&dev, const_cast<void*>(host), 0) != cudaSuccess) return nullptr;
        registered.emplace_back(host, static_cast<const std::uint8_t*>(dev ? dev : const_cast<void*>(host)));
        return registered.back().second;
    };
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
    int first = 1;
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

        if (first) {
            first = 0;
            if (v->shape.rank == 2 && (v->shape.dims[0] != cfg.rows || v->shape.dims[1] != cfg.row_bytes)) {
                fprintf(stderr, "producer and consumer formats differ (producer %" PRIu32 "x%" PRIu32 ", consumer %ux%u)\n",
                        static_cast<uint32_t>(v->shape.dims[0]), static_cast<uint32_t>(v->shape.dims[1]),
                        cfg.rows, cfg.row_bytes);
                fclose(csv);
                return 1;
            }
        }

        if (last && v->seqno > last + 1 && seen >= warmup) skipped += static_cast<long>(v->seqno - last - 1);
        last = v->seqno;

        const std::uint8_t* src;
        if (zerocopy) {
            src = static_cast<const std::uint8_t*>(v->device_ptr);
            if (!src) { fprintf(stderr, "no CUDA view of the frame\n"); return 1; }
        } else {
            if (v->row_pitch != cfg.row_bytes) {   // d_stage holds a packed frame; a padded (zerocopy) producer would overflow it
                fprintf(stderr, "copy consumer needs a packed frame (row_pitch %u, expected %u): is the producer in zerocopy mode?\n",
                        v->row_pitch, cfg.row_bytes);
                return 1;
            }
            const std::uint8_t* reg = nullptr;
            if (hostmem != HostMem::Pageable && !(reg = device_view(v->data))) {
                fprintf(stderr, "cudaHostRegister (%s) of the shared frame failed\n", hostmem_name);
                return 1;
            }
            if (hostmem == HostMem::Mapped) {
                // The kernel reads the producer's slot through the mapping: like the pageable memcpy it is not
                // covered by the seqlock, but the window is now the kernel time (fine unless 5+ frames behind).
                src = reg;
            } else {
                CK(cudaMemcpyAsync(d_stage, v->data, cfg.frame_bytes, cudaMemcpyHostToDevice, stream));
                CK(cudaStreamSynchronize(stream));
                src = static_cast<const std::uint8_t*>(d_stage);
            }
        }
        const std::uint64_t t_gpu = rt_now_ns();
        if (cfg.fourcc == V4L2_PIX_FMT_NV12) {
            CK(launch_nv12_to_nchw(src, cfg.w, cfg.h, v->row_pitch, v->row_pitch * cfg.h, d_in, net_w, net_h, stream));
        } else {
            CK(launch_yuyv_to_nchw(src, cfg.w, cfg.h, v->row_pitch, d_in, net_w, net_h, stream));
        }
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
    printf("consumer(%s%s%s%s): %ld frames -> %s, %ld frames skipped (consumer behind)\n", zerocopy ? "zerocopy" : "copy",
           zerocopy ? "" : ", host memory ", zerocopy ? "" : hostmem_name, warm_gpu ? ", GPU pre-warmed" : "", recorded,
           out_csv, skipped);
    for (const auto& r : registered) cudaHostUnregister(const_cast<void*>(r.first));
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
    FrameConfig cfg;
    std::int32_t rc = parse_env(cfg);
    if (rc != 0) return rc;
    if (argc >= 4 && !strcmp(argv[1], "producer") && (!strcmp(argv[2], "zerocopy") || !strcmp(argv[2], "copy"))) {
        const long frames = argc > 4 ? atol(argv[4]) : 0;
        return !strcmp(argv[2], "zerocopy") ? producer_zerocopy(argv[3], frames, cfg) : producer_copy(argv[3], frames, cfg);
    }
    if (argc >= 6 && !strcmp(argv[1], "consumer") && (!strcmp(argv[2], "zerocopy") || !strcmp(argv[2], "copy")))
        return consumer(!strcmp(argv[2], "zerocopy"), argv[3], atol(argv[4]), argv[5], cfg);
    fprintf(stderr,
            "usage: %s producer zerocopy|copy /dev/videoN [frames]\n"
            "       %s consumer zerocopy|copy <engine.plan> <frames> <out.csv>\n"
            "environment: E2E_FORMAT=YUYV|NV12  E2E_WIDTH=W  E2E_HEIGHT=H  (defaults: YUYV 640 480)\n"
            "             E2E_WARM_GPU=1  E2E_HOSTMEM=pageable|pinned|mapped (copy consumer)\n",
            argv[0], argv[0]);
    return 2;
}
