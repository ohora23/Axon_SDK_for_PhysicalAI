// SPDX-License-Identifier: Apache-2.0
// dczc — RT-Linux helpers: memory pinning, scheduling, page-fault prevention
//
// Call once before entering the RT loop. Inside the RT loop no syscalls are
// allowed.

#pragma once

#include <cstddef>
#include <cstdint>

namespace dczc {

struct RtSetupConfig {
    int    sched_priority      = 80;          // SCHED_FIFO priority (1-99)
    bool   lock_all_memory     = true;        // mlockall(MCL_CURRENT|MCL_FUTURE|MCL_ONFAULT)
    bool   prefault_stack      = true;
    std::size_t prefault_heap_bytes = 1 * 1024 * 1024;
    bool   set_cpu_affinity    = false;       // When true, use cpu_id below
    int    cpu_id              = -1;
    bool   disable_malloc_trim = true;        // Disable glibc malloc trim (M_TRIM_THRESHOLD=-1)
};

// Initialize the RT process. On failure returns a negative value with errno
// set; the caller decides whether to log/exit.
//
// Guarantees:
//   - All current pages are wired (mlockall MCL_CURRENT)
//   - Future mappings are wired with first-touch fault handling
//     (MCL_FUTURE | MCL_ONFAULT)
//   - Stack and heap are pre-faulted → 0 page faults inside the RT loop
//   - Optional CPU pinning + SCHED_FIFO promotion
int rt_setup_memory_and_sched(const RtSetupConfig& cfg = {});

// Pre-fault a dma-buf-backed view (read-touch). Call right after dma-buf
// attach and before the RT loop starts to avoid first-access page faults.
int rt_prefault_dma_buf_view(void* mapped_view, std::size_t size) noexcept;

// Monotonic time in nanoseconds, used for staleness measurement.
// CLOCK_MONOTONIC_RAW via vDSO — RT-safe.
std::uint64_t rt_now_ns() noexcept;

}  // namespace dczc
