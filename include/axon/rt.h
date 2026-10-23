// SPDX-License-Identifier: Apache-2.0
// axon — RT-Linux helpers: memory pinning, scheduling, page-fault prevention
//
// Call once before entering the RT loop. Inside the RT loop no syscalls are
// allowed.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace axon {

struct RtSetupConfig {
    int    sched_priority      = 80;          // SCHED_FIFO priority (1-99)
    bool   lock_all_memory     = true;        // mlockall(MCL_CURRENT|MCL_FUTURE|MCL_ONFAULT)
    bool   prefault_stack      = true;
    std::size_t prefault_heap_bytes = 1 * 1024 * 1024;
    bool   set_cpu_affinity    = false;       // When true, use cpu_id below
    int    cpu_id              = -1;
    bool   disable_malloc_trim = true;        // Disable glibc malloc trim (M_TRIM_THRESHOLD=-1)
    // >= 0: also hold a CPU wake-up latency limit (CpuLatencyHold) for the rest of the process,
    // on cpu_id when set_cpu_affinity is set, otherwise system-wide. -1: leave C-states alone.
    // A second rt_setup call replaces the first one's hold. On a later failure (e.g. SCHED_FIFO
    // denied) the hold stays taken, like the affinity and mlockall already applied.
    int    cpu_latency_us      = -1;
};

// Keeps CPUs out of deep idle states while it lives. A producer that sleeps between frames
// otherwise wakes on a core coming out of a deep C-state, at a lower clock: measured on a
// Ryzen 9800X3D, a 30 Hz producer wrote 16 MiB at 2.8-3.3 GHz instead of 5.2 GHz and its
// latency doubled (benchmarks/ros2_cpp, "The producer's clock decides low-rate host numbers").
//
//   cpu = -1  the whole system, through /dev/cpu_dma_latency. The kernel drops the request
//             when the fd closes, so it also ends if the process dies. Needs root by default,
//             or the group the udev rule in tools/udev/ grants.
//   cpu >= 0  that CPU only, through /sys/devices/system/cpu/cpuN/power/pm_qos_resume_latency_us.
//             Cheaper on power for a pinned producer. Needs root or the udev rule. The old value
//             is restored when the hold is destroyed; a process that dies without unwinding
//             leaves the limit set (sysfs is not fd-scoped). The attribute holds one request per
//             CPU, not an aggregate: give each CPU one owner, and nest holds on it only LIFO.
//
// A hold belongs to the process that acquired it: a fork()ed child neither restores nor closes
// it. Not thread-safe: acquire and release a given CPU's holds from one thread.
//
// max_latency_us is the deepest idle state's exit latency allowed. 0 permits only polling idle:
// the core spins, at the most power. 1 permits the shallowest halt state (C1). On that machine
// (one PC, 2 runs per setting) 1 kept 4.6-5.0 GHz, against 5.2 GHz for 0 and 3.0-3.6 GHz for
// 20 (C2). So 1 is the usual choice. See /sys/devices/system/cpu/cpu0/cpuidle/state*/{name,latency} for a machine.
class CpuLatencyHold {
public:
    // std::nullopt on failure, errno set: EACCES without the rights above, EINVAL for a negative
    // latency or cpu < -1, ENOENT for an unknown or offline CPU, EIO for an unreadable value.
    static std::optional<CpuLatencyHold> acquire(int max_latency_us = 0, int cpu = -1);

    CpuLatencyHold(CpuLatencyHold&& o) noexcept;
    CpuLatencyHold& operator=(CpuLatencyHold&& o) noexcept;
    CpuLatencyHold(const CpuLatencyHold&) = delete;
    CpuLatencyHold& operator=(const CpuLatencyHold&) = delete;
    ~CpuLatencyHold();

    int cpu() const noexcept { return cpu_; }
    int max_latency_us() const noexcept { return us_; }

private:
    CpuLatencyHold(int fd, int cpu, int us, std::string path, std::string restore) noexcept;
    void release() noexcept;
    int fd_ = -1;          // /dev/cpu_dma_latency (cpu == -1)
    int cpu_ = -1;
    int us_ = 0;
    int owner_pid_ = 0;    // only the acquiring process releases
    std::string path_;     // sysfs attribute (cpu >= 0)
    std::string restore_;  // its previous value
};

// The logical CPUs that share cpu's physical core (its SMT siblings, cpu included), from
// sysfs. Use it to put a producer and an RT consumer on different physical cores. Empty when
// the CPU is offline or unknown.
std::vector<int> rt_core_siblings(int cpu);

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

}  // namespace axon
