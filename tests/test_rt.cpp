// SPDX-License-Identifier: Apache-2.0
// RT helpers — clock monotonicity, prefault, and best-effort setup.
//
// Tests run unprivileged, so SCHED_FIFO promotion and mlockall may be denied;
// those are checked as "does not crash / returns a defined result", not success.

#include "axon/rt.h"
#include "axon_test.h"

#include <cstdlib>
#include <cstring>

using namespace axon;

AXON_TEST(now_ns_monotonic) {
    std::uint64_t a = rt_now_ns();
    std::uint64_t b = rt_now_ns();
    CHECK(a != 0);
    CHECK(b >= a);
}

AXON_TEST(prefault_view) {
    const std::size_t n = 256 * 1024;
    void* p = std::malloc(n);
    REQUIRE(p != nullptr);
    std::memset(p, 0xab, n);
    CHECK(rt_prefault_dma_buf_view(p, n) == 0);
    CHECK(rt_prefault_dma_buf_view(nullptr, n) == -1);  // rejects null
    CHECK(rt_prefault_dma_buf_view(p, 0) == -1);        // rejects zero size
    std::free(p);
}

AXON_TEST(setup_does_not_crash) {
    // Ask for no privileged actions so this passes unprivileged in CI.
    RtSetupConfig cfg;
    cfg.sched_priority = 0;       // skip SCHED_FIFO
    cfg.lock_all_memory = false;  // skip mlockall
    cfg.prefault_stack = true;
    cfg.prefault_heap_bytes = 64 * 1024;
    cfg.set_cpu_affinity = false;
    cfg.disable_malloc_trim = true;
    CHECK(rt_setup_memory_and_sched(cfg) == 0);
}

AXON_TEST_MAIN("rt")
