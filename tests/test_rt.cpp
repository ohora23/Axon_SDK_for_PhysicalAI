// SPDX-License-Identifier: Apache-2.0
// RT helpers — clock monotonicity, prefault, and best-effort setup.
//
// Tests run unprivileged, so SCHED_FIFO promotion and mlockall may be denied;
// those are checked as "does not crash / returns a defined result", not success.

#include "axon/rt.h"
#include "axon_test.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

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

static std::string read_attr(int cpu) {
    std::ifstream f("/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/power/pm_qos_resume_latency_us");
    std::string v;
    std::getline(f, v);
    return v;
}

AXON_TEST(latency_hold_rejects_bad_args) {
    errno = 0;
    CHECK(!CpuLatencyHold::acquire(-1, -1));
    CHECK(errno == EINVAL);
    CHECK(!CpuLatencyHold::acquire(0, -2));
}

AXON_TEST(core_siblings_include_self) {
    const auto s = rt_core_siblings(0);  // cpu0 cannot be offlined
    REQUIRE(!s.empty());
    CHECK(std::find(s.begin(), s.end(), 0) != s.end());
    CHECK(rt_core_siblings(-1).empty());
}

AXON_TEST(latency_hold_system_wide) {
    // Needs root or group plugdev; without them it must fail cleanly with EACCES.
    errno = 0;
    auto h = CpuLatencyHold::acquire(0, -1);
    if (!h) {
        CHECK(errno == EACCES || errno == EPERM);
        std::printf("  (skipped: /dev/cpu_dma_latency not writable)\n");
        return;
    }
    CHECK(h->cpu() == -1);
    CHECK(h->max_latency_us() == 0);
}

AXON_TEST(latency_hold_per_cpu_sets_and_restores) {
    const std::string before = read_attr(0);
    REQUIRE(!before.empty());
    errno = 0;
    auto h = CpuLatencyHold::acquire(5, 0);
    if (!h) {
        CHECK(errno == EACCES || errno == EPERM);
        std::printf("  (skipped: per-CPU PM QoS not writable; see tools/udev/)\n");
        return;
    }
    CHECK(read_attr(0) == "5");
    {
        CpuLatencyHold moved = std::move(*h);  // the moved-from hold must not restore
        h.reset();
        CHECK(read_attr(0) == "5");
        auto zero = CpuLatencyHold::acquire(0, 0);  // 0 µs is written as "n/a"
        REQUIRE(zero.has_value());
        CHECK(read_attr(0) == "n/a");
    }  // zero restores "5", then moved restores the original
    CHECK(read_attr(0) == before);
}

AXON_TEST_MAIN("rt")
