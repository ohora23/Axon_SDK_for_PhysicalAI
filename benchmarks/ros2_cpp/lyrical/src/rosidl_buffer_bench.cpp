// SPDX-License-Identifier: Apache-2.0
// rosidl_buffer_bench — the GPU -> GPU single-stream cell of ../../src/bench.cpp, over the
// ROS 2 Lyrical rosidl::Buffer CUDA backend (ros2/rosidl_buffer_backends): a
// sensor_msgs/Image whose `data` is a CUDA-backed rosidl::Buffer, shared through CUDA VMM
// and synchronized with CUDA IPC write/read events. Same rules as bench.cpp: t0 before the
// full-payload write, header [t0, seq] + tail [seq], busy-polling consumer, clock stops once
// the consumer stream has the frame and its header/tail read back. A frame the backend
// delivered on the CPU fallback path (a promoted buffer) is counted, not timed.
//
//   rosidl_buffer_bench <pub|sub> <bytes> <frames> <rate_hz>
#include <cuda_runtime_api.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <thread>
#include <vector>

#include "cuda_buffer/cuda_buffer_api.hpp"
#include "rclcpp/rclcpp.hpp"
#include "sensor_msgs/msg/image.hpp"

namespace {

using Image = sensor_msgs::msg::Image;
constexpr std::uint32_t kWidth = 256, kChannels = 4;  // payload as a 256-wide RGBA image

std::uint64_t now_ns() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC_RAW, &t);
    return t.tv_sec * 1000000000ull + t.tv_nsec;
}

void check(cudaError_t e, const char* what) {
    if (e != cudaSuccess) {
        std::fprintf(stderr, "CUDA error: %s: %s\n", what, cudaGetErrorString(e));
        std::exit(1);
    }
}

rclcpp::QoS qos() { return rclcpp::QoS(rclcpp::KeepLast(1)).best_effort().durability_volatile(); }

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

int run_pub(std::size_t bytes, int frames, int rate_hz) {
    auto node = rclcpp::Node::make_shared("rosidl_buffer_pub");
    check(cudaSetDevice(0), "cudaSetDevice");
    cudaStream_t stream = nullptr;
    check(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking), "cudaStreamCreate");
    void* src = nullptr;  // the "inference output" already on the GPU
    check(cudaMalloc(&src, bytes), "cudaMalloc");
    check(cudaMemset(src, 0x5a, bytes), "cudaMemset");
    constexpr int kSlots = 16;  // pinned header/tail staging, as in cudaipc_bench
    std::uint64_t* staging = nullptr;
    check(cudaMallocHost(reinterpret_cast<void**>(&staging), kSlots * 3 * sizeof(std::uint64_t)), "cudaMallocHost");

    rclcpp::PublisherOptions opts;
    opts.use_intra_process_comm = rclcpp::IntraProcessSetting::Disable;
    auto pub = node->create_publisher<Image>("/axon_bench_rosidl_buffer", qos(), opts);
    for (int k = 0; k < 200 && pub->get_subscription_count() < 1; ++k)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    const std::uint32_t rows = static_cast<std::uint32_t>(bytes / (kWidth * kChannels));
    for (int s = 1; s <= frames; ++s) {
        const std::uint64_t t0 = now_ns();
        Image msg;
        msg.height = rows;
        msg.width = kWidth;
        msg.encoding = "rgba8";
        msg.step = kWidth * kChannels;
        msg.data = cuda_buffer_backend::allocate_buffer(bytes);
        {
            auto wh = cuda_buffer_backend::from_output_buffer(msg.data, stream);
            auto* dst = static_cast<std::uint8_t*>(wh.get_ptr());
            std::uint64_t* h = staging + (s % kSlots) * 3;  // [t0, seq, seq]
            h[0] = t0;
            h[1] = h[2] = static_cast<std::uint64_t>(s);
            check(cudaMemcpyAsync(dst, src, bytes, cudaMemcpyDeviceToDevice, stream), "DtoD payload");
            check(cudaMemcpyAsync(dst, h, 16, cudaMemcpyHostToDevice, stream), "HtoD header");
            check(cudaMemcpyAsync(dst + bytes - 8, h + 2, 8, cudaMemcpyHostToDevice, stream), "HtoD tail");
        }  // WriteHandle destructor records the write event on `stream`
        pub->publish(msg);
        sleep_until_ns(t0 + 1000000000ull / rate_hz);
    }
    check(cudaStreamSynchronize(stream), "cudaStreamSynchronize");
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    return 0;
}

int run_sub(std::size_t bytes, int frames, int rate_hz) {
    auto node = rclcpp::Node::make_shared("rosidl_buffer_sub");
    check(cudaSetDevice(0), "cudaSetDevice");
    cudaStream_t stream = nullptr;
    check(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking), "cudaStreamCreate");
    std::uint64_t* host = nullptr;
    check(cudaMallocHost(reinterpret_cast<void**>(&host), 3 * sizeof(std::uint64_t)), "cudaMallocHost");

    rclcpp::SubscriptionOptions opts;
    opts.use_intra_process_comm = rclcpp::IntraProcessSetting::Disable;
    opts.acceptable_buffer_backends = "cuda";  // opt in; without it the subscription takes the CPU path
    auto sub = node->create_subscription<Image>("/axon_bench_rosidl_buffer", qos(), [](const Image&) {}, opts);

    std::vector<double> lat;
    int seen = 0, torn = 0, fallback = 0;
    std::uint64_t last = 0;
    const double deadline = 10.0 + frames * (2.0 / rate_hz);
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() < deadline && seen < frames) {
        Image msg;
        rclcpp::MessageInfo info;
        if (!sub->take(msg, info)) continue;  // busy poll, like bench.cpp
        // A frame that arrived on the CPU path (backend != "cuda", or promoted into a fresh CUDA
        // buffer by from_input_buffer) is counted as a fallback and not timed.
        const bool cpu_path = msg.data.get_backend_type() != "cuda";
        auto rh = cuda_buffer_backend::from_input_buffer(msg.data, stream);  // waits on the write event
        const bool promoted = cpu_path || rh.get_promoted_buffer() != nullptr;
        const auto* dev = static_cast<const std::uint8_t*>(rh.get_ptr());
        check(cudaMemcpyAsync(host, dev, 16, cudaMemcpyDeviceToHost, stream), "DtoH header");
        check(cudaMemcpyAsync(host + 2, dev + bytes - 8, 8, cudaMemcpyDeviceToHost, stream), "DtoH tail");
        check(cudaStreamSynchronize(stream), "cudaStreamSynchronize");
        const std::uint64_t now = now_ns();
        if (host[1] != host[2]) { ++torn; continue; }
        if (host[1] <= last) continue;
        last = host[1];
        ++seen;
        if (promoted) { ++fallback; continue; }
        if (seen > 20) lat.push_back((now - host[0]) / 1000.0);
    }
    double mean = 0;
    for (double x : lat) mean += x;
    std::printf("{\"role\":\"sub\",\"transport\":\"rosidl_buffer\",\"bytes\":%zu,\"gpu\":1,\"frames_sent\":%d,"
                "\"frames_seen\":%d,\"torn\":%d,\"cpu_fallback\":%d,\"n\":%zu,\"p50_us\":%.1f,\"p90_us\":%.1f,"
                "\"p99_us\":%.1f,\"max_us\":%.1f,\"mean_us\":%.1f}\n",
                bytes, frames, seen, torn, fallback, lat.size(), percentile(lat, 50), percentile(lat, 90),
                percentile(lat, 99), lat.empty() ? 0.0 : *std::max_element(lat.begin(), lat.end()),
                lat.empty() ? 0.0 : mean / lat.size());
    std::fflush(stdout);
    return seen > 0 ? 0 : 3;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 5) {
        std::fprintf(stderr, "usage: rosidl_buffer_bench <pub|sub> <bytes> <frames> <rate_hz>\n");
        return 1;
    }
    const std::string role = argv[1];
    const std::size_t bytes = std::strtoull(argv[2], nullptr, 10);
    const int frames = std::atoi(argv[3]), rate = std::atoi(argv[4]);
    if (bytes % (kWidth * kChannels) != 0 || frames < 21 || rate < 1) {
        std::fprintf(stderr, "bytes must be a multiple of %u, frames > 20, rate >= 1\n", kWidth * kChannels);
        return 1;
    }
    rclcpp::init(0, nullptr);
    const int rc = role == "pub" ? run_pub(bytes, frames, rate) : role == "sub" ? run_sub(bytes, frames, rate) : 1;
    rclcpp::shutdown();
    return rc;
}
