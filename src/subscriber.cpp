// SPDX-License-Identifier: Apache-2.0
// axon — TensorSubscriber (design doc §1.1.3 / §3.3 / §3.5)
//
// wait_handshake() runs on the non-RT side: it connects to the sidecar, receives
// every dma-buf FD, and mmaps each one (accelerator import is deferred). The RT
// loop then calls latest_view(), which is a pure seqlock read with no syscalls
// and no allocation — the FDs are already attached and prefaulted.

#include "axon/subscriber.h"

#include <array>
#include <cstring>
#include <string>
#include <vector>

#include <sys/mman.h>
#include <unistd.h>

#include "axon/detail/accelerator.h"
#include "axon/detail/descriptor_util.h"
#include "axon/detail/metadata_channel.h"
#include "axon/detail/sidecar.h"
#include "axon/pool.h"
#include "axon/rt.h"

namespace axon {

struct TensorSubscriber::Impl {
    std::string service_name;

    detail::SidecarClient* client = nullptr;
    detail::MetadataChannel* meta = nullptr;

    std::vector<int>   fds;          // received dma-buf FDs (owned)
    std::vector<void*> views;        // host mmap of each FD (or nullptr)
    std::uint64_t      buffer_size = 0;
    std::uint32_t      pool_generation = 0;

    // Device (Accelerator) path: imported per-buffer device pointers + the
    // AccelBuffer handles used to release them. Empty for host backends.
    bool device_backed = false;
    std::vector<void*> device_ptrs;
    std::vector<detail::AccelBuffer> accel_bufs;

    FallbackPolicy fallback = FallbackPolicy::LastKnownGood;

    // Last consistent view, reused by the LastKnownGood fallback.
    bool          have_last_good = false;
    TensorView    last_good {};
    std::uint64_t last_good_publish_ts_ns = 0;  // to recompute fallback staleness

    std::uint64_t handshake_count = 0;
    std::uint64_t fallback_count = 0;

    // ---- Sync-fence cache (R2, option B) ----
    // drain_fences() (non-RT) fills this ring off the sidecar socket; latest_view()
    // (RT) looks a descriptor's token up here with no syscall. Latest-value-wins:
    // when the ring wraps, the superseded fence FD is closed (else it leaks). Ring
    // size >> frames in flight for a single-host consumer; if a needed token was
    // already evicted, lookup misses and latest_view() skips that frame.
    struct FenceSlot { std::uint64_t token = 0; int fd = -1; };
    static constexpr std::size_t kFenceRing = 16;
    std::array<FenceSlot, kFenceRing> fences {};
    std::size_t fence_next = 0;

    void store_fence(std::uint64_t token, int fd) {
        FenceSlot& slot = fences[fence_next];
        if (slot.fd >= 0) ::close(slot.fd);  // evict the superseded fence
        slot.token = token;
        slot.fd = fd;
        fence_next = (fence_next + 1) % kFenceRing;
    }

    int lookup_fence(std::uint64_t token) const noexcept {
        for (const FenceSlot& s : fences) {
            if (s.fd >= 0 && s.token == token) return s.fd;
        }
        return -1;
    }

    void close_fences() {
        for (FenceSlot& s : fences) {
            if (s.fd >= 0) { ::close(s.fd); s.fd = -1; }
        }
        fence_next = 0;
    }

    void detach() {
        for (std::size_t i = 0; i < views.size(); ++i) {
            if (views[i] && views[i] != MAP_FAILED) ::munmap(views[i], buffer_size);
        }
#if AXON_WITH_CUDA
        for (auto& ab : accel_bufs) detail::accel_free(&ab);  // unmap device memory
#endif
        for (int fd : fds) {
            if (fd >= 0) ::close(fd);   // non-RT owner closes (design doc §1.3.5)
        }
        close_fences();   // fences belong to the retiring pool generation
        // The cached good frame aliases buffers we just freed — invalidate it so
        // a post-detach fallback can't hand back a dangling data/device_ptr.
        have_last_good = false;
        views.clear();
        device_ptrs.clear();
        accel_bufs.clear();
        device_backed = false;
        fds.clear();
    }
};

TensorSubscriber::TensorSubscriber() : impl_(std::make_unique<Impl>()) {}

TensorSubscriber::~TensorSubscriber() {
    impl_->detach();
    delete impl_->client;
    delete impl_->meta;
}

std::unique_ptr<TensorSubscriber> TensorSubscriber::create(
    std::string_view service_name) {
    auto sub = std::unique_ptr<TensorSubscriber>(new TensorSubscriber());
    sub->impl_->service_name.assign(service_name);
    sub->impl_->client = detail::SidecarClient::create(sub->impl_->service_name);
    if (!sub->impl_->client) return nullptr;
    return sub;
}

int TensorSubscriber::wait_handshake(int timeout_ms) {
    Impl& s = *impl_;

    if (s.client->connect(timeout_ms) < 0) return -1;

    detail::PoolHandshakeHeader hdr {};
    std::vector<int> received;
    if (s.client->recv_pool_handshake(&hdr, &received, timeout_ms) < 0) return -1;
    if (hdr.wire_version != kWireVersion) { errno = EPROTO; return -1; }

    // Attach: cache FDs, then either host-mmap (UMA backends) or device-import
    // (Accelerator) each buffer as a zero-copy view.
    s.detach();
    s.fds = std::move(received);
    s.buffer_size = hdr.buffer_size;
    s.pool_generation = hdr.pool_generation;
    s.device_backed =
        (hdr.backend == static_cast<std::uint8_t>(PoolBackend::Accelerator));

    if (s.device_backed) {
#if AXON_WITH_CUDA
        // The shared FDs are CUDA VMM POSIX handles, not host-mappable dma-bufs —
        // import each into this process' address space as a device pointer.
        s.device_ptrs.assign(s.fds.size(), nullptr);
        s.accel_bufs.assign(s.fds.size(), detail::AccelBuffer{});
        for (std::size_t i = 0; i < s.fds.size(); ++i) {
            detail::AccelBuffer ab{};
            if (detail::accel_import(s.fds[i], s.buffer_size, &ab)) {
                s.accel_bufs[i] = ab;
                s.device_ptrs[i] = ab.device_ptr;
            }  // else leave null — latest_view falls back for that buffer
        }
#else
        // GPU pool but this build has no CUDA support — cannot consume it.
        errno = ENOTSUP;
        return -1;
#endif
    } else {
        s.views.resize(s.fds.size(), nullptr);
        for (std::size_t i = 0; i < s.fds.size(); ++i) {
            void* p = ::mmap(nullptr, s.buffer_size, PROT_READ,
                             MAP_SHARED | MAP_POPULATE, s.fds[i], 0);
            if (p == MAP_FAILED) {
                // Some V4L2 drivers refuse direct dma-buf mmap. Leave the view null.
                s.views[i] = nullptr;
            } else {
                s.views[i] = p;
                rt_prefault_dma_buf_view(p, s.buffer_size);
            }
        }
    }

    // Open the metadata plane (seqlock slot).
    delete s.meta;
    s.meta = detail::MetadataChannel::create_subscriber(s.service_name, timeout_ms);
    if (!s.meta) return -1;

    s.handshake_count++;
    return 0;
}

int TensorSubscriber::drain_fences() noexcept {
    Impl& s = *impl_;
    if (!s.client) return 0;
    // Captureless lambda -> C function pointer; ctx carries the Impl. Timeout 0 =
    // bounded, non-blocking drain, so the caller places the only syscall here.
    auto sink = [](std::uint64_t token, int fd, void* ctx) {
        static_cast<Impl*>(ctx)->store_fence(token, fd);
    };
    return s.client->poll_sync_fences(0, sink, &s);
}

std::optional<TensorView> TensorSubscriber::latest_view(int max_retry) noexcept {
    Impl& s = *impl_;
    if (!s.meta) return std::nullopt;

    TensorDescriptor desc;
    int retries = 0;
    bool ok = s.meta->read_latest(&desc, max_retry, &retries);

    // Pool-generation mismatch means the FDs we hold are stale — a re-handshake
    // is required (deferred: counted + treated as a fallback trigger here).
    bool gen_ok = ok && (desc.pool_generation == s.pool_generation);

    if (ok && gen_ok) {
        std::size_t idx = static_cast<std::size_t>(desc.bo_handle);
        // A fence-gated frame whose fence hasn't been drained yet may still be
        // mid-write on the producer's accelerator — skip it (fall through to the
        // fallback) rather than hand out a possibly-torn view.
        int fence_fd = -1;
        bool fence_ready = true;
        if (desc.sync_fence_kind == SyncFenceKind::SyncFileViaSidecar) {
            fence_fd = s.lookup_fence(desc.sync_fence_token);
            fence_ready = (fence_fd >= 0);
        }
        const std::size_t nbufs = s.device_backed ? s.device_ptrs.size()
                                                   : s.views.size();
        if (fence_ready && idx < nbufs) {
            TensorView v {};
            void* base = s.device_backed ? s.device_ptrs[idx] : s.views[idx];
            char* p = base ? static_cast<char*>(base) + desc.offset : nullptr;
            v.data       = s.device_backed ? nullptr : p;
            v.device_ptr = s.device_backed ? p : nullptr;
            v.accel_handle = AcceleratorHandle{nullptr, PoolBackend::Custom, s.buffer_size};
            v.shape.rank = desc.rank;
            for (std::uint8_t i = 0; i < kMaxRank; ++i) v.shape.dims[i] = desc.shape[i];
            v.dtype = desc.dtype;
            v.staleness_ns = rt_now_ns() - desc.producer_publish_ts_ns;
            v.seqno = desc.seqno;
            v.sync_fd = fence_fd;  // borrowed; owned by the fence ring
            v.seqlock_retries = retries;

            // v2 imaging/depth metadata — forward so the consumer can index
            // padded rows, scale to physical units, and deproject.
            v.row_pitch = desc.row_pitch;
            v.depth_scale = desc.depth_scale;
            v.invalid_value = desc.invalid_value;
            v.sample_units = desc.sample_units;
            v.capture_clock = desc.capture_clock;
            v.layout = desc.layout;
            v.capture_ts_ns = desc.capture_ts_ns;
            v.intr_fx = desc.intr_fx; v.intr_fy = desc.intr_fy;
            v.intr_cx = desc.intr_cx; v.intr_cy = desc.intr_cy;
            v.intr_ref_width = desc.intr_ref_width;
            v.intr_ref_height = desc.intr_ref_height;

            s.last_good = v;
            s.last_good.sync_fd = -1;  // the stored copy owns no fence (already synced)
            s.last_good_publish_ts_ns = desc.producer_publish_ts_ns;
            s.have_last_good = true;
            return v;
        }
    }

    // Fallback path (design doc §3.5).
    s.fallback_count++;
    switch (s.fallback) {
        case FallbackPolicy::LastKnownGood:
            if (s.have_last_good) {
                TensorView v = s.last_good;
                // Staleness keeps growing while we reuse the stale frame, so the
                // RT safety analysis can still see the bound being approached.
                v.staleness_ns = rt_now_ns() - s.last_good_publish_ts_ns;
                v.seqlock_retries = retries;
                return v;
            }
            return std::nullopt;
        case FallbackPolicy::ZeroCommand:
        case FallbackPolicy::UserCallback:
        case FallbackPolicy::AbortLoop:
        default:
            return std::nullopt;  // RT loop applies the safe-stop / user policy
    }
}

void TensorSubscriber::set_fallback_policy(FallbackPolicy p) noexcept {
    impl_->fallback = p;
}

std::uint64_t TensorSubscriber::pool_handshake_count() const noexcept {
    return impl_->handshake_count;
}

std::uint64_t TensorSubscriber::fallback_invocation_count() const noexcept {
    return impl_->fallback_count;
}

}  // namespace axon
