// SPDX-License-Identifier: Apache-2.0
// axon — Jetson camera → CUDA zero-copy demo (PoolBackend::NvBufSurface).
//
//   producer: a UVC camera captures straight INTO an NvBufSurface pool
//             (V4L2_MEMORY_DMABUF) and axon publishes only the descriptor.
//   consumer: latest_view() hands back the same frame both as a host view
//             (data) and as a CUDA device pointer (device_ptr) — no copy on
//             either side. Every 30 frames it proves they are the same bytes.
//
// Usage (two terminals, on the Jetson):
//   ./axon_jetson_camera producer /dev/video0 [frames]
//   ./axon_jetson_camera consumer [frames]

#include "axon/pool.h"
#include "axon/publisher.h"
#include "axon/subscriber.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>

#include <cuda.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>

using namespace axon;

namespace {

constexpr std::uint32_t kW = 640, kH = 480;
constexpr std::size_t kBuffers = 6;   // camera queue + the held latest frame + margin
const char* kService = "jetson/camera";

std::uint64_t fnv(const std::uint8_t* p, std::uint32_t pitch, std::uint32_t row_bytes,
                  std::uint32_t rows) {
    std::uint64_t h = 1469598103934665603ull;
    for (std::uint32_t r = 0; r < rows; ++r)
        for (std::uint32_t c = 0; c < row_bytes; ++c) h = (h ^ p[r * pitch + c]) * 1099511628211ull;
    return h;
}

int xioctl(int fd, unsigned long req, void* arg) {
    int r;
    do r = ioctl(fd, req, arg); while (r < 0 && errno == EINTR);
    return r;
}

int qbuf(int v, const TensorPool& pool, int index) {
    v4l2_buffer b{};
    b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    b.memory = V4L2_MEMORY_DMABUF;
    b.index = static_cast<std::uint32_t>(index);
    b.m.fd = pool.dma_buf_fds()[static_cast<std::size_t>(index)];
    b.length = static_cast<std::uint32_t>(pool.buffer_size());
    return xioctl(v, VIDIOC_QBUF, &b);
}

int producer(const char* dev, long frames) {
    TensorPoolConfig cfg{kBuffers, 0, PoolBackend::NvBufSurface, nullptr};
    cfg.width = kW;
    cfg.height = kH;
    cfg.fourcc = V4L2_PIX_FMT_YUYV;
    auto pool = TensorPool::create(cfg);
    if (!pool) { perror("NvBufSurface pool"); return 1; }

    int v = open(dev, O_RDWR);
    if (v < 0) { perror(dev); return 1; }
    v4l2_format fmt{};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = kW;
    fmt.fmt.pix.height = kH;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    fmt.fmt.pix.bytesperline = pool->row_pitch();
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    if (xioctl(v, VIDIOC_S_FMT, &fmt) < 0 || fmt.fmt.pix.bytesperline != pool->row_pitch() ||
        fmt.fmt.pix.width != kW || fmt.fmt.pix.height != kH) {
        fprintf(stderr, "camera can't do %ux%u YUYV at pitch %u\n", kW, kH, pool->row_pitch());
        return 1;
    }
    v4l2_requestbuffers rb{};
    rb.count = kBuffers;
    rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    rb.memory = V4L2_MEMORY_DMABUF;
    if (xioctl(v, VIDIOC_REQBUFS, &rb) < 0 || rb.count != kBuffers) { perror("REQBUFS(DMABUF)"); return 1; }

    // All but one buffer go to the camera; the held-back slot is the frame
    // consumers are reading, re-queued only once a newer frame is published.
    for (std::size_t i = 0; i + 1 < kBuffers; ++i)
        if (qbuf(v, *pool, static_cast<int>(i)) < 0) { perror("QBUF"); return 1; }
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(v, VIDIOC_STREAMON, &type) < 0) { perror("STREAMON"); return 1; }

    auto pub = TensorPublisher::create(kService, *pool);
    if (!pub) { perror("publisher"); return 1; }
    printf("producer: %zu NvBufSurface buffers %ux%u pitch=%u — camera writes into them directly\n",
           kBuffers, kW, kH, pool->row_pitch());
    printf("producer: waiting for a consumer...\n");
    pub->handshake_pool();

    int held = static_cast<int>(kBuffers - 1);   // never queued yet
    for (long n = 1; frames <= 0 || n <= frames; ++n) {
        v4l2_buffer b{};
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_DMABUF;
        if (xioctl(v, VIDIOC_DQBUF, &b) < 0) { perror("DQBUF"); break; }

        AcquiredDescriptor a = pub->acquire_descriptor();
        a.buffer_index = static_cast<int>(b.index);   // the camera picked the slot
        // axon treats the last dim as one (pitched) row, so YUYV goes out as
        // H × (W·2) bytes; publish() silently drops a descriptor that fails that rule.
        a.desc->rank = 2;
        a.desc->shape[0] = kH;
        a.desc->shape[1] = kW * 2;
        a.desc->dtype = DType::U8;
        a.desc->layout = TensorLayout::HW;
        a.desc->offset = 0;
        a.desc->size = static_cast<std::uint64_t>(pool->row_pitch()) * kH;
        a.desc->row_pitch = pool->row_pitch();
        pub->publish(std::move(a));

        if (qbuf(v, *pool, held) < 0) { perror("re-QBUF"); break; }
        held = static_cast<int>(b.index);
        if (n % 30 == 0) printf("producer: frame %ld (buffer %u)\n", n, b.index);
    }
    xioctl(v, VIDIOC_STREAMOFF, &type);
    close(v);
    return 0;
}

int consumer(long frames) {
    auto sub = TensorSubscriber::create(kService);
    if (!sub || sub->wait_handshake(10000) != 0) { perror("handshake"); return 1; }

    std::vector<std::uint8_t> host;
    std::uint64_t last = 0, checked = 0, matched = 0;
    double stale_sum = 0;
    long got = 0;
    while (frames <= 0 || got < frames) {
        auto v = sub->latest_view();
        if (!v || v->seqno == last) { usleep(1000); continue; }
        last = v->seqno;
        ++got;
        stale_sum += static_cast<double>(v->staleness_ns) / 1e3;
        if (!v->device_ptr) { fprintf(stderr, "no CUDA view (built without CUDA?)\n"); return 1; }

        if (got % 30 == 0) {
            // Proof only: copy the device view back and compare with the host view.
            // The pipeline itself never copies — a CUDA kernel would read device_ptr.
            const std::uint32_t rp = v->row_pitch;
            host.resize(static_cast<std::size_t>(rp) * kH);
            cuMemcpyDtoH(host.data(), reinterpret_cast<CUdeviceptr>(v->device_ptr), host.size());
            const auto* hv = static_cast<const std::uint8_t*>(v->data);
            bool same = fnv(host.data(), rp, kW * 2, kH) == fnv(hv, rp, kW * 2, kH);
            ++checked;
            matched += same;
            printf("consumer: seq=%llu staleness(avg)=%.0f us device_ptr=%p host==device: %s\n",
                   static_cast<unsigned long long>(v->seqno), stale_sum / static_cast<double>(got), v->device_ptr,
                   same ? "yes" : "NO");
        }
    }
    printf("consumer: %ld frames, host==device on %llu/%llu checks\n", got,
           static_cast<unsigned long long>(matched), static_cast<unsigned long long>(checked));
    return matched == checked ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    if (argc >= 3 && !strcmp(argv[1], "producer"))
        return producer(argv[2], argc > 3 ? atol(argv[3]) : 0);
    if (argc >= 2 && !strcmp(argv[1], "consumer"))
        return consumer(argc > 2 ? atol(argv[2]) : 0);
    fprintf(stderr, "usage: %s producer /dev/videoN [frames] | consumer [frames]\n", argv[0]);
    return 2;
}
