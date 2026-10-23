// SPDX-License-Identifier: Apache-2.0
// iox2_bench — the host → host single-stream cell of bench.cpp over Eclipse iceoryx2
// (C++ bindings), its zero-copy publish/subscribe: the publisher loans a slice in shared
// memory, writes the payload once and sends it; the subscriber receives a reference to the
// same memory. Same rules as bench.cpp: t0 before the full-payload write, header [t0, seq] +
// tail [seq], busy-polling consumer, 20 warm-up frames, torn frames counted, not timed.
// Latest-value delivery like the other cells: subscriber buffer 1 with safe overflow.
//
//   iox2_bench <pub|sub> <bytes> <frames> <rate_hz>
#include "iox2/iceoryx2.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <thread>
#include <vector>

#include <sys/resource.h>

namespace {

std::uint64_t now_ns() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC_RAW, &t);
    return t.tv_sec * 1000000000ull + t.tv_nsec;
}

double cpu_seconds() {
    rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return ru.ru_utime.tv_sec + ru.ru_stime.tv_sec + (ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) * 1e-6;
}

void sleep_until_ns(std::uint64_t target) {
    const std::uint64_t now = now_ns();
    if (now >= target) return;
    const std::uint64_t d = target - now;
    timespec ts{static_cast<time_t>(d / 1000000000ull), static_cast<long>(d % 1000000000ull)};
    nanosleep(&ts, nullptr);
}

double percentile(std::vector<double> v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    const std::size_t rank = static_cast<std::size_t>(std::ceil(p / 100.0 * v.size()));
    return v[std::clamp<std::size_t>(rank, 1, v.size()) - 1];
}

auto open_service(iox2::Node<iox2::ServiceType::Ipc>& node, std::size_t subs) {
    return node.service_builder(iox2::ServiceName::create("axon_bench_iox2").value())
        .publish_subscribe<iox2::bb::Slice<std::uint8_t>>()
        .history_size(0)
        .subscriber_max_buffer_size(1)
        .enable_safe_overflow(true)  // latest value wins, like keep-last(1) and axon
        .max_subscribers(subs)
        .open_or_create()
        .value();
}

int run_pub(std::size_t bytes, int frames, int rate_hz) {
    auto node = iox2::NodeBuilder().create<iox2::ServiceType::Ipc>().value();
    auto service = open_service(node, 8);
    auto publisher = service.publisher_builder().initial_max_slice_len(bytes).create().value();
    // BENCH_SRC_OFFSET / BENCH_ADDR_LOG: as in bench.cpp (4 KiB aliasing diagnosis).
    const std::size_t src_off = std::getenv("BENCH_SRC_OFFSET") ? std::strtoul(std::getenv("BENCH_SRC_OFFSET"), nullptr, 10) : 0;
    std::vector<std::uint8_t> src_buf(bytes + src_off);
    std::uint8_t* const src = src_buf.data() + src_off;
    for (std::size_t i = 0; i < bytes; ++i) src[i] = static_cast<std::uint8_t>(i * 31 & 0xff);

    // Like the ROS 2 producers: wait for the subscriber, then 500 ms.
    for (int k = 0; k < 200 && service.dynamic_config().number_of_subscribers() < 1; ++k)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    const double cpu0 = cpu_seconds();
    const std::uint64_t w0 = now_ns();
    for (std::uint64_t seq = 1; seq <= static_cast<std::uint64_t>(frames); ++seq) {
        const std::uint64_t t0 = now_ns();
        auto sample = publisher.loan_slice_uninit(bytes).value();
        auto* dst = reinterpret_cast<std::uint8_t*>(sample.payload_mut().data());
        if (seq == 1 && std::getenv("BENCH_ADDR_LOG"))
            std::fprintf(stderr, "addr src%%4096=%zu dst%%4096=%zu\n", reinterpret_cast<std::uintptr_t>(src) % 4096,
                         reinterpret_cast<std::uintptr_t>(dst) % 4096);
        std::memcpy(dst, src, bytes);
        std::memcpy(dst, &t0, 8);
        std::memcpy(dst + 8, &seq, 8);
        std::memcpy(dst + bytes - 8, &seq, 8);
        iox2::send(iox2::assume_init(std::move(sample))).value();
        sleep_until_ns(t0 + 1000000000ull / rate_hz);
    }
    std::printf("{\"role\":\"pub\",\"transport\":\"iox2\",\"bytes\":%zu,\"gpu\":0,\"frames\":%d,\"cpu_s\":%.4f,\"wall_s\":%.4f}\n",
                bytes, frames, cpu_seconds() - cpu0, (now_ns() - w0) / 1e9);
    std::fflush(stdout);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    return 0;
}

int run_sub(std::size_t bytes, int frames, int rate_hz) {
    auto node = iox2::NodeBuilder().create<iox2::ServiceType::Ipc>().value();
    auto service = open_service(node, 8);
    auto subscriber = service.subscriber_builder().create().value();

    std::vector<double> lat;
    int seen = 0, torn = 0;
    std::uint64_t last = 0, first_seq = 0;
    const double deadline = 10.0 + frames * (2.0 / rate_hz);
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() < deadline &&
           last < static_cast<std::uint64_t>(frames)) {
        auto sample = subscriber.receive().value();  // busy poll, like bench.cpp
        if (!sample.has_value()) continue;
        const auto payload = sample->payload();
        const auto* p = reinterpret_cast<const std::uint8_t*>(payload.data());
        const std::uint64_t now = now_ns();
        std::uint64_t t0, head, tail;
        std::memcpy(&t0, p, 8);
        std::memcpy(&head, p + 8, 8);
        std::memcpy(&tail, p + bytes - 8, 8);
        if (head != tail) { ++torn; continue; }
        if (head <= last) continue;
        last = head;
        if (++seen == 1) first_seq = head;
        if (seen > 20) lat.push_back((now - t0) / 1000.0);
    }
    double mean = 0;
    for (double x : lat) mean += x;
    std::printf("{\"role\":\"sub\",\"transport\":\"iox2\",\"bytes\":%zu,\"gpu\":0,\"frames_sent\":%d,\"frames_seen\":%d,"
                "\"torn\":%d,\"n\":%zu,\"p50_us\":%.1f,\"p90_us\":%.1f,\"p99_us\":%.1f,\"max_us\":%.1f,\"mean_us\":%.1f,"
                "\"first_seq\":%llu}\n",
                bytes, frames, seen, torn, lat.size(), percentile(lat, 50), percentile(lat, 90), percentile(lat, 99),
                lat.empty() ? 0.0 : *std::max_element(lat.begin(), lat.end()), lat.empty() ? 0.0 : mean / lat.size(),
                static_cast<unsigned long long>(first_seq));
    std::fflush(stdout);
    return seen > 0 ? 0 : 3;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 5) {
        std::fprintf(stderr, "usage: iox2_bench <pub|sub> <bytes> <frames> <rate_hz>\n");
        return 1;
    }
    const std::string role = argv[1];
    const std::size_t bytes = std::strtoull(argv[2], nullptr, 10);
    const int frames = std::atoi(argv[3]), rate = std::atoi(argv[4]);
    if (bytes < 24 || frames < 21 || rate < 1) {
        std::fprintf(stderr, "bytes >= 24, frames > 20, rate >= 1\n");
        return 1;
    }
    iox2::set_log_level_from_env_or(iox2::LogLevel::Warn);
    return role == "pub" ? run_pub(bytes, frames, rate) : role == "sub" ? run_sub(bytes, frames, rate) : 1;
}
