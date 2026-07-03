// SPDX-License-Identifier: Apache-2.0
// dczc — Metadata plane (internal): pluggable backend interface
//
// The "metadata plane" from the design doc §1.1. `MetadataChannel` is an
// abstract interface with two interchangeable backends — the design-doc promise
// that swapping in Iceoryx2 "changes only this file" made concrete:
//
//   - Seqlock  (default, always built): a single-slot, seqlock-protected
//              TensorDescriptor in a POSIX shared-memory object (§3.3). No
//              external dependency.
//   - Iceoryx2 (opt-in, -DDCZC_WITH_ICEORYX2=ON): the real lock-free SHM queue,
//              publish_subscribe<TensorDescriptor>.
//
// Backend selection at runtime via the DCZC_METADATA_BACKEND env var
// ("seqlock" | "iceoryx2"); default is iceoryx2 when compiled in, else seqlock.
//
// Single-producer / multi-consumer "latest value wins" — exactly what the RT
// consumer's latest_view() needs.

#pragma once

#include <cstdint>
#include <string>

#include "dczc/tensor_descriptor.h"

namespace dczc::detail {

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

    // Factories — pick the compiled/enabled backend.
    static MetadataChannel* create_publisher(const std::string& service_name);
    static MetadataChannel* create_subscriber(const std::string& service_name,
                                              int timeout_ms);
};

// Exposed for the seqlock backend and its unit test.
std::string metadata_shm_name(const std::string& service_name);

}  // namespace dczc::detail
