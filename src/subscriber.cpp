// SPDX-License-Identifier: Apache-2.0
// axon — TensorSubscriber (design doc §1.1.3 / §3.3 / §3.5)
//
// wait_handshake() runs on the non-RT side: it connects to the sidecar, receives
// every dma-buf FD, and mmaps each one (accelerator import is deferred). The RT
// loop then calls latest_view(), which is a pure seqlock read with no syscalls
// and no allocation — the FDs are already attached and prefaulted.

#include "axon/subscriber.h"

#include <array>
#include <cerrno>
#include <climits>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include <linux/futex.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "axon/detail/accelerator.h"
#include "axon/detail/descriptor_util.h"
#include "axon/detail/metadata_channel.h"
#include "axon/detail/nvbuf.h"
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
#if AXON_ENABLE_JETSON
    std::vector<detail::NvBuf> nvbufs;       // imported NvBufSurface per buffer
    std::vector<detail::NvBufCuda> nvcudas;  // CUDA view per buffer
#endif

    FallbackPolicy fallback = FallbackPolicy::LastKnownGood;

    // Last consistent view, reused by the LastKnownGood fallback.
    bool          have_last_good = false;
    TensorView    last_good {};
    std::uint64_t last_good_publish_ts_ns = 0;  // to recompute fallback staleness

    std::uint64_t handshake_count = 0;
    std::uint64_t fallback_count = 0;

    // wait_for_update / wait_any: what this subscriber has already seen.
    std::uint32_t seen_notify = 0;  // notify_word() value at the last latest_view()
    std::uint64_t seen_seqno = 0;   // seqno of the last latest_view() (polling fallback)

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
#if AXON_ENABLE_JETSON
        for (std::size_t i = 0; i < nvbufs.size(); ++i) {
            detail::nvbuf_cuda_unmap(nvbufs[i], &nvcudas[i]);
            detail::nvbuf_free(&nvbufs[i]);
        }
        nvbufs.clear();
        nvcudas.clear();
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
    std::vector<std::uint8_t> ext;
    if (s.client->recv_pool_handshake(&hdr, &received, timeout_ms, &ext) < 0) return -1;
    if (hdr.wire_version != kWireVersion) {
        for (int fd : received) ::close(fd);
        errno = EPROTO;
        return -1;
    }

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

    // Jetson NvBufSurface: import FDs + map CUDA device view from the ext blob.
#if AXON_ENABLE_JETSON
    // The device view is optional: a malformed blob just leaves device_ptr null.
    const std::size_t psz = detail::nvbuf_import_params_size();
    if (hdr.backend == static_cast<std::uint8_t>(PoolBackend::NvBufSurface) &&
        ext.size() == s.fds.size() * psz) {
        s.nvbufs.resize(s.fds.size());
        s.nvcudas.resize(s.fds.size());
        s.device_ptrs.resize(s.fds.size(), nullptr);
        for (std::size_t i = 0; i < s.fds.size(); ++i) {
            if (detail::nvbuf_import(s.fds[i], ext.data() + i * psz, &s.nvbufs[i])) {
                detail::nvbuf_cuda_map(s.nvbufs[i], &s.nvcudas[i]);
                s.device_ptrs[i] = s.nvcudas[i].device_ptr;  // nullptr on CUDA failure
            }
        }
    }
#endif

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

    // Read notify BEFORE the descriptor: a publish landing after this load makes the
    // next wait return at once, so no update can slip between a read and a wait. It is
    // committed to seen_notify only when a fresh frame is handed out below, so a frame
    // skipped now (fence not drained yet, generation mismatch) still wakes the next wait.
    auto* notify = s.meta->notify_word();
    const std::uint32_t notify_now = notify ? notify->load(std::memory_order_acquire) : 0;

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
            void* host = s.device_backed ? nullptr : s.views[idx];
            void* dev  = s.device_backed ? s.device_ptrs[idx]
                       : (idx < s.device_ptrs.size() ? s.device_ptrs[idx] : nullptr);
            char* hp = host ? static_cast<char*>(host) + desc.offset : nullptr;
            char* dp = dev  ? static_cast<char*>(dev)  + desc.offset : nullptr;
            v.data       = hp;   // null for device-backed (Accelerator) frames
            v.device_ptr = dp;   // Accelerator, or NvBufSurface with a CUDA view
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
            s.seen_notify = notify_now;
            s.seen_seqno = desc.seqno;
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

// ---------------------------------------------------------------- wait

namespace {

// True if subscriber `s` has a frame newer than its last latest_view().
bool has_update(detail::MetadataChannel* meta, std::uint32_t seen_notify, std::uint64_t seen_seqno) {
    if (auto* w = meta->notify_word()) return w->load(std::memory_order_seq_cst) != seen_notify;
    TensorDescriptor d;
    int r = 0;
    return meta->read_latest(&d, 8, &r) && d.seqno != seen_seqno;
}

timespec deadline_after(int timeout_ms) {
    timespec t {};
    clock_gettime(CLOCK_MONOTONIC, &t);
    t.tv_sec += timeout_ms / 1000;
    t.tv_nsec += static_cast<long>(timeout_ms % 1000) * 1000000L;
    if (t.tv_nsec >= 1000000000L) { t.tv_sec += 1; t.tv_nsec -= 1000000000L; }
    return t;
}

bool past(const timespec& d) {
    timespec now {};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec > d.tv_sec || (now.tv_sec == d.tv_sec && now.tv_nsec >= d.tv_nsec);
}

}  // namespace

int TensorSubscriber::wait_for_update(int timeout_ms) noexcept {
    TensorSubscriber* self = this;
    int r = wait_any(&self, 1, timeout_ms);
    if (r == 0) return 1;
    return errno == ETIMEDOUT ? 0 : -1;
}

// futex_waitv needs linux-libc-dev >= 5.16 (struct futex_waitv, SYS_futex_waitv). With
// older headers (JetPack 5 ships 5.4) only the polling path is built.
#if defined(SYS_futex_waitv) && defined(FUTEX_WAITV_MAX)
#define AXON_HAVE_FUTEX_WAITV 1
constexpr std::size_t kMaxWaitv = FUTEX_WAITV_MAX;
#else
#define AXON_HAVE_FUTEX_WAITV 0
constexpr std::size_t kMaxWaitv = 0;
#endif

int TensorSubscriber::wait_any(TensorSubscriber* const* subs, std::size_t n, int timeout_ms) noexcept {
    if (n == 0 || subs == nullptr) { errno = EINVAL; return -1; }
    for (std::size_t i = 0; i < n; ++i)
        if (!subs[i] || !subs[i]->impl_->meta) { errno = EINVAL; return -1; }

    const timespec deadline = deadline_after(timeout_ms < 0 ? 0 : timeout_ms);
    bool futexes = n <= kMaxWaitv;
    for (std::size_t i = 0; i < n && futexes; ++i)
        futexes = subs[i]->impl_->meta->notify_word() != nullptr;

    int result = -1;
    int err = ETIMEDOUT;
#if AXON_HAVE_FUTEX_WAITV
    futex_waitv w[FUTEX_WAITV_MAX];
#endif
    for (;;) {
        // Raise the waiter flag before checking (see SeqlockMetadataChannel::publish):
        // a publish after the check below then either is seen by it or wakes us.
        if (futexes)
            for (std::size_t i = 0; i < n; ++i)
                subs[i]->impl_->meta->waiters_word()->store(1, std::memory_order_seq_cst);

        for (std::size_t i = 0; i < n; ++i) {
            const Impl& s = *subs[i]->impl_;
            if (has_update(s.meta, s.seen_notify, s.seen_seqno)) { result = static_cast<int>(i); break; }
        }
        if (result >= 0) break;
        if (timeout_ms >= 0 && past(deadline)) break;

#if AXON_HAVE_FUTEX_WAITV
        if (futexes) {
            for (std::size_t i = 0; i < n; ++i) {
                w[i] = futex_waitv {};
                w[i].val = subs[i]->impl_->seen_notify;
                w[i].uaddr = reinterpret_cast<std::uintptr_t>(subs[i]->impl_->meta->notify_word());
                w[i].flags = FUTEX2_SIZE_U32;  // process-shared: no FUTEX_PRIVATE_FLAG
            }
            long rc = ::syscall(SYS_futex_waitv, w, static_cast<unsigned>(n), 0,
                                timeout_ms < 0 ? nullptr : &deadline, CLOCK_MONOTONIC);
            if (rc >= 0 || errno == EAGAIN || errno == EINTR) continue;  // woken or value moved: recheck
            if (errno == ETIMEDOUT) continue;                             // loop ends on past(deadline)
            if (errno == ENOSYS) { futexes = false; continue; }          // kernel < 5.16: poll
            err = errno;
            break;
        }
#endif
        timespec step {0, 100000};  // ponytail: 100 µs poll fallback (old kernels, iceoryx2)
        nanosleep(&step, nullptr);
    }

    if (result < 0) errno = err;
    return result;
}

}  // namespace axon
