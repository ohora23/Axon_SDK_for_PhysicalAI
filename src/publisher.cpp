// SPDX-License-Identifier: Apache-2.0
// axon — TensorPublisher (design doc §1.1.3 / §1.3)
//
// Owns the sidecar server (FD plane) and the metadata channel (metadata plane).
// handshake_pool() bulk-delivers every pool FD once; subsequent publish() calls
// carry only the fixed-size TensorDescriptor through the seqlock slot, so the
// sidecar cost is amortized to ~0 in steady state (design doc §5.1, T_sc).

#include "axon/publisher.h"

#include <atomic>
#include <cstring>
#include <string>
#include <vector>

#include <sys/mman.h>
#include <unistd.h>

#include "axon/detail/descriptor_util.h"
#include "axon/detail/metadata_channel.h"
#include "axon/detail/sidecar.h"
#include "axon/rt.h"

namespace axon {

namespace {
constexpr int kHandshakeWaitMs = 2000;  // wait for the first consumer to connect
}

struct TensorPublisher::Impl {
    std::string service_name;
    TensorPool* pool = nullptr;
    std::size_t buffer_size = 0;

    detail::SidecarServer* server = nullptr;
    detail::MetadataChannel* meta = nullptr;

    std::vector<void*> host_views;   // producer-side mmap of each buffer (UMA)
    TensorDescriptor staging {};     // filled by the caller between acquire/publish
    std::size_t next_index = 0;
    SeqNo seqno = 0;

    void unmap_views() {
        for (void* v : host_views) {
            if (v && v != MAP_FAILED) ::munmap(v, buffer_size);
        }
        host_views.clear();
    }

    void map_views() {
        unmap_views();
        const auto& fds = pool->dma_buf_fds();
        host_views.resize(fds.size(), nullptr);
        for (std::size_t i = 0; i < fds.size(); ++i) {
            void* p = ::mmap(nullptr, buffer_size, PROT_READ | PROT_WRITE,
                             MAP_SHARED, fds[i], 0);
            host_views[i] = (p == MAP_FAILED) ? nullptr : p;  // V4L2 may refuse
        }
    }
};

TensorPublisher::TensorPublisher() : impl_(std::make_unique<Impl>()) {}

TensorPublisher::~TensorPublisher() {
    impl_->unmap_views();
    delete impl_->server;
    delete impl_->meta;
}

std::unique_ptr<TensorPublisher> TensorPublisher::create(
    std::string_view service_name, TensorPool& pool) {
    auto pub = std::unique_ptr<TensorPublisher>(new TensorPublisher());
    pub->impl_->service_name.assign(service_name);
    pub->impl_->pool = &pool;

    // Per-buffer byte size: use the pool's authoritative page-aligned size.
    // (lseek(SEEK_END) is only a fallback — some real dma-buf exporters reject
    // it and would silently yield 0, breaking every mmap.)
    pub->impl_->buffer_size = pool.buffer_size();
    if (pub->impl_->buffer_size == 0) {
        const auto& fds = pool.dma_buf_fds();
        if (!fds.empty()) {
            off_t sz = ::lseek(fds[0], 0, SEEK_END);
            if (sz > 0) pub->impl_->buffer_size = static_cast<std::size_t>(sz);
        }
    }
    if (pub->impl_->buffer_size == 0) return nullptr;  // can't map buffers

    pub->impl_->server = detail::SidecarServer::create(pub->impl_->service_name);
    pub->impl_->meta = detail::MetadataChannel::create_publisher(pub->impl_->service_name);
    if (!pub->impl_->server || !pub->impl_->meta) return nullptr;

    pub->impl_->map_views();
    return pub;
}

int TensorPublisher::handshake_pool() {
    impl_->server->set_pool(impl_->pool->dma_buf_fds(),
                            static_cast<std::uint32_t>(impl_->pool->generation()),
                            impl_->buffer_size, kWireVersion);
    return impl_->server->accept_and_handshake(kHandshakeWaitMs);
}

AcquiredDescriptor TensorPublisher::acquire_descriptor() {
    const auto& fds = impl_->pool->dma_buf_fds();
    AcquiredDescriptor a {};
    if (fds.empty()) {
        a.desc = &impl_->staging;
        a.buffer_index = -1;
        return a;
    }
    // Latest-value-wins ring: round-robin over the pool, overwriting the oldest.
    // Deliberately wait-free — no slot ref-count / back-pressure (decided design;
    // a slow consumer gets a fresher frame, never blocks the producer). Torn-frame
    // safety is bought with ring depth, not locking — see docs/usage.md sizing.
    int idx = static_cast<int>(impl_->next_index % fds.size());
    impl_->next_index++;

    std::memset(&impl_->staging, 0, sizeof(impl_->staging));
    impl_->staging.bo_handle = static_cast<BoHandle>(idx);
    impl_->staging.pool_generation = impl_->pool->generation();

    a.desc = &impl_->staging;
    a.buffer_index = idx;
    a.host_view = (static_cast<std::size_t>(idx) < impl_->host_views.size())
                      ? impl_->host_views[static_cast<std::size_t>(idx)]
                      : nullptr;
    a.accel_handle = AcceleratorHandle{nullptr, PoolBackend::Custom, impl_->buffer_size};
    return a;
}

void TensorPublisher::publish(AcquiredDescriptor&& d, int sync_fd) {
    TensorDescriptor* desc = d.desc;

    // Producer-stamped fields (the caller owns shape/dtype/offset/size).
    desc->seqno = ++impl_->seqno;
    desc->pool_generation = impl_->pool->generation();
    desc->bo_handle = static_cast<BoHandle>(d.buffer_index >= 0 ? d.buffer_index : 0);
    desc->producer_publish_ts_ns = rt_now_ns();

    // Reject a descriptor whose [offset, size) or row_pitch would read/write
    // outside the buffer — drop the frame rather than publish a corrupt view.
    if (!detail::descriptor_is_valid(*desc, impl_->buffer_size)) return;

    // Deliver an explicit fence FD before publishing the metadata that references
    // it, so a consumer that reads the new seqno can already find the fence.
    if (sync_fd >= 0 && desc->sync_fence_kind == SyncFenceKind::SyncFileViaSidecar) {
        impl_->server->broadcast_sync_fence(desc->sync_fence_token, sync_fd);
    }

    impl_->meta->publish(*desc);

    // Pick up any consumer that connected after the initial handshake (non-RT,
    // non-blocking); they receive the current pool bundle.
    impl_->server->set_pool(impl_->pool->dma_buf_fds(),
                            static_cast<std::uint32_t>(impl_->pool->generation()),
                            impl_->buffer_size, kWireVersion);
    impl_->server->accept_and_handshake(0);
}

int TensorPublisher::reannounce_pool() {
    impl_->map_views();  // FDs changed after a retire/reallocate
    impl_->server->set_pool(impl_->pool->dma_buf_fds(),
                            static_cast<std::uint32_t>(impl_->pool->generation()),
                            impl_->buffer_size, kWireVersion);
    int n = impl_->server->broadcast_pool();
    n += impl_->server->accept_and_handshake(0);
    return n;
}

}  // namespace axon
