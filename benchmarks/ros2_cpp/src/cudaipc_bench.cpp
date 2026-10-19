// SPDX-License-Identifier: Apache-2.0
// cudaipc_bench — the GPU → GPU single-stream cell of bench.cpp, over ros2_cuda_ipc
// (github.com/dskkato/ros2_cuda_ipc, MIT): CUDA VMM + POSIX FDs over a Unix socket, a
// GpuImage descriptor on a ROS 2 topic, and a CUDA IPC event for producer completion.
// Built only when ros2_cuda_ipc is found (see CMakeLists.txt). Same rules as bench.cpp:
// t0 before the full-payload write, header [t0, seq] + tail [seq], busy-polling consumer,
// clock stops once the consumer stream has the frame and its header/tail read back.
//
//   cudaipc_bench <pub|sub> <bytes> <frames> <rate_hz>
#include <cuda_runtime_api.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <sys/resource.h>

#include <rclcpp/rclcpp.hpp>

#include "axon/rt.h"
#include "gpu_read_kernel.h"
#include "ros2_cuda_ipc_core/publisher/gpu_buffer_manager.hpp"
#include "ros2_cuda_ipc_core/transport/message_utils.hpp"
#include "ros2_cuda_ipc_image/image_reader.hpp"
#include "ros2_cuda_ipc_msgs/msg/gpu_image.hpp"

namespace {

using GpuImage = ros2_cuda_ipc_msgs::msg::GpuImage;
constexpr std::uint32_t kWidth = 256, kChannels = 4;  // payload as a 256-wide RGBA image

void check(cudaError_t e, const char* what) {
    if (e != cudaSuccess) {
        std::fprintf(stderr, "CUDA error: %s: %s\n", what, cudaGetErrorString(e));
        std::exit(1);
    }
}

rclcpp::QoS qos() { return rclcpp::QoS(rclcpp::KeepLast(1)).best_effort().durability_volatile(); }

double cpu_seconds() {
    rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    return ru.ru_utime.tv_sec + ru.ru_stime.tv_sec + (ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) * 1e-6;
}

void sleep_until_ns(std::uint64_t target) {
    const std::uint64_t now = axon::rt_now_ns();
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
    auto node = rclcpp::Node::make_shared("cudaipc_pub");
    check(cudaSetDevice(0), "cudaSetDevice");
    // ros2_cuda_ipc does not reuse a block for 100 ms after publishing it, so it needs
    // rate x 0.1 s blocks (10 at 100 Hz); 16 by default, CUDAIPC_BLOCKS overrides.
    const char* nb_env = std::getenv("CUDAIPC_BLOCKS");
    const std::size_t n_blocks = nb_env ? std::strtoul(nb_env, nullptr, 10) : 16;
    ros2_cuda_ipc_core::publisher::GpuBufferManager manager({n_blocks, bytes, 0});
    if (!manager.initialise()) { std::fprintf(stderr, "GpuBufferManager::initialise failed\n"); return 1; }
    cudaStream_t stream = nullptr;
    check(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking), "cudaStreamCreate");
    void* src = nullptr;  // the "inference output" already on the GPU
    check(cudaMalloc(&src, bytes), "cudaMalloc");
    check(cudaMemset(src, 0x5a, bytes), "cudaMemset");

    rclcpp::PublisherOptions opts;
    opts.use_intra_process_comm = rclcpp::IntraProcessSetting::Disable;
    auto pub = node->create_publisher<GpuImage>("/axon_bench_cudaipc", qos(), opts);
    for (int k = 0; k < 200 && pub->get_subscription_count() < 1; ++k)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    const std::uint32_t rows = static_cast<std::uint32_t>(bytes / (kWidth * kChannels));
    // Pinned header/tail staging: a pageable source would make cudaMemcpyAsync wait for the
    // stream first, which defeats ros2_cuda_ipc's event-based completion. A ring of 16 slots,
    // so a slot is rewritten only after its copy (16 frames earlier) has long finished.
    constexpr int kSlots = 16;
    std::uint64_t* staging = nullptr;
    check(cudaMallocHost(reinterpret_cast<void**>(&staging), kSlots * 3 * sizeof(std::uint64_t)), "cudaMallocHost");
    int skipped = 0;
    const bool host_sync = std::getenv("CUDAIPC_HOST_SYNC") != nullptr;
    const double cpu0 = cpu_seconds();
    const std::uint64_t start = axon::rt_now_ns();
    for (int s = 1; s <= frames; ++s) {
        const std::uint64_t t0 = axon::rt_now_ns();
        auto block = manager.acquire_for_publish();
        if (!block) { ++skipped; sleep_until_ns(t0 + 1000000000ull / rate_hz); continue; }  // every block leased
        auto* dst = static_cast<std::uint8_t*>(block->device_ptr());
        std::uint64_t* h = staging + (s % kSlots) * 3;  // [t0, seq, seq]
        h[0] = t0;
        h[1] = h[2] = static_cast<std::uint64_t>(s);
        check(cudaMemcpyAsync(dst, src, bytes, cudaMemcpyDeviceToDevice, stream), "DtoD payload");
        check(cudaMemcpyAsync(dst, h, 16, cudaMemcpyHostToDevice, stream), "HtoD header");
        check(cudaMemcpyAsync(dst + bytes - 8, h + 2, 8, cudaMemcpyHostToDevice, stream), "HtoD tail");
        // ros2_cuda_ipc's own completion model: prepare_publish records a CUDA IPC event on
        // `stream`, and the consumer's read() waits on it, so the producer never blocks on
        // the copies (axon's cell cuCtxSynchronizes before publishing).
        // CUDAIPC_HOST_SYNC=1: finish the copies on the host before publishing, as axon's
        // cell does, so the consumer's event wait finds the work already done.
        if (host_sync) check(cudaStreamSynchronize(stream), "cudaStreamSynchronize");
        auto desc = block->prepare_publish(stream);
        if (!desc) { std::fprintf(stderr, "prepare_publish: %s\n", desc.error().to_string().c_str()); return 1; }
        GpuImage msg;
        ros2_cuda_ipc_core::transport::fill_buffer_core_message(desc.value(), msg.core);
        msg.dtype = static_cast<std::uint8_t>(ros2_cuda_ipc_image::DType::U8);
        msg.shape = {rows, kWidth, kChannels};
        msg.strides = {kWidth * kChannels, kChannels, 1};
        msg.encoding = "rgba8";
        pub->publish(msg);
        sleep_until_ns(t0 + 1000000000ull / rate_hz);
    }
    check(cudaStreamSynchronize(stream), "cudaStreamSynchronize");
    const double wall = (axon::rt_now_ns() - start) * 1e-9;
    std::printf("{\"role\":\"pub\",\"transport\":\"cudaipc\",\"bytes\":%zu,\"gpu\":1,\"frames\":%d,"
                "\"blocks\":%zu,\"skipped_no_block\":%d,\"cpu_s\":%.4f,\"wall_s\":%.4f}\n",
                bytes, frames, n_blocks, skipped, cpu_seconds() - cpu0, wall);
    std::fflush(stdout);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));  // let the consumer drop its leases
    return 0;
}

int run_sub(std::size_t bytes, int frames, int rate_hz) {
    auto node = rclcpp::Node::make_shared("cudaipc_sub");
    check(cudaSetDevice(0), "cudaSetDevice");
    cudaStream_t stream = nullptr;
    check(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking), "cudaStreamCreate");
    std::uint64_t* host = nullptr;  // pinned 24 B for the header/tail readback
    check(cudaMallocHost(reinterpret_cast<void**>(&host), 3 * sizeof(std::uint64_t)), "cudaMallocHost");
    const bool kernel_read = axon_bench::consumer_kernel_enabled();
    axon_bench::GpuReadKernel gpu_reader;
    if (kernel_read) gpu_reader.init();

    rclcpp::SubscriptionOptions opts;
    opts.use_intra_process_comm = rclcpp::IntraProcessSetting::Disable;
    auto sub = node->create_subscription<GpuImage>("/axon_bench_cudaipc", qos(), [](const GpuImage&) {}, opts);
    ros2_cuda_ipc_image::ImageReader reader;

    std::vector<double> lat, st_take, st_read, st_sync;  // per-stage µs, timed frames only
    int seen = 0, torn = 0, read_fail = 0;
    std::uint64_t last = 0;
    const double deadline = 10.0 + frames * (2.0 / rate_hz);
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() < deadline && seen < frames) {
        GpuImage msg;
        rclcpp::MessageInfo info;
        if (!sub->take(msg, info)) continue;  // busy poll, like bench.cpp
        const std::uint64_t t_take = axon::rt_now_ns();
        auto view = reader.read(msg, reinterpret_cast<CUstream>(stream));  // waits on the producer's event
        const std::uint64_t t_read = axon::rt_now_ns();
        if (!view) { ++read_fail; continue; }
        auto* dev = view->read.data<std::uint8_t>();
        if (kernel_read) {  // read the frame on the GPU, behind the event wait on the same stream
            gpu_reader.launch(reinterpret_cast<CUdeviceptr>(dev), bytes, reinterpret_cast<CUstream>(stream));
            check(cudaStreamSynchronize(stream), "cudaStreamSynchronize");
            for (int k = 0; k < 3; ++k) host[k] = gpu_reader.result()[k];
        } else {
            check(cudaMemcpyAsync(host, dev, 16, cudaMemcpyDeviceToHost, stream), "DtoH header");
            check(cudaMemcpyAsync(host + 2, dev + bytes - 8, 8, cudaMemcpyDeviceToHost, stream), "DtoH tail");
            check(cudaStreamSynchronize(stream), "cudaStreamSynchronize");
        }
        const std::uint64_t now = axon::rt_now_ns();
        if (host[1] != host[2]) { ++torn; continue; }
        if (host[1] <= last) continue;
        last = host[1];
        ++seen;
        if (seen > 20) {
            lat.push_back((now - host[0]) / 1000.0);
            st_take.push_back((t_take - host[0]) / 1e3);  // t0 -> descriptor taken from DDS
            st_read.push_back((t_read - t_take) / 1e3);   // read(): import + event wait enqueued
            st_sync.push_back((now - t_read) / 1e3);      // event wait + 24 B readback + sync
        }
    }
    std::printf("{\"role\":\"sub\",\"transport\":\"cudaipc\",\"bytes\":%zu,\"gpu\":1,\"frames_sent\":%d,"
                "\"frames_seen\":%d,\"torn\":%d,\"read_fail\":%d,\"n\":%zu,\"p50_us\":%.1f,\"p90_us\":%.1f,"
                "\"p99_us\":%.1f,\"max_us\":%.1f,\"mean_us\":%.1f,\"consumer_kernel\":%d,\"host_sync_producer\":%s,"
                "\"stage_take_p50_us\":%.1f,\"stage_read_p50_us\":%.1f,\"stage_sync_p50_us\":%.1f}\n",
                bytes, frames, seen, torn, read_fail, lat.size(), percentile(lat, 50), percentile(lat, 90),
                percentile(lat, 99), lat.empty() ? 0.0 : *std::max_element(lat.begin(), lat.end()),
                lat.empty() ? 0.0 : [&] { double t = 0; for (double x : lat) t += x; return t / lat.size(); }(),
                kernel_read ? 1 : 0,
                std::getenv("CUDAIPC_HOST_SYNC") ? "true" : "false",
                percentile(st_take, 50), percentile(st_read, 50), percentile(st_sync, 50));
    std::fflush(stdout);
    return seen > 0 ? 0 : 3;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 5) {
        std::fprintf(stderr, "usage: cudaipc_bench <pub|sub> <bytes> <frames> <rate_hz>\n");
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
