// SPDX-License-Identifier: Apache-2.0
// axon_bridge — reusable, ROS-agnostic bootstrap over the axon dma-buf pool and
// the FD sidecar. Header-only and free of any ROS dependency, so ROS1 and (later)
// ROS2 glue share the exact same class.
//
// Split of responsibilities:
//   - THIS bridge owns the payload plane: pool allocation, mmap of every buffer,
//     and the SCM_RIGHTS sidecar handshake (server on the producer, client on the
//     consumer). It is the batch of boilerplate every integration repeats.
//   - The CALLER owns the metadata plane: it maps its own message type
//     (a ROS topic message, etc.) to/from the fields it needs, and writes/reads
//     payload through buffer()/payload(). Only descriptors + the pool FDs (once)
//     leave the process; the tensor never travels through ROS.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <sys/mman.h>
#include <unistd.h>

#include "axon/pool.h"
#include "axon/tensor_descriptor.h"   // kWireVersion
#include "axon/types.h"
#include "axon/detail/sidecar.h"

namespace axon_bridge {

// ---------------------------------------------------------------------------
// Producer: owns a dma-buf pool, mmaps every buffer read-write, and runs the FD
// sidecar server. Write payload into buffer(i), then publish your own descriptor.
// ---------------------------------------------------------------------------
class Producer {
public:
    // Returns nullptr on any failure (pool, mmap, or sidecar).
    static std::unique_ptr<Producer> create(
        const std::string& service, std::size_t n_buffers, std::size_t buffer_size,
        axon::PoolBackend backend = axon::PoolBackend::Custom) {

        auto pool = axon::TensorPool::create({n_buffers, buffer_size, backend, nullptr});
        if (!pool) return nullptr;

        const auto& fds = pool->dma_buf_fds();
        std::vector<void*> views(fds.size(), nullptr);
        for (std::size_t i = 0; i < fds.size(); ++i) {
            void* p = ::mmap(nullptr, buffer_size, PROT_READ | PROT_WRITE, MAP_SHARED, fds[i], 0);
            if (p == MAP_FAILED) { unmap(views, buffer_size); return nullptr; }
            views[i] = p;
        }

        auto* server = axon::detail::SidecarServer::create(service);
        if (!server) { unmap(views, buffer_size); return nullptr; }
        server->set_pool(fds, static_cast<std::uint32_t>(pool->generation()),
                         static_cast<std::uint64_t>(buffer_size), axon::kWireVersion,
                         static_cast<std::uint8_t>(backend));

        return std::unique_ptr<Producer>(
            new Producer(std::move(pool), std::move(views), server, buffer_size));
    }

    ~Producer() { unmap(views_, buffer_size_); delete server_; }

    // Deliver the pool FDs to any consumer that just connected (non-blocking).
    // Call once per publish loop iteration.
    void poll_new_consumers() { server_->accept_and_handshake(0); }

    // Writable host view of buffer i; nullptr if out of range.
    void* buffer(std::size_t i) noexcept { return i < views_.size() ? views_[i] : nullptr; }

    std::size_t   buffer_count()    const noexcept { return views_.size(); }
    std::size_t   buffer_size()     const noexcept { return buffer_size_; }
    std::uint32_t pool_generation() const noexcept {
        return static_cast<std::uint32_t>(pool_->generation());
    }
    int connected_count() const noexcept { return server_->connected_count(); }

    Producer(const Producer&) = delete;
    Producer& operator=(const Producer&) = delete;

private:
    Producer(std::unique_ptr<axon::TensorPool> pool, std::vector<void*> views,
             axon::detail::SidecarServer* server, std::size_t bsz)
        : pool_(std::move(pool)), views_(std::move(views)), server_(server), buffer_size_(bsz) {}

    static void unmap(std::vector<void*>& v, std::size_t sz) {
        for (void* p : v) if (p && p != MAP_FAILED) ::munmap(p, sz);
        v.clear();
    }

    std::unique_ptr<axon::TensorPool> pool_;
    std::vector<void*>                views_;
    axon::detail::SidecarServer*      server_;
    std::size_t                       buffer_size_;
};

// ---------------------------------------------------------------------------
// Consumer: connects the sidecar, receives the pool FDs once, and mmaps them
// read-only. For each descriptor that arrives, call payload(bo_handle, offset)
// to read the tensor straight from shared memory — zero copy.
// ---------------------------------------------------------------------------
class Consumer {
public:
    // Returns nullptr if the sidecar connect or handshake fails (producer down).
    static std::unique_ptr<Consumer> create(const std::string& service,
                                             int connect_timeout_ms = 10000) {
        auto* client = axon::detail::SidecarClient::create(service);
        if (!client || client->connect(connect_timeout_ms) != 0) { delete client; return nullptr; }

        axon::detail::PoolHandshakeHeader hdr{};
        std::vector<int> fds;
        if (client->recv_pool_handshake(&hdr, &fds, connect_timeout_ms) != 0) {
            delete client; return nullptr;
        }

        std::vector<void*> views(fds.size(), nullptr);
        for (std::size_t i = 0; i < fds.size(); ++i) {
            void* p = ::mmap(nullptr, hdr.buffer_size, PROT_READ, MAP_SHARED | MAP_POPULATE, fds[i], 0);
            views[i] = (p == MAP_FAILED) ? nullptr : p;
            ::close(fds[i]);   // mmap holds the mapping; the FD is no longer needed
        }

        return std::unique_ptr<Consumer>(
            new Consumer(client, std::move(views), hdr.buffer_size, hdr.pool_generation));
    }

    ~Consumer() {
        for (void* p : views_) if (p && p != MAP_FAILED) ::munmap(p, buffer_size_);
        delete client_;
    }

    // Bounds-checked, read-only base pointer for (bo_handle, offset); nullptr if
    // the handle is out of range or unmapped.
    const void* payload(std::uint64_t bo_handle, std::uint64_t offset) const noexcept {
        if (bo_handle >= views_.size() || !views_[bo_handle]) return nullptr;
        return static_cast<const char*>(views_[bo_handle]) + offset;
    }

    std::uint32_t pool_generation() const noexcept { return pool_generation_; }
    std::size_t   buffer_size()     const noexcept { return buffer_size_; }
    std::size_t   buffer_count()    const noexcept { return views_.size(); }

    Consumer(const Consumer&) = delete;
    Consumer& operator=(const Consumer&) = delete;

private:
    Consumer(axon::detail::SidecarClient* client, std::vector<void*> views,
             std::uint64_t bsz, std::uint32_t gen)
        : client_(client), views_(std::move(views)), buffer_size_(bsz), pool_generation_(gen) {}

    axon::detail::SidecarClient* client_;
    std::vector<void*>           views_;
    std::uint64_t                buffer_size_;
    std::uint32_t                pool_generation_;
};

}  // namespace axon_bridge
