// SPDX-License-Identifier: Apache-2.0
// dczc closed-loop demo + measurement harness
//
// Exercises the *public* dczc API the way an application would, with no camera
// and no accelerator (Custom memfd pool), and reports the metrics the design doc
// calls for:
//   - end-to-end staleness distribution         (design doc §5 — bounded staleness)
//   - seqlock retry distribution                (design doc §8.1)
//   - fallback invocations                       (design doc §3.5)
//
// Topology: the parent process is the producer; a forked child is the RT
// consumer. The producer streams N frames at a target rate, stamping each frame
// with its seqno and a synthetic "inference result". The consumer RT-reads the
// latest view every tick, records staleness, and verifies the payload came
// through the shared dma-buf uncopied.
//
// Usage: dczc_demo [--frames N] [--rate-hz R] [--buffers B] [--quiet]

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <ctime>
#include <sys/wait.h>
#include <unistd.h>

#include "dczc/pool.h"
#include "dczc/publisher.h"
#include "dczc/rt.h"
#include "dczc/subscriber.h"

using namespace dczc;

namespace {

struct Config {
    int frames = 300;
    int rate_hz = 200;
    int buffers = 8;
    bool quiet = false;
};

constexpr std::size_t kBufBytes = 256 * 1024;
const char* kService = "demo/tensor_stream";

void sleep_ns(std::uint64_t ns) {
    struct timespec ts {static_cast<time_t>(ns / 1000000000ULL),
                        static_cast<long>(ns % 1000000000ULL)};
    nanosleep(&ts, nullptr);
}

// ---- percentile stats over a sample vector (non-RT reporting) ----
struct Stats {
    std::uint64_t n = 0, min = 0, max = 0, p50 = 0, p90 = 0, p99 = 0;
    double mean = 0.0;
};

Stats summarize(std::vector<std::uint64_t> v) {
    Stats s;
    if (v.empty()) return s;
    std::sort(v.begin(), v.end());
    s.n = v.size();
    s.min = v.front();
    s.max = v.back();
    auto pct = [&](double p) {
        std::size_t idx = static_cast<std::size_t>(p * (double)(v.size() - 1) + 0.5);
        return v[idx];
    };
    s.p50 = pct(0.50);
    s.p90 = pct(0.90);
    s.p99 = pct(0.99);
    long double sum = 0;
    for (auto x : v) sum += x;
    s.mean = static_cast<double>(sum / (long double)v.size());
    return s;
}

void print_us(const char* label, const Stats& s) {
    std::fprintf(stderr,
        "  %-22s n=%-6lu  min=%.1f  mean=%.1f  p50=%.1f  p90=%.1f  p99=%.1f  max=%.1f  (µs)\n",
        label, (unsigned long)s.n,
        static_cast<double>(s.min) / 1e3, s.mean / 1e3,
        static_cast<double>(s.p50) / 1e3, static_cast<double>(s.p90) / 1e3,
        static_cast<double>(s.p99) / 1e3, static_cast<double>(s.max) / 1e3);
}

// ---- consumer (child) ----
int run_consumer(const Config& cfg) {
    // Best-effort RT setup — tolerated if unprivileged (no CAP_SYS_NICE).
    RtSetupConfig rt;
    rt.sched_priority = 0;       // don't require SCHED_FIFO in the demo
    rt.lock_all_memory = false;  // mlockall often needs privilege
    rt.prefault_heap_bytes = 256 * 1024;
    rt_setup_memory_and_sched(rt);

    auto sub = TensorSubscriber::create(kService);
    if (!sub) { std::fprintf(stderr, "consumer: create failed\n"); return 10; }
    sub->set_fallback_policy(FallbackPolicy::LastKnownGood);
    if (sub->wait_handshake(5000) != 0) {
        std::fprintf(stderr, "consumer: handshake failed\n");
        return 11;
    }

    std::vector<std::uint64_t> staleness;
    std::vector<std::uint64_t> retries;
    staleness.reserve(static_cast<std::size_t>(cfg.frames));
    retries.reserve(static_cast<std::size_t>(cfg.frames));

    const std::uint64_t tick_ns = 1000000000ULL / static_cast<std::uint64_t>(cfg.rate_hz);
    std::uint64_t last_seqno = 0;
    std::uint64_t payload_errors = 0;
    std::uint64_t distinct = 0;
    const std::uint64_t deadline = rt_now_ns() +
        (static_cast<std::uint64_t>(cfg.frames) * tick_ns * 4) + 2000000000ULL;

    for (;;) {
        auto v = sub->latest_view(8);
        if (v && v->seqno != last_seqno) {
            last_seqno = v->seqno;
            ++distinct;
            staleness.push_back(v->staleness_ns);
            retries.push_back(static_cast<std::uint64_t>(v->seqlock_retries));

            // Zero-copy payload check: the producer stamps the seqno at offset 0.
            if (v->data) {
                std::uint64_t stamped = 0;
                std::memcpy(&stamped, v->data, sizeof(stamped));
                if (stamped != v->seqno) ++payload_errors;
            }
            if (v->seqno >= static_cast<std::uint64_t>(cfg.frames)) break;
        }
        if (rt_now_ns() > deadline) break;
        sleep_ns(tick_ns);
    }

    Stats st = summarize(staleness);
    Stats rt_st = summarize(retries);

    if (!cfg.quiet) {
        std::fprintf(stderr, "\n─────────── dczc demo — measurement report ───────────\n");
        std::fprintf(stderr, "  frames streamed:       %d @ %d Hz\n", cfg.frames, cfg.rate_hz);
        std::fprintf(stderr, "  distinct frames seen:  %lu\n", (unsigned long)distinct);
        std::fprintf(stderr, "  final seqno observed:  %lu\n", (unsigned long)last_seqno);
        std::fprintf(stderr, "  payload errors:        %lu  (must be 0 — zero-copy integrity)\n",
                     (unsigned long)payload_errors);
        std::fprintf(stderr, "  fallback invocations:  %lu\n",
                     (unsigned long)sub->fallback_invocation_count());
        print_us("end-to-end staleness", st);
        std::fprintf(stderr,
            "  %-22s n=%-6lu  min=%lu  p50=%lu  p99=%lu  max=%lu  (retries)\n",
            "seqlock retries", (unsigned long)rt_st.n, (unsigned long)rt_st.min,
            (unsigned long)rt_st.p50, (unsigned long)rt_st.p99, (unsigned long)rt_st.max);
        std::fprintf(stderr, "──────────────────────────────────────────────────────\n");
    }

    if (payload_errors != 0) return 12;
    if (last_seqno < static_cast<std::uint64_t>(cfg.frames)) {
        std::fprintf(stderr, "consumer: did not observe the final frame (%lu < %d)\n",
                     (unsigned long)last_seqno, cfg.frames);
        return 13;
    }
    return 0;
}

// ---- producer (parent) ----
int run_producer(const Config& cfg, pid_t child) {
    auto pool = TensorPool::create(
        TensorPoolConfig{static_cast<std::size_t>(cfg.buffers), kBufBytes,
                         PoolBackend::Custom, nullptr});
    if (!pool) { std::fprintf(stderr, "producer: pool failed\n"); return 20; }

    auto pub = TensorPublisher::create(kService, *pool);
    if (!pub) { std::fprintf(stderr, "producer: publisher failed\n"); return 21; }

    // Block until the consumer connects, then bulk-deliver the pool FDs once.
    pub->handshake_pool();

    const std::uint64_t tick_ns = 1000000000ULL / static_cast<std::uint64_t>(cfg.rate_hz);
    for (std::uint64_t s = 1; s <= static_cast<std::uint64_t>(cfg.frames); ++s) {
        std::uint64_t t0 = rt_now_ns();

        AcquiredDescriptor a = pub->acquire_descriptor();
        if (a.buffer_index < 0 || !a.host_view) {
            std::fprintf(stderr, "producer: acquire failed at seq %lu\n", (unsigned long)s);
            break;
        }
        // Synthetic inference output: seqno at offset 0, ramp fill after it.
        std::memcpy(a.host_view, &s, sizeof(s));
        std::memset(static_cast<char*>(a.host_view) + sizeof(s),
                    static_cast<int>(s & 0xff), 1024);

        a.desc->rank = 4;
        a.desc->shape[0] = 1; a.desc->shape[1] = 3;
        a.desc->shape[2] = 224; a.desc->shape[3] = 224;
        a.desc->dtype = DType::U8;
        a.desc->offset = 0;
        a.desc->size = kBufBytes;
        a.desc->sync_fence_kind = SyncFenceKind::None;
        a.desc->capture_ts_ns = t0;

        pub->publish(std::move(a));

        std::uint64_t elapsed = rt_now_ns() - t0;
        if (elapsed < tick_ns) sleep_ns(tick_ns - elapsed);
    }

    int status = 0;
    waitpid(child, &status, 0);
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    std::fprintf(stderr, "producer: consumer terminated abnormally\n");
    return 22;
}

Config parse_args(int argc, char** argv) {
    Config c;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> int { return (i + 1 < argc) ? std::atoi(argv[++i]) : 0; };
        if (a == "--frames") c.frames = next();
        else if (a == "--rate-hz") c.rate_hz = next();
        else if (a == "--buffers") c.buffers = next();
        else if (a == "--quiet") c.quiet = true;
    }
    if (c.frames < 1) c.frames = 1;
    if (c.rate_hz < 1) c.rate_hz = 1;
    if (c.buffers < 2) c.buffers = 2;
    return c;
}

}  // namespace

int main(int argc, char** argv) {
    Config cfg = parse_args(argc, argv);

    pid_t pid = fork();
    if (pid < 0) { std::perror("fork"); return 1; }
    if (pid == 0) {
        _exit(run_consumer(cfg));  // child: no parent-destructor cleanup
    }
    return run_producer(cfg, pid);
}
