// SPDX-License-Identifier: Apache-2.0
// dczc — TensorSubscriber (design doc §1.1.3 / §3.3 / §3.5)
//
// wait_handshake() runs on the non-RT side: it connects to the sidecar, receives
// every dma-buf FD, and mmaps each one (accelerator import is deferred). The RT
// loop then calls latest_view(), which is a pure seqlock read with no syscalls
// and no allocation — the FDs are already attached and prefaulted.

#include "dczc/subscriber.h"

#include <cstring>
#include <string>
#include <vector>

#include <sys/mman.h>
#include <unistd.h>

#include "dczc/detail/descriptor_util.h"
#include "dczc/detail/metadata_channel.h"
#include "dczc/detail/sidecar.h"
#include "dczc/rt.h"

namespace dczc {

struct TensorSubscriber::Impl {
    std::string service_name;

    detail::SidecarClient* client = nullptr;
    detail::MetadataChannel* meta = nullptr;

    std::vector<int>   fds;          // received dma-buf FDs (owned)
    std::vector<void*> views;        // mmap of each FD (or nullptr)
    std::uint64_t      buffer_size = 0;
    std::uint32_t      pool_generation = 0;

    FallbackPolicy fallback = FallbackPolicy::LastKnownGood;

    // Last consistent view, reused by the LastKnownGood fallback.
    bool          have_last_good = false;
    TensorView    last_good {};
    std::uint64_t last_good_publish_ts_ns = 0;  // to recompute fallback staleness

    std::uint64_t handshake_count = 0;
    std::uint64_t fallback_count = 0;

    void detach() {
        for (std::size_t i = 0; i < views.size(); ++i) {
            if (views[i] && views[i] != MAP_FAILED) ::munmap(views[i], buffer_size);
        }
        for (int fd : fds) {
            if (fd >= 0) ::close(fd);   // non-RT owner closes (design doc §1.3.5)
        }
        views.clear();
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

    // Attach: cache FDs and mmap each as a zero-copy host view. Prefault so the
    // first RT access never faults (design doc §3.2).
    s.detach();
    s.fds = std::move(received);
    s.buffer_size = hdr.buffer_size;
    s.pool_generation = hdr.pool_generation;
    s.views.resize(s.fds.size(), nullptr);
    for (std::size_t i = 0; i < s.fds.size(); ++i) {
        void* p = ::mmap(nullptr, s.buffer_size, PROT_READ,
                         MAP_SHARED | MAP_POPULATE, s.fds[i], 0);
        if (p == MAP_FAILED) {
            // Some V4L2 drivers refuse direct dma-buf mmap; the accelerator-import
            // path (deferred) is the general answer. Leave the view null.
            s.views[i] = nullptr;
        } else {
            s.views[i] = p;
            rt_prefault_dma_buf_view(p, s.buffer_size);
        }
    }

    // Open the metadata plane (seqlock slot).
    delete s.meta;
    s.meta = detail::MetadataChannel::create_subscriber(s.service_name, timeout_ms);
    if (!s.meta) return -1;

    s.handshake_count++;
    return 0;
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
        if (idx < s.views.size()) {
            TensorView v {};
            void* base = s.views[idx];
            v.data = base ? static_cast<char*>(base) + desc.offset : nullptr;
            v.accel_handle = AcceleratorHandle{nullptr, PoolBackend::Custom, s.buffer_size};
            v.shape.rank = desc.rank;
            for (std::uint8_t i = 0; i < kMaxRank; ++i) v.shape.dims[i] = desc.shape[i];
            v.dtype = desc.dtype;
            v.staleness_ns = rt_now_ns() - desc.producer_publish_ts_ns;
            v.seqno = desc.seqno;
            v.sync_fd = -1;  // fence surfacing via sidecar is deferred (see sidecar.cpp)
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

}  // namespace dczc
