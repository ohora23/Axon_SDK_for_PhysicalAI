// SPDX-License-Identifier: Apache-2.0
// dczc — Metadata plane (internal)
//
// The "metadata plane" from the design doc §1.1. In the final build this is
// Iceoryx2's lock-free SHM queue; until Iceoryx2 is wired in (CMakeLists keeps
// find_package(iceoryx2) commented out until after the spike), this provides an
// API-compatible stand-in: a single-slot, seqlock-protected TensorDescriptor in
// a POSIX shared-memory object.
//
// Single-producer / multi-consumer "latest value wins" semantics — exactly what
// the RT consumer's latest_view() needs (design doc §3.3). When Iceoryx2 lands,
// only this file changes; TensorPublisher / TensorSubscriber keep their shape.
//
// DEFERRED: Iceoryx2 multi-slot ring backend (replaces the single seqlock slot).

#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include "dczc/tensor_descriptor.h"

namespace dczc::detail {

// Shared-memory layout. The guard is an independent seqlock counter (odd while a
// write is in progress) — distinct from TensorDescriptor::seqno, which is the
// application-level data version.
struct alignas(64) MetadataSlot {
    std::atomic<std::uint64_t> guard;   // seqlock guard: odd = write in progress
    std::uint32_t              wire_version;
    std::uint32_t              ready;    // 0 until the first publish lands
    TensorDescriptor           desc;     // POD payload
};

// Derive the POSIX shm object name for a service (sanitized, leading '/').
std::string metadata_shm_name(const std::string& service_name);

class MetadataChannel {
public:
    // Publisher side: create (or truncate) the shm object and zero the slot.
    static MetadataChannel* create_publisher(const std::string& service_name);
    // Subscriber side: open an existing shm object read/write (for the guard).
    static MetadataChannel* create_subscriber(const std::string& service_name,
                                              int timeout_ms);
    ~MetadataChannel();

    // Seqlock write of the latest descriptor. RT-safe after construction: no
    // syscalls, no allocation. Single writer assumed.
    void publish(const TensorDescriptor& desc) noexcept;

    // Seqlock read of the latest descriptor. Returns true on a consistent read
    // within `max_retry` attempts; writes the retry count to *retries_out.
    // Returns false (with the partial value left untouched) on no-data or when
    // the writer kept interleaving past the retry cap.
    bool read_latest(TensorDescriptor* out, int max_retry,
                     int* retries_out) noexcept;

    bool has_data() const noexcept;

    MetadataChannel(const MetadataChannel&) = delete;
    MetadataChannel& operator=(const MetadataChannel&) = delete;

private:
    MetadataChannel();
    MetadataSlot* slot_;
    std::size_t   map_len_;
    std::string   shm_name_;
    bool          owns_;        // publisher unlinks on destroy
};

}  // namespace dczc::detail
