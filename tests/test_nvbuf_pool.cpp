// SPDX-License-Identifier: Apache-2.0
// TensorPool — NvBufSurface backend e2e (Jetson only).
//
// Built only with AXON_ENABLE_JETSON. Verifies:
//   pool_basics — allocate a NvBufSurface pool and check geometry / FDs / row_pitch.
//   cross_process_host_view — fork; producer writes per-row patterns, consumer
//   reads back through the existing host-mmap path.
//   cross_process_device_view (AXON_WITH_CUDA) — consumer reads the same frame
//   through latest_view()->device_ptr (NvBufSurfaceImport → EGL → CUDA).

#include "axon/pool.h"
#include "axon/publisher.h"
#include "axon/subscriber.h"
#include "axon/rt.h"
#include "axon_test.h"

#include <cstdint>
#include <cstring>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <linux/videodev2.h>

#if AXON_WITH_CUDA
#include <cuda.h>
#endif

using namespace axon;

namespace {

constexpr int   kNBuffers  = 8;
constexpr int   kWidth     = 640;
constexpr int   kHeight    = 480;
constexpr std::uint32_t kFourcc = V4L2_PIX_FMT_YUYV;
constexpr int   kFrames    = 20;
const char*     kService   = "e2e/nvbuf_pool_stream";

void sleep_ms(int ms) {
    struct timespec ts {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, nullptr);
}

// ---- consumer process ----
int run_consumer() {
    auto sub = TensorSubscriber::create(kService);
    if (!sub) return 10;
    if (sub->wait_handshake(4000) != 0) return 11;

    std::uint64_t seen = 0;
    bool content_ok = false;
    for (int i = 0; i < 4000; ++i) {
        auto v = sub->latest_view(8);
        if (v && v->seqno >= kFrames) {
            seen = v->seqno;
            if (v->data) {
                const std::uint8_t* p = static_cast<const std::uint8_t*>(v->data);
                const std::uint32_t rp = v->row_pitch;
                // Verify row 0: all bytes should equal (seqno % 256).
                std::uint8_t expected = static_cast<std::uint8_t>(v->seqno % 256);
                bool row0_ok = true;
                for (int c = 0; c < 128; ++c) {
                    if (p[c] != expected) { row0_ok = false; break; }
                }
                // Verify row 1: all bytes should equal ((seqno+1) % 256).
                std::uint8_t expected1 = static_cast<std::uint8_t>((v->seqno + 1) % 256);
                bool row1_ok = true;
                for (int c = 0; c < 128; ++c) {
                    if (p[rp + c] != expected1) { row1_ok = false; break; }
                }
                content_ok = row0_ok && row1_ok;
            }
            break;
        }
        sleep_ms(1);
    }
    if (seen < kFrames) return 14;
    if (!content_ok) return 15;
    return 0;
}

}  // namespace

AXON_TEST(pool_basics) {
    TensorPoolConfig c {};
    c.n_buffers    = 4;
    c.buffer_size  = 0;
    c.backend      = PoolBackend::NvBufSurface;
    c.v4l2_device  = nullptr;
    c.width        = kWidth;
    c.height       = kHeight;
    c.fourcc       = kFourcc;

    auto pool = TensorPool::create(c);
    REQUIRE(pool != nullptr);
    REQUIRE(pool->dma_buf_fds().size() == 4);
    for (int fd : pool->dma_buf_fds()) CHECK(fd >= 0);

    std::uint32_t rp = pool->row_pitch();
    CHECK(rp >= static_cast<std::uint32_t>(kWidth * 2));  // YUYV = 2 bytes/pixel

    std::size_t bs = pool->buffer_size();
    CHECK(bs >= static_cast<std::size_t>(rp) * kHeight);

    CHECK(pool->backend() == PoolBackend::NvBufSurface);

    // Bad fourcc → nullptr.
    TensorPoolConfig bad = c;
    bad.fourcc = V4L2_PIX_FMT_MJPEG;
    CHECK(TensorPool::create(bad) == nullptr);
}

AXON_TEST(cross_process_host_view) {
    TensorPoolConfig c {};
    c.n_buffers    = kNBuffers;
    c.buffer_size  = 0;
    c.backend      = PoolBackend::NvBufSurface;
    c.v4l2_device  = nullptr;
    c.width        = kWidth;
    c.height       = kHeight;
    c.fourcc       = kFourcc;

    auto pool = TensorPool::create(c);
    REQUIRE(pool != nullptr);

    auto pub = TensorPublisher::create(kService, *pool);
    REQUIRE(pub != nullptr);

    pid_t pid = fork();
    REQUIRE(pid >= 0);
    if (pid == 0) _exit(run_consumer());

    int accepted = pub->handshake_pool();
    CHECK(accepted >= 1);

    std::uint32_t rp = pool->row_pitch();

    for (int s = 1; s <= kFrames; ++s) {
        AcquiredDescriptor a = pub->acquire_descriptor();
        REQUIRE(a.buffer_index >= 0);
        REQUIRE(a.host_view != nullptr);

        // Write a per-row pattern: row r has bytes equal to (s + r) % 256.
        std::uint8_t* base = static_cast<std::uint8_t*>(a.host_view);
        for (int r = 0; r < kHeight; ++r) {
            std::uint8_t val = static_cast<std::uint8_t>((s + r) % 256);
            std::memset(base + r * rp, val, 1280);
        }

        a.desc->rank = 2;
        a.desc->shape[0] = static_cast<std::uint32_t>(kHeight);
        a.desc->shape[1] = static_cast<std::uint32_t>(kWidth);
        a.desc->dtype = DType::U8;
        a.desc->offset = 0;
        a.desc->size = rp * kHeight;
        a.desc->row_pitch = rp;
        a.desc->sync_fence_kind = SyncFenceKind::None;

        pub->publish(std::move(a));
        sleep_ms(2);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status));
    if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
        std::printf("    consumer exit code = %d\n", WEXITSTATUS(status));
    }
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}


#if AXON_WITH_CUDA
const char* kServiceCuda = "e2e/nvbuf_pool_cuda";

AXON_TEST(cross_process_device_view) {
    TensorPoolConfig c {};
    c.n_buffers    = kNBuffers;
    c.buffer_size  = 0;
    c.backend      = PoolBackend::NvBufSurface;
    c.v4l2_device  = nullptr;
    c.width        = kWidth;
    c.height       = kHeight;
    c.fourcc       = kFourcc;

    pid_t pid = fork();
    REQUIRE(pid >= 0);

    if (pid == 0) {
        // Child = consumer
        cuInit(0);
        CUdevice dev;
        cuDeviceGet(&dev, 0);
        CUcontext ctx;
        cuDevicePrimaryCtxRetain(&ctx, dev);
        cuCtxSetCurrent(ctx);

        auto sub = TensorSubscriber::create(kServiceCuda);
        if (!sub) _exit(10);
        if (sub->wait_handshake(4000) != 0) _exit(11);

        std::uint64_t seen = 0;
        bool content_ok = false;
        for (int i = 0; i < 4000; ++i) {
            auto v = sub->latest_view(8);
            if (v && v->seqno >= kFrames && v->data && v->device_ptr) {
                seen = v->seqno;
                const std::uint32_t rp = v->row_pitch;
                // Copy row_pitch*2 bytes from device to host.
                std::vector<std::uint8_t> dev_buf(rp * 2);
                cuMemcpyDtoH(dev_buf.data(),
                             reinterpret_cast<CUdeviceptr>(v->device_ptr),
                             rp * 2);
                const std::uint8_t* host = static_cast<const std::uint8_t*>(v->data);
                // Compare first 1280 bytes of row 0 and row 1.
                content_ok = (memcmp(host, dev_buf.data(), 1280) == 0 &&
                              memcmp(host + rp, dev_buf.data() + rp, 1280) == 0);
            }
            if (seen >= kFrames) break;
            sleep_ms(1);
        }
        if (seen < kFrames) _exit(14);
        if (!content_ok) _exit(15);
        _exit(0);
    }

    // Parent = producer
    auto pool = TensorPool::create(c);
    REQUIRE(pool != nullptr);

    auto pub = TensorPublisher::create(kServiceCuda, *pool);
    REQUIRE(pub != nullptr);

    int accepted = pub->handshake_pool();
    CHECK(accepted >= 1);

    // Verify device_ptr is non-null after fork.
    CHECK(pool->device_ptr(0) != nullptr);

    std::uint32_t rp = pool->row_pitch();

    for (int s = 1; s <= kFrames; ++s) {
        AcquiredDescriptor a = pub->acquire_descriptor();
        REQUIRE(a.buffer_index >= 0);
        REQUIRE(a.host_view != nullptr);

        std::uint8_t* base = static_cast<std::uint8_t*>(a.host_view);
        for (int r = 0; r < kHeight; ++r) {
            std::uint8_t val = static_cast<std::uint8_t>((s + r) % 256);
            std::memset(base + r * rp, val, 1280);
        }

        a.desc->rank = 2;
        a.desc->shape[0] = static_cast<std::uint32_t>(kHeight);
        a.desc->shape[1] = static_cast<std::uint32_t>(kWidth);
        a.desc->dtype = DType::U8;
        a.desc->offset = 0;
        a.desc->size = rp * kHeight;
        a.desc->row_pitch = rp;
        a.desc->sync_fence_kind = SyncFenceKind::None;

        pub->publish(std::move(a));
        sleep_ms(2);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status));
    if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
        std::printf("    consumer exit code = %d\n", WEXITSTATUS(status));
    }
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

#endif  // AXON_WITH_CUDA

AXON_TEST_MAIN("nvbuf_pool")
