// SPDX-License-Identifier: Apache-2.0
// axon — Metadata plane (internal): pluggable backend interface
//
// The "metadata plane" from the design doc §1.1. `MetadataChannel` is an
// abstract interface with two interchangeable backends — the design-doc promise
// that swapping in Iceoryx2 "changes only this file" made concrete:
//
//   - Seqlock  (default, always built): a single-slot, seqlock-protected
//              TensorDescriptor in a POSIX shared-memory object (§3.3). No
//              external dependency.
//   - Iceoryx2 (opt-in, -DAXON_WITH_ICEORYX2=ON): the real lock-free SHM queue,
//              publish_subscribe<TensorDescriptor>.
//
// Backend selection at runtime via the AXON_METADATA_BACKEND env var
// ("seqlock" | "iceoryx2"); default is iceoryx2 when compiled in, else seqlock.
//
// Single-producer / multi-consumer "latest value wins" — exactly what the RT
// consumer's latest_view() needs.

#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "axon/tensor_descriptor.h"

namespace axon::detail {

class MetadataChannel {
public:
    virtual ~MetadataChannel() = default;

    // Publish the latest descriptor. RT-safe after construction on the seqlock
    // backend: no syscalls, no allocation. Single writer assumed.
    virtual void publish(const TensorDescriptor& desc) noexcept = 0;

    // Read the latest descriptor. Returns true on a consistent read within
    // `max_retry` attempts and writes the retry count to *retries_out. Returns
    // false on no-data or persistent writer interleaving.
    virtual bool read_latest(TensorDescriptor* out, int max_retry,
                             int* retries_out) noexcept = 0;

    virtual bool has_data() const noexcept = 0;

    // Update notification (TensorSubscriber::wait_any). notify_word() is a 32-bit
    // counter in shared memory that the publisher bumps after every publish, usable as
    // a process-shared futex; waiters_word() is a flag a consumer sets before sleeping on
    // it, so the publisher only pays a FUTEX_WAKE syscall when someone may be asleep.
    // nullptr when the backend or the publisher's slot has no such word (Iceoryx2, or a
    // publisher built before these fields): wait_any then polls.
    virtual std::atomic<std::uint32_t>* notify_word() noexcept { return nullptr; }
    virtual std::atomic<std::uint32_t>* waiters_word() noexcept { return nullptr; }

    // Factories — pick the compiled/enabled backend.
    static MetadataChannel* create_publisher(const std::string& service_name);
    static MetadataChannel* create_subscriber(const std::string& service_name,
                                              int timeout_ms);
};

// Exposed for the seqlock backend and its unit test.
std::string metadata_shm_name(const std::string& service_name);

}  // namespace axon::detail
