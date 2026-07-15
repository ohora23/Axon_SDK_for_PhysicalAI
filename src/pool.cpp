// SPDX-License-Identifier: Apache-2.0
// axon — TensorPool: dma-buf-backed buffer pool (design doc §4.1)
//
// Backends:
//   Custom  — memfd-backed buffers. Dependency-free, always available; the FDs
//             are SCM_RIGHTS-transferable and mmap-able, so the sidecar /
//             seqlock data path is fully exercisable without a camera or
//             /dev/udmabuf. This is the default for tests and CI.
//   UDMABUF — real dma-bufs created from memfd via /dev/udmabuf. Lets the FD
//             plane be validated against genuine dma-buf objects, still without
//             a camera.
//   V4L2    — VIDIOC_REQBUFS + VIDIOC_EXPBUF, ported from the week 1-2 spike.
//             Requires a capture device.

#include "axon/pool.h"

#include <cerrno>
#include <cstring>
#include <vector>

#include <fcntl.h>
#include <linux/udmabuf.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#if AXON_WITH_CUDA
#include "axon/detail/accelerator.h"
#endif

namespace axon {

namespace {

std::size_t page_round_up(std::size_t n) {
    long pg = sysconf(_SC_PAGESIZE);
    std::size_t page = pg > 0 ? static_cast<std::size_t>(pg) : 4096;
    return (n + page - 1) & ~(page - 1);
}

// One memfd-backed buffer exported as a dma-buf (UDMABUF) or used directly
// (Custom). For UDMABUF we keep the backing memfd alive for the dma-buf's
// lifetime.
struct Buffer {
    int share_fd = -1;   // FD handed to consumers (dma-buf, memfd, or CUDA export)
    int backing_fd = -1; // memfd kept alive for UDMABUF; -1 otherwise
    // Accelerator backend only:
    void* device_ptr = nullptr;     // CUdeviceptr in the owning process
    unsigned long long accel_h = 0; // CUmemGenericAllocationHandle (for free)
    std::size_t accel_size = 0;     // granularity-aligned size
};

int make_sealed_memfd(std::size_t size) {
    int fd = memfd_create("axon_buf", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) return -1;
    if (ftruncate(fd, static_cast<off_t>(size)) < 0) {
        ::close(fd);
        return -1;
    }
    // udmabuf requires the backing memfd to be shrink-sealed.
    if (fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK) < 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

}  // namespace

struct TensorPool::Impl {
    TensorPoolConfig cfg {};
    std::size_t buffer_size = 0;          // page-aligned actual size
    PoolGeneration gen = 1;               // starts at 1 (matches spike PoC)
    std::vector<Buffer> buffers;
    std::vector<int> share_fds;           // cached view for dma_buf_fds()
    std::vector<bool> in_use;
    std::size_t ring_head = 0;

    // V4L2-only state.
    int v4l2_fd = -1;

    bool allocate();
    void free_all();
    bool allocate_custom();
    bool allocate_udmabuf();
    bool allocate_v4l2();
    bool allocate_accelerator();
    void rebuild_share_fds();
};

void TensorPool::Impl::rebuild_share_fds() {
    share_fds.clear();
    share_fds.reserve(buffers.size());
    for (const auto& b : buffers) share_fds.push_back(b.share_fd);
}

bool TensorPool::Impl::allocate_custom() {
    for (std::size_t i = 0; i < cfg.n_buffers; ++i) {
        int fd = make_sealed_memfd(buffer_size);
        if (fd < 0) return false;
        buffers.push_back(Buffer{fd, -1});
    }
    return true;
}

bool TensorPool::Impl::allocate_udmabuf() {
    int udma = ::open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
    if (udma < 0) return false;

    bool ok = true;
    for (std::size_t i = 0; i < cfg.n_buffers && ok; ++i) {
        int memfd = make_sealed_memfd(buffer_size);
        if (memfd < 0) { ok = false; break; }

        struct udmabuf_create create {};
        create.memfd = static_cast<__u32>(memfd);
        create.flags = UDMABUF_FLAGS_CLOEXEC;
        create.offset = 0;
        create.size = buffer_size;

        int dmabuf = ::ioctl(udma, UDMABUF_CREATE, &create);
        if (dmabuf < 0) {
            ::close(memfd);
            ok = false;
            break;
        }
        buffers.push_back(Buffer{dmabuf, memfd});
    }
    ::close(udma);
    return ok;
}

bool TensorPool::Impl::allocate_v4l2() {
    if (!cfg.v4l2_device) { errno = EINVAL; return false; }
    int fd = ::open(cfg.v4l2_device, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return false;
    v4l2_fd = fd;

    struct v4l2_requestbuffers req {};
    req.count = static_cast<__u32>(cfg.n_buffers);
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (::ioctl(fd, VIDIOC_REQBUFS, &req) < 0) return false;
    if (req.count < cfg.n_buffers) { errno = ENOSPC; return false; }

    for (std::size_t i = 0; i < cfg.n_buffers; ++i) {
        struct v4l2_exportbuffer expbuf {};
        expbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        expbuf.index = static_cast<__u32>(i);
        expbuf.flags = O_RDWR | O_CLOEXEC;
        if (::ioctl(fd, VIDIOC_EXPBUF, &expbuf) < 0) return false;
        buffers.push_back(Buffer{expbuf.fd, -1});
    }
    return true;
}

bool TensorPool::Impl::allocate_accelerator() {
#if AXON_WITH_CUDA
    for (std::size_t i = 0; i < cfg.n_buffers; ++i) {
        detail::AccelBuffer ab;
        if (!detail::accel_alloc(buffer_size, &ab)) return false;
        Buffer b;
        b.share_fd = ab.export_fd;   // POSIX shareable handle → sidecar via dma_buf_fds()
        b.device_ptr = ab.device_ptr;
        b.accel_h = ab.handle;
        b.accel_size = ab.aligned;
        buffers.push_back(b);
    }
    return true;
#else
    errno = ENOSYS;   // built without CUDA
    return false;
#endif
}

bool TensorPool::Impl::allocate() {
    bool ok = false;
    switch (cfg.backend) {
        case PoolBackend::Custom:      ok = allocate_custom();      break;
        case PoolBackend::UDMABUF:     ok = allocate_udmabuf();     break;
        case PoolBackend::V4L2:        ok = allocate_v4l2();        break;
        case PoolBackend::Accelerator: ok = allocate_accelerator(); break;
    }
    if (!ok) { free_all(); return false; }

    in_use.assign(buffers.size(), false);
    ring_head = 0;
    rebuild_share_fds();
    return true;
}

void TensorPool::Impl::free_all() {
    for (auto& b : buffers) {
#if AXON_WITH_CUDA
        if (b.device_ptr) {   // Accelerator: unmap/free device memory + close export FD
            detail::AccelBuffer ab{b.device_ptr, b.share_fd, b.accel_size, b.accel_h};
            detail::accel_free(&ab);
            continue;
        }
#endif
        if (b.share_fd >= 0) ::close(b.share_fd);
        if (b.backing_fd >= 0) ::close(b.backing_fd);
    }
    buffers.clear();
    share_fds.clear();
    in_use.clear();
    if (v4l2_fd >= 0) { ::close(v4l2_fd); v4l2_fd = -1; }
}

TensorPool::TensorPool() : impl_(std::make_unique<Impl>()) {}
TensorPool::~TensorPool() { impl_->free_all(); }

std::unique_ptr<TensorPool> TensorPool::create(const TensorPoolConfig& cfg) {
    if (cfg.n_buffers == 0 || cfg.buffer_size == 0) return nullptr;

    auto pool = std::unique_ptr<TensorPool>(new TensorPool());
    pool->impl_->cfg = cfg;
    pool->impl_->buffer_size = page_round_up(cfg.buffer_size);
    if (!pool->impl_->allocate()) return nullptr;
    return pool;
}

const std::vector<int>& TensorPool::dma_buf_fds() const noexcept {
    return impl_->share_fds;
}

PoolGeneration TensorPool::generation() const noexcept {
    return impl_->gen;
}

std::size_t TensorPool::buffer_size() const noexcept {
    return impl_->buffer_size;
}

PoolBackend TensorPool::backend() const noexcept {
    return impl_->cfg.backend;
}

void* TensorPool::device_ptr(std::size_t index) const noexcept {
    return index < impl_->buffers.size() ? impl_->buffers[index].device_ptr : nullptr;
}

int TensorPool::acquire_next() {
    const std::size_t n = impl_->buffers.size();
    for (std::size_t k = 0; k < n; ++k) {
        std::size_t idx = (impl_->ring_head + k) % n;
        if (!impl_->in_use[idx]) {
            impl_->in_use[idx] = true;
            impl_->ring_head = (idx + 1) % n;
            return static_cast<int>(idx);
        }
    }
    return -1;  // pool exhausted — RT loop is behind; producer should back off
}

void TensorPool::release(int index) {
    if (index >= 0 && static_cast<std::size_t>(index) < impl_->in_use.size()) {
        impl_->in_use[static_cast<std::size_t>(index)] = false;
    }
}

void TensorPool::retire_and_reallocate(std::size_t new_buffer_size) {
    impl_->free_all();
    impl_->buffer_size = page_round_up(new_buffer_size);
    impl_->gen += 1;
    impl_->allocate();
}

}  // namespace axon
