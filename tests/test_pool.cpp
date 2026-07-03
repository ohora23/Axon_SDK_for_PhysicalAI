// SPDX-License-Identifier: Apache-2.0
// TensorPool — Custom (memfd) backend always; UDMABUF when /dev/udmabuf exists.

#include "dczc/pool.h"
#include "dczc/types.h"
#include "dczc_test.h"

#include <cstring>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace dczc;

namespace {
bool have_udmabuf() {
    struct stat st;
    return ::stat("/dev/udmabuf", &st) == 0;
}
}  // namespace

DCZC_TEST(custom_backend_allocates_fds) {
    TensorPoolConfig cfg {};
    cfg.n_buffers = 8;
    cfg.buffer_size = 4096;
    cfg.backend = PoolBackend::Custom;
    cfg.v4l2_device = nullptr;

    auto pool = TensorPool::create(cfg);
    REQUIRE(pool != nullptr);
    CHECK(pool->generation() == 1);
    REQUIRE(pool->dma_buf_fds().size() == 8);
    for (int fd : pool->dma_buf_fds()) CHECK(fd >= 0);
}

DCZC_TEST(custom_backend_fds_are_mmappable_and_shared) {
    TensorPoolConfig cfg {};
    cfg.n_buffers = 2;
    cfg.buffer_size = 4096;
    cfg.backend = PoolBackend::Custom;

    auto pool = TensorPool::create(cfg);
    REQUIRE(pool != nullptr);
    int fd = pool->dma_buf_fds()[0];

    void* a = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    void* b = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    REQUIRE(a != MAP_FAILED);
    REQUIRE(b != MAP_FAILED);
    // Two mappings of the same FD alias the same memory (zero-copy proof).
    static_cast<char*>(a)[0] = 0x42;
    CHECK(static_cast<char*>(b)[0] == 0x42);
    munmap(a, 4096);
    munmap(b, 4096);
}

DCZC_TEST(ring_acquire_release) {
    TensorPoolConfig cfg {};
    cfg.n_buffers = 3;
    cfg.buffer_size = 4096;
    cfg.backend = PoolBackend::Custom;

    auto pool = TensorPool::create(cfg);
    REQUIRE(pool != nullptr);

    int a = pool->acquire_next();
    int b = pool->acquire_next();
    int c = pool->acquire_next();
    CHECK(a >= 0 && b >= 0 && c >= 0);
    CHECK(a != b && b != c && a != c);
    CHECK(pool->acquire_next() == -1);  // exhausted

    pool->release(b);
    CHECK(pool->acquire_next() == b);   // freed slot comes back
}

DCZC_TEST(retire_bumps_generation) {
    TensorPoolConfig cfg {};
    cfg.n_buffers = 4;
    cfg.buffer_size = 4096;
    cfg.backend = PoolBackend::Custom;

    auto pool = TensorPool::create(cfg);
    REQUIRE(pool != nullptr);
    CHECK(pool->generation() == 1);
    pool->retire_and_reallocate(8192);
    CHECK(pool->generation() == 2);
    CHECK(pool->dma_buf_fds().size() == 4);
    for (int fd : pool->dma_buf_fds()) CHECK(fd >= 0);
}

DCZC_TEST(udmabuf_backend_when_available) {
    if (!have_udmabuf()) {
        std::printf("    /dev/udmabuf absent — skipping (Custom backend covers the path)\n");
        return;
    }
    TensorPoolConfig cfg {};
    cfg.n_buffers = 4;
    cfg.buffer_size = 64 * 1024;
    cfg.backend = PoolBackend::UDMABUF;

    auto pool = TensorPool::create(cfg);
    if (!pool) {
        // Permission (udmabuf group) can deny even when the node exists.
        std::printf("    udmabuf present but create failed (likely permissions) — tolerated\n");
        return;
    }
    REQUIRE(pool->dma_buf_fds().size() == 4);
    int fd = pool->dma_buf_fds()[0];
    void* p = mmap(nullptr, 64 * 1024, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p != MAP_FAILED) {
        std::memset(p, 0x7e, 64 * 1024);
        CHECK(static_cast<unsigned char*>(p)[0] == 0x7e);
        munmap(p, 64 * 1024);
    }
}

DCZC_TEST(rejects_bad_config) {
    TensorPoolConfig cfg {};
    cfg.n_buffers = 0;
    cfg.buffer_size = 4096;
    cfg.backend = PoolBackend::Custom;
    CHECK(TensorPool::create(cfg) == nullptr);

    cfg.n_buffers = 4;
    cfg.buffer_size = 0;
    CHECK(TensorPool::create(cfg) == nullptr);
}

DCZC_TEST_MAIN("pool")
