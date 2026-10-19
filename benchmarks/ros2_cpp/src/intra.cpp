// SPDX-License-Identifier: Apache-2.0
// axon_bench — intra-process zero-copy benchmark (same-process, two nodes).
// Usage: intra <bytes> <frames> <rate_hz> <gpu 0|1> <poll|wait>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <chrono>
#include <vector>
#include <string>
#include <algorithm>
#include <cmath>
#include <atomic>
#include <thread>
#include <sys/resource.h>
#include <sys/time.h>
#include <time.h>

#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/u_int8_multi_array.hpp>

#include "axon_bench/msg/payload147_k.hpp"
#include "axon_bench/msg/payload1_m.hpp"
#include "axon_bench/msg/payload4_m.hpp"

#include "axon/rt.h"
#include "gpu_read_kernel.h"

#include <cuda.h>

// ============ CUDA helpers (copied from bench.cpp) ============

static void check_cuda(CUresult r, const char* call) {
    if (r != CUDA_SUCCESS) {
        const char* msg = nullptr;
        cuGetErrorName(r, &msg);
        std::fprintf(stderr, "CUDA error: %s returned %s\n", call, msg ? msg : "?");
        std::exit(1);
    }
}

static CUcontext g_ctx = nullptr;  // the executor thread makes it current too

static void init_cuda_ctx() {
    check_cuda(cuInit(0), "cuInit");
    CUdevice dev;
    check_cuda(cuDeviceGet(&dev, 0), "cuDeviceGet");
    CUcontext ctx;
    check_cuda(cuDevicePrimaryCtxRetain(&ctx, dev), "cuDevicePrimaryCtxRetain");
    check_cuda(cuCtxSetCurrent(ctx), "cuCtxSetCurrent");
    g_ctx = ctx;
}

static double cpu_seconds() {
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return ru.ru_utime.tv_sec + ru.ru_stime.tv_sec
          + ru.ru_utime.tv_usec * 1e-6 + ru.ru_stime.tv_usec * 1e-6;
}

struct SubResult {
    int frames_seen = 0;
    int torn = 0;
    int copied = 0;       // host: frames where msg != sent_ptr
    int converted = 0;    // gpu: type-adapter convert count
    double cpu_s = 0;
    std::vector<double> samples_us;
};

static double percentile(std::vector<double> v, double p) {
    std::sort(v.begin(), v.end());
    if (v.empty()) return 0;
    int rank = static_cast<int>(std::ceil(p / 100.0 * v.size()));
    if (rank < 1) rank = 1;
    if (rank > (int)v.size()) rank = (int)v.size();
    return v[rank - 1];
}

static double mean_val(const std::vector<double>& v) {
    if (v.empty()) return 0;
    double s = 0;
    for (auto x : v) s += x;
    return s / v.size();
}

static void sleep_until(uint64_t target_ns) {
    uint64_t now = axon::rt_now_ns();
    if (now < target_ns) {
        uint64_t remaining = target_ns - now;
        struct timespec ts;
        ts.tv_sec = (time_t)(remaining / 1000000000ULL);
        ts.tv_nsec = (long)(remaining % 1000000000ULL);
        nanosleep(&ts, nullptr);
    }
}

static void print_sub_result(const char* transport, int bytes, int gpu, int frames_sent,
                             const SubResult& res, double wall_s) {
    std::fprintf(stdout,
        "{\"role\":\"sub\",\"transport\":\"%s\",\"bytes\":%d,\"gpu\":%d,"
        "\"frames_sent\":%d,\"frames_seen\":%d,\"torn\":%d,\"n\":%d,"
        "\"p50_us\":%.1f,\"p90_us\":%.1f,\"p99_us\":%.1f,\"max_us\":%.1f,"
        "\"mean_us\":%.1f,\"cpu_s\":%.4f,\"wall_s\":%.4f",
        transport, bytes, gpu, frames_sent, res.frames_seen, res.torn,
        (int)res.samples_us.size(),
        percentile(res.samples_us, 50), percentile(res.samples_us, 90),
        percentile(res.samples_us, 99),
        res.samples_us.empty() ? 0.0 :
            *std::max_element(res.samples_us.begin(), res.samples_us.end()),
        mean_val(res.samples_us), res.cpu_s, wall_s);
    if (gpu == 0) {
        std::fprintf(stdout, ",\"copied\":%d}", res.copied);
    } else {
        std::fprintf(stdout, ",\"converted\":%d,\"consumer_kernel\":%d}", res.converted,
                     axon_bench::consumer_kernel_enabled() ? 1 : 0);
    }
    std::fprintf(stdout, "\n");
    std::fflush(stdout);
}

// ============ REP-2007 type adapter: GpuFrame <-> UInt8MultiArray ============

struct GpuFrame {
    CUdeviceptr ptr;
    std::size_t bytes;
};

static std::atomic<int> g_converted{0};

template<>
struct rclcpp::TypeAdapter<GpuFrame, std_msgs::msg::UInt8MultiArray> {
    using is_specialized = std::true_type;
    using custom_type = GpuFrame;
    using ros_message_type = std_msgs::msg::UInt8MultiArray;

    static void convert_to_ros_message(const custom_type& source,
                                       ros_message_type& destination) {
        destination.data.resize(source.bytes);
        check_cuda(cuMemcpyDtoH(destination.data.data(), source.ptr,
                                source.bytes),
                   "TypeAdapter cuMemcpyDtoH");
        g_converted.fetch_add(1, std::memory_order_relaxed);
    }

    static void convert_to_custom(const ros_message_type& source,
                                  custom_type& destination) {
        std::fprintf(stderr, "convert_to_custom not implemented\n");
        std::abort();
    }
};

RCLCPP_USING_CUSTOM_TYPE_AS_ROS_MESSAGE_TYPE(GpuFrame,
                                             std_msgs::msg::UInt8MultiArray);

// ============ Host cell: run_intra_host<MsgT> ============

template<typename MsgT>
static void run_intra_host(int bytes, int frames, int rate_hz,
                           const char* mode_str) {
    rclcpp::NodeOptions node_opts;
    node_opts.use_intra_process_comms(true);

    auto pub_node = rclcpp::Node::make_shared("intra_pub", node_opts);
    auto sub_node = rclcpp::Node::make_shared("intra_sub", node_opts);

    auto qos = rclcpp::QoS(rclcpp::KeepLast(1));
    qos.best_effort();
    qos.durability_volatile();

    std::uint64_t last_seqno = 0;
    int seen = 0;
    std::atomic<int> torn_cnt{0};
    std::atomic<int> copied_cnt{0};
    std::vector<double> latencies;

    auto pub = pub_node->create_publisher<MsgT>("/axon_bench_intra", qos);


    std::vector<uint8_t> src(bytes);
    for (int i = 0; i < (int)bytes; ++i)
        src[i] = (uint8_t)(i * 31 & 0xff);

    std::vector<const void*> sent_ptr(frames + 1);

    auto sub_cb = [&](std::unique_ptr<MsgT> msg) {
        if (!msg) return;
        uint64_t now = axon::rt_now_ns();

        uint64_t head_seq, tail_seq;
        std::memcpy(&head_seq, msg->data.data() + 8, 8);
        std::memcpy(&tail_seq, msg->data.data() + bytes - 8, 8);

        if (head_seq != tail_seq) {
            torn_cnt.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        uint64_t t0;
        std::memcpy(&t0, msg->data.data(), 8);

        if (head_seq > last_seqno) {
            ++seen;
            if (seen > 20) latencies.push_back((now - t0) / 1000.0);
            last_seqno = head_seq;

            if (head_seq <= (uint64_t)frames) {
                if (msg.get() != sent_ptr[head_seq]) {
                    copied_cnt.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
    };

    auto sub = sub_node->create_subscription<MsgT>(
        "/axon_bench_intra", qos, sub_cb);

    auto start_wait = std::chrono::steady_clock::now();
    while (pub->get_intra_process_subscription_count() < 1) {
        if (std::chrono::steady_clock::now() - start_wait >
            std::chrono::seconds(10)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    rclcpp::executors::SingleThreadedExecutor exec;
    exec.add_node(sub_node);
    std::thread exec_thread;
    std::atomic<bool> done{false};

    if (std::string(mode_str) == "poll") {
        exec_thread = std::thread([&]() {
            while (!done.load()) {
                exec.spin_some(std::chrono::nanoseconds(0));
            }
        });
    } else {
        exec_thread = std::thread([&]() {
            exec.spin();
        });
    }

    struct timeval tv0, tv1;
    gettimeofday(&tv0, nullptr);
    double cpu0 = cpu_seconds();

    for (std::uint64_t seq = 1; seq <= (std::uint64_t)frames; ++seq) {
        uint64_t t0 = axon::rt_now_ns();

        auto m = std::make_unique<MsgT>(
            rosidl_runtime_cpp::MessageInitialization::SKIP);
        std::memcpy(m->data.data(), src.data(), bytes);

        std::memcpy(m->data.data(), &t0, 8);
        std::memcpy(m->data.data() + 8, &seq, 8);
        std::memcpy(m->data.data() + bytes - 8, &seq, 8);

        sent_ptr[seq] = m.get();
        pub->publish(std::move(m));
        sleep_until(t0 + (uint64_t)(1e9 / rate_hz));
    }

    gettimeofday(&tv1, nullptr);
    double wall_s = (tv1.tv_sec - tv0.tv_sec) +
                    (tv1.tv_usec - tv0.tv_usec) * 1e-6;
    double cpu1 = cpu_seconds();

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    done.store(true);
    if (std::string(mode_str) == "wait") exec.cancel();
    exec_thread.join();

    SubResult res;
    res.frames_seen = seen;
    res.torn = torn_cnt.load();
    res.copied = copied_cnt.load();
    res.cpu_s = cpu1 - cpu0;
    res.samples_us = std::move(latencies);

    std::string tr = std::string("ros_intra_") + mode_str;
    print_sub_result(tr.c_str(), bytes, 0, frames, res, wall_s);
}

// ============ GPU cell: run_intra_gpu ============

static void run_intra_gpu(int bytes, int frames, int rate_hz,
                          const char* mode_str) {
    init_cuda_ctx();

    rclcpp::NodeOptions node_opts;
    node_opts.use_intra_process_comms(true);

    auto pub_node = rclcpp::Node::make_shared("intra_pub", node_opts);
    auto sub_node = rclcpp::Node::make_shared("intra_sub", node_opts);

    auto qos = rclcpp::QoS(rclcpp::KeepLast(1));
    qos.best_effort();
    qos.durability_volatile();

    std::uint64_t last_seqno = 0;
    int seen = 0;
    std::atomic<int> torn_cnt{0};
    std::vector<double> latencies;
    g_converted.store(0);

    // Publisher with GpuFrame TypeAdapter
    auto pub = pub_node->create_publisher<
        rclcpp::TypeAdapter<GpuFrame,
                            std_msgs::msg::UInt8MultiArray>>(
        "/axon_bench_intra", qos);


    // Source on host
    std::vector<uint8_t> src(bytes);
    for (int i = 0; i < (int)bytes; ++i)
        src[i] = (uint8_t)(i * 31 & 0xff);

    // Device ring + d_src
    const char* nbuf_env = std::getenv("AXON_BENCH_NBUF");
    const std::size_t nbuf = nbuf_env ? std::strtoul(nbuf_env, nullptr, 10) : 4;

    std::vector<CUdeviceptr> d_ring(nbuf);
    for (std::size_t i = 0; i < nbuf; ++i)
        check_cuda(cuMemAlloc(&d_ring[i], bytes), "cuMemAlloc ring");

    CUdeviceptr d_src;
    check_cuda(cuMemAlloc(&d_src, bytes), "cuMemAlloc src");
    check_cuda(cuMemcpyHtoD(d_src, src.data(), bytes), "cuMemcpyHtoD src");

    const bool kernel_read = axon_bench::consumer_kernel_enabled();
    axon_bench::GpuReadKernel reader;
    if (kernel_read) reader.init();

    // GPU subscriber callback
    auto sub_cb = [&](std::unique_ptr<GpuFrame> frame) {
        if (!frame || frame->bytes != (std::size_t)bytes) return;

        uint8_t hdr[16];
        uint8_t tail[8];
        if (kernel_read) {  // read the frame on the GPU (same process: no context switch)
            reader.launch(frame->ptr, bytes, 0);
            check_cuda(cuCtxSynchronize(), "cuCtxSynchronize");
            const uint64_t r[3] = {reader.result()[0], reader.result()[1], reader.result()[2]};
            std::memcpy(hdr, r, 16);
            std::memcpy(tail, r + 2, 8);
        } else {
            check_cuda(cuMemcpyDtoH(hdr, frame->ptr, 16),
                       "callback cuMemcpyDtoH hdr");
            check_cuda(cuMemcpyDtoH(tail, frame->ptr + bytes - 8, 8),
                       "callback cuMemcpyDtoH tail");
        }

        uint64_t head_seq, tail_seq, t0;
        std::memcpy(&t0, hdr, 8);
        std::memcpy(&head_seq, hdr + 8, 8);
        std::memcpy(&tail_seq, tail, 8);

        uint64_t now = axon::rt_now_ns();

        if (head_seq != tail_seq) {
            torn_cnt.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        if (head_seq > last_seqno) {
            ++seen;
            if (seen > 20) latencies.push_back((now - t0) / 1000.0);
            last_seqno = head_seq;
        }
    };

    auto sub = sub_node->create_subscription<GpuFrame>(
        "/axon_bench_intra", qos, sub_cb);

    // Executor for subscriber
    auto start_wait = std::chrono::steady_clock::now();
    while (pub->get_intra_process_subscription_count() < 1) {
        if (std::chrono::steady_clock::now() - start_wait >
            std::chrono::seconds(10)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    rclcpp::executors::SingleThreadedExecutor exec;
    exec.add_node(sub_node);
    std::thread exec_thread;
    std::atomic<bool> done{false};

    if (std::string(mode_str) == "poll") {
        exec_thread = std::thread([&]() {
            check_cuda(cuCtxSetCurrent(g_ctx), "cuCtxSetCurrent executor");
            while (!done.load()) {
                exec.spin_some(std::chrono::nanoseconds(0));
            }
        });
    } else {
        exec_thread = std::thread([&]() {
            check_cuda(cuCtxSetCurrent(g_ctx), "cuCtxSetCurrent executor");
            exec.spin();
        });
    }

    // Publisher (main thread)
    struct timeval tv0, tv1;
    gettimeofday(&tv0, nullptr);
    double cpu0 = cpu_seconds();

    for (std::uint64_t seq = 1; seq <= (std::uint64_t)frames; ++seq) {
        uint64_t t0 = axon::rt_now_ns();
        CUdeviceptr dp = d_ring[seq % nbuf];

        // Copy payload from d_src
        check_cuda(cuMemcpyDtoD(dp, d_src, bytes), "cuMemcpyDtoD");

        // Stamp header (t0 at offset 0, seq at 8) and tail (seq at bytes-8)
        uint64_t header16[2];
        header16[0] = t0;
        header16[1] = (uint64_t)seq;
        check_cuda(cuMemcpyHtoD(dp, header16, 16), "cuMemcpyHtoD header");
        check_cuda(cuMemcpyHtoD(dp + bytes - 8, &seq, 8),
                   "cuMemcpyHtoD tail");
        check_cuda(cuCtxSynchronize(), "cuCtxSynchronize");

        pub->publish(std::make_unique<GpuFrame>(GpuFrame{dp, (size_t)bytes}));
        sleep_until(t0 + (uint64_t)(1e9 / rate_hz));
    }

    gettimeofday(&tv1, nullptr);
    double wall_s = (tv1.tv_sec - tv0.tv_sec) +
                    (tv1.tv_usec - tv0.tv_usec) * 1e-6;
    double cpu1 = cpu_seconds();

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    done.store(true);
    if (std::string(mode_str) == "wait") exec.cancel();
    exec_thread.join();

    // Cleanup device buffers
    for (std::size_t i = 0; i < nbuf; ++i)
        check_cuda(cuMemFree(d_ring[i]), "cuMemFree ring");
    check_cuda(cuMemFree(d_src), "cuMemFree src");

    SubResult res;
    res.frames_seen = seen;
    res.torn = torn_cnt.load();
    res.converted = g_converted.load();
    res.cpu_s = cpu1 - cpu0;
    res.samples_us = std::move(latencies);

    std::string tr = std::string("ros_intra_") + mode_str;
    print_sub_result(tr.c_str(), bytes, 1, frames, res, wall_s);
}

// ============ Dispatch ============

static void dispatch_intra(int bytes, int frames, int rate_hz, int gpu,
                           const char* mode_str) {
    if (gpu == 0) {
        if (bytes == 150528) {
            run_intra_host<axon_bench::msg::Payload147K>(
                bytes, frames, rate_hz, mode_str);
        } else if (bytes == 1048576) {
            run_intra_host<axon_bench::msg::Payload1M>(
                bytes, frames, rate_hz, mode_str);
        } else {
            run_intra_host<axon_bench::msg::Payload4M>(
                bytes, frames, rate_hz, mode_str);
        }
    } else {
        run_intra_gpu(bytes, frames, rate_hz, mode_str);
    }
}

int main(int argc, char** argv) {
    if (argc != 6) {
        std::fprintf(stderr,
                     "Usage: intra <bytes> <frames> <rate_hz> <gpu 0|1> <poll|wait>\n");
        return 1;
    }

    int bytes = atoi(argv[1]);
    int frames = atoi(argv[2]);
    int rate_hz = atoi(argv[3]);
    int gpu = atoi(argv[4]);
    const char* mode = argv[5];

    if (bytes != 150528 && bytes != 1048576 && bytes != 4194304) {
        std::fprintf(stderr,
                     "bytes must be 150528, 1048576, or 4194304\n");
        return 1;
    }
    if (gpu != 0 && gpu != 1) {
        std::fprintf(stderr, "gpu must be 0 or 1\n");
        return 1;
    }
    if (strcmp(mode, "poll") != 0 && strcmp(mode, "wait") != 0) {
        std::fprintf(stderr, "mode must be poll or wait\n");
        return 1;
    }

    rclcpp::init(0, nullptr);
    dispatch_intra(bytes, frames, rate_hz, gpu, mode);
    rclcpp::shutdown();
    return 0;
}
