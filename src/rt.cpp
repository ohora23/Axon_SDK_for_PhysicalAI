// SPDX-License-Identifier: Apache-2.0
// dczc — RT-Linux helpers (design doc §3.2)

#include "dczc/rt.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <malloc.h>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <unistd.h>

namespace dczc {

namespace {

// Touch every page of a region so it is wired by mlockall before the RT loop.
void touch_pages(volatile char* p, std::size_t n) {
    const long page = sysconf(_SC_PAGESIZE) > 0 ? sysconf(_SC_PAGESIZE) : 4096;
    for (std::size_t i = 0; i < n; i += static_cast<std::size_t>(page)) {
        p[i] = 0;
    }
    if (n > 0) p[n - 1] = 0;
}

}  // namespace

int rt_setup_memory_and_sched(const RtSetupConfig& cfg) {
    if (cfg.disable_malloc_trim) {
        // Keep freed memory resident — avoids brk/munmap inside the RT loop.
        mallopt(M_TRIM_THRESHOLD, -1);
        mallopt(M_MMAP_MAX, 0);
    }

    if (cfg.lock_all_memory) {
        int flags = MCL_CURRENT | MCL_FUTURE;
#ifdef MCL_ONFAULT
        flags |= MCL_ONFAULT;
#endif
        if (mlockall(flags) < 0) {
            // MCL_ONFAULT may be unsupported on older kernels — retry without it.
            if (errno == EINVAL && mlockall(MCL_CURRENT | MCL_FUTURE) < 0) {
                return -1;
            } else if (errno != 0 && (flags == (MCL_CURRENT | MCL_FUTURE))) {
                return -1;
            }
        }
    }

    if (cfg.prefault_stack) {
        volatile char stack_prefault[64 * 1024];
        touch_pages(stack_prefault, sizeof(stack_prefault));
    }

    if (cfg.prefault_heap_bytes > 0) {
        void* heap = std::malloc(cfg.prefault_heap_bytes);
        if (!heap) return -1;
        touch_pages(static_cast<volatile char*>(heap), cfg.prefault_heap_bytes);
        std::free(heap);  // stays resident thanks to M_TRIM_THRESHOLD = -1
    }

    if (cfg.set_cpu_affinity && cfg.cpu_id >= 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(static_cast<unsigned>(cfg.cpu_id), &set);
        if (sched_setaffinity(0, sizeof(set), &set) < 0) return -1;
    }

    if (cfg.sched_priority > 0) {
        struct sched_param param {};
        param.sched_priority = cfg.sched_priority;
        if (sched_setscheduler(0, SCHED_FIFO, &param) < 0) {
            // Lacking CAP_SYS_NICE is the common non-fatal case; report via errno
            // and let the caller decide (tests run unprivileged).
            return -1;
        }
    }

    return 0;
}

int rt_prefault_dma_buf_view(void* mapped_view, std::size_t size) noexcept {
    if (!mapped_view || size == 0) {
        errno = EINVAL;
        return -1;
    }
    // Read-touch every page so the first RT access never faults.
    volatile const char* p = static_cast<volatile const char*>(mapped_view);
    const long page = sysconf(_SC_PAGESIZE) > 0 ? sysconf(_SC_PAGESIZE) : 4096;
    volatile char sink = 0;
    for (std::size_t i = 0; i < size; i += static_cast<std::size_t>(page)) {
        sink = static_cast<char>(sink ^ p[i]);
    }
    if (size > 0) sink = static_cast<char>(sink ^ p[size - 1]);
    (void)sink;
    return 0;
}

std::uint64_t rt_now_ns() noexcept {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<std::uint64_t>(ts.tv_nsec);
}

}  // namespace dczc
