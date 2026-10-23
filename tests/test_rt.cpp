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
#include <ctime>
#include <fstream>
#include <string>

#include <sys/wait.h>
#include <unistd.h>

using namespace axon;

AXON_TEST(now_ns_monotonic) {
    std::uint64_t a = rt_now_ns();
    std::uint64_t b = rt_now_ns();
    CHECK(a != 0);
    CHECK(b >= a);
}

// Sleeps for most of the wait and spins only the tail: reaches the deadline, and uses far less CPU
// time than wall time. A past deadline returns at once; spin_us = 0 is a sleep. Only the upper CPU
// bound is checked: preemption during the spin (a loaded CI host) lowers thread CPU time.
AXON_TEST(sleep_until_prewarmed) {
    auto cpu_ns = [] {
        struct timespec ts;
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
        return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ULL + static_cast<std::uint64_t>(ts.tv_nsec);
    };
    const std::uint64_t t0 = rt_now_ns(), c0 = cpu_ns();
    const std::uint64_t deadline = t0 + 30'000'000;  // 30 ms, 2 ms of it spun
    const std::uint64_t r = rt_sleep_until_prewarmed(deadline, 2000);
    const std::uint64_t cpu = cpu_ns() - c0;
    CHECK(r >= deadline);
    CHECK(rt_now_ns() >= deadline);
    CHECK(cpu < 15'000'000);   // most of the 30 ms was slept, not spun
    const std::uint64_t past = rt_now_ns();
    CHECK(rt_sleep_until_prewarmed(past - 1000, 2000) >= past);  // already passed: returns at once
    CHECK(rt_now_ns() - past < 1'000'000);
    const std::uint64_t d2 = rt_now_ns() + 5'000'000;
    CHECK(rt_sleep_until_prewarmed(d2, 0) >= d2);                // plain sleep
    CHECK(rt_sleep_until_prewarmed(1000, 4'000'000) >= 1000);    // spin longer than the deadline value
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

AXON_TEST(latency_hold_unknown_cpu_is_enoent) {
    errno = 0;
    CHECK(!CpuLatencyHold::acquire(1, 9999));
    CHECK(errno == ENOENT);
}

AXON_TEST(latency_hold_offline_cpu_is_enodev) {
    for (int cpu = 1; cpu < 256; ++cpu) {
        std::ifstream f("/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/online");
        std::string v;
        if (!f || !std::getline(f, v) || v != "0") continue;
        errno = 0;
        CHECK(!CpuLatencyHold::acquire(1, cpu));
        CHECK(errno == ENODEV);
        return;
    }
    std::printf("  (skipped: no offline CPU)\n");
}

AXON_TEST(latency_hold_forked_child_does_not_restore) {
    const std::string before = read_attr(0);
    auto h = CpuLatencyHold::acquire(7, 0);
    if (!h) {
        std::printf("  (skipped: per-CPU PM QoS not writable)\n");
        return;
    }
    const pid_t pid = fork();
    if (pid == 0) {
        h.reset();  // the child's copy: must leave the parent's limit alone
        std::_Exit(read_attr(0) == "7" ? 0 : 1);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    CHECK(read_attr(0) == "7");
    h.reset();
    CHECK(read_attr(0) == before);
}

AXON_TEST(setup_twice_restores_the_original) {
    // rt_setup's hold lives until process exit, so run it in a child that exits normally and
    // check from outside that the very first value came back (not the first call's limit).
    const std::string before = read_attr(0);
    if (!CpuLatencyHold::acquire(1, 0)) {
        std::printf("  (skipped: per-CPU PM QoS not writable)\n");
        return;
    }  // (released right away)
    const pid_t pid = fork();
    if (pid == 0) {
        RtSetupConfig cfg;
        cfg.sched_priority = 0;
        cfg.lock_all_memory = false;
        cfg.prefault_heap_bytes = 0;
        cfg.set_cpu_affinity = true;
        cfg.cpu_id = 0;
        cfg.cpu_latency_us = 5;
        if (rt_setup_memory_and_sched(cfg) != 0 || read_attr(0) != "5") std::_Exit(2);
        cfg.cpu_latency_us = 3;
        if (rt_setup_memory_and_sched(cfg) != 0 || read_attr(0) != "3") std::_Exit(3);
        std::exit(0);  // runs static destructors: the hold restores
    }
    int st = 0;
    waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    CHECK(read_attr(0) == before);
}

AXON_TEST_MAIN("rt")
