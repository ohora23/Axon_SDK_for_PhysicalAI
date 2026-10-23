// SPDX-License-Identifier: Apache-2.0
// axon_bench — C++ benchmark: axon vs ROS 2 (rclcpp default, and loaned messages +
// Fast DDS data-sharing), host and GPU payloads. See README.md for the method.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <chrono>
#include <optional>
#include <vector>
#include <string>
#include <algorithm>
#include <cmath>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/syscall.h>
#include <sched.h>
#include <linux/perf_event.h>

#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/u_int8_multi_array.hpp>

#include "axon_bench/msg/payload147_k.hpp"
#include "axon_bench/msg/payload1_m.hpp"
#include "axon_bench/msg/payload4_m.hpp"
#include "axon_bench/msg/payload16_m.hpp"
#include "axon_bench/msg/payload64_m.hpp"
#include "axon_bench/msg/payload64.hpp"
#include "axon_bench/msg/payload8640.hpp"

// Payload sizes the ros_loan cells have a fixed-size message type for.
#define AXON_BENCH_SIZES(X) X(64, Payload64) X(8640, Payload8640) X(150528, Payload147K) \
    X(1048576, Payload1M) X(4194304, Payload4M) X(16777216, Payload16M) X(67108864, Payload64M)

#include "axon/pool.h"
#include "axon/publisher.h"
#include "axon/subscriber.h"
#include "axon/rt.h"
#include "fastdds_check.h"
#include "gpu_read_kernel.h"

#include <cuda.h>

using namespace axon;

static void check_cuda(CUresult r, const char* call) {
    if (r != CUDA_SUCCESS) {
        const char* msg = nullptr;
        cuGetErrorName(r, &msg);
        std::fprintf(stderr, "CUDA error: %s returned %s\n", call, msg ? msg : "?");
        std::exit(1);
    }
}

static void init_cuda_ctx() {
    check_cuda(cuInit(0), "cuInit");
    CUdevice dev;
    check_cuda(cuDeviceGet(&dev, 0), "cuDeviceGet");
    CUcontext ctx;
    check_cuda(cuDevicePrimaryCtxRetain(&ctx, dev), "cuDevicePrimaryCtxRetain");
    check_cuda(cuCtxSetCurrent(ctx), "cuCtxSetCurrent");
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
    std::vector<double> samples_us;
    double cpu_s = 0;
    double cpu_window_s = 0;
    double window_s = 0;
    std::uint64_t first_seq = 0;  // seqno of the first frame counted (frames before it are setup)
};

// BENCH_NSUBS (default 1, at least 1) and BENCH_WAIT (opt-in blocking consumers) for the fan-out runs.
static int env_nsubs() { const char* e = std::getenv("BENCH_NSUBS"); return e ? std::max(1, atoi(e)) : 1; }
static bool env_wait() { const char* e = std::getenv("BENCH_WAIT"); return e && atoi(e); }

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

static void stamp_header_tail_vec(std::vector<uint8_t>& buf, std::uint64_t t0, std::uint64_t seqno) {
    std::memcpy(buf.data(), &t0, 8);
    std::memcpy(buf.data() + 8, &seqno, 8);
    std::memcpy(buf.data() + buf.size() - 8, &seqno, 8);
}

template<typename Container>
static void stamp_header_tail_arr(Container& buf, std::uint64_t t0, std::uint64_t seqno) {
    std::memcpy(buf.data(), &t0, 8);
    std::memcpy(buf.data() + 8, &seqno, 8);
    std::memcpy(buf.data() + buf.size() - 8, &seqno, 8);
}

static std::uint64_t read_head_seqno(const uint8_t* buf, std::size_t sz) {
    std::uint64_t seqno;
    std::memcpy(&seqno, buf + 8, 8);
    return seqno;
}

static std::uint64_t read_tail_seqno(const uint8_t* buf, std::size_t sz) {
    std::uint64_t seqno;
    std::memcpy(&seqno, buf + sz - 8, 8);
    return seqno;
}

static void sleep_until(std::uint64_t target_ns) {
    std::uint64_t now = axon::rt_now_ns();
    if (now < target_ns) {
        uint64_t remaining = target_ns - now;
        struct timespec ts;
        ts.tv_sec = (time_t)(remaining / 1000000000ULL);
        ts.tv_nsec = (long)(remaining % 1000000000ULL);
        nanosleep(&ts, nullptr);
    }
}

static void print_sub_result(const char* transport, int bytes, int gpu, int frames_sent,
                              const SubResult& res) {
    const int wait_on = env_wait() && std::strcmp(transport, "ros_dyn") != 0 ? 1 : 0;  // ros_dyn never blocks
    const char* sub_id_env = std::getenv("BENCH_SUB_ID");
    int sub_id = sub_id_env ? atoi(sub_id_env) : 0;
    std::fprintf(stdout,
        "{\"role\":\"sub\",\"transport\":\"%s\",\"bytes\":%d,\"gpu\":%d,\"frames_sent\":%d,\"frames_seen\":%d,\"torn\":%d,\"n\":%d,\"p50_us\":%.1f,\"p90_us\":%.1f,\"p99_us\":%.1f,\"max_us\":%.1f,\"mean_us\":%.1f,\"cpu_s\":%.4f,\"cpu_window_s\":%.4f,\"window_s\":%.4f,\"sub_id\":%d,\"wait\":%d,\"first_seq\":%llu,\"consumer_kernel\":%d}\n",
        transport, bytes, gpu, frames_sent, res.frames_seen, res.torn,
        (int)res.samples_us.size(),
        percentile(res.samples_us, 50), percentile(res.samples_us, 90),
        percentile(res.samples_us, 99),
        res.samples_us.empty() ? 0.0 : *std::max_element(res.samples_us.begin(), res.samples_us.end()),
        mean_val(res.samples_us), res.cpu_s, res.cpu_window_s, res.window_s,
        sub_id, wait_on, (unsigned long long)res.first_seq,
        gpu == 1 && axon_bench::consumer_kernel_enabled() && std::strcmp(transport, "ros_dyn") != 0 ? 1 : 0);
    std::fflush(stdout);
}

// ============ ROS dynamic producer ============
static int run_pub_ros_dyn(int bytes, int frames, int rate_hz, int gpu) {
    axon_bench::require_fits_shm_segment(bytes, "the UInt8MultiArray payload");
    rclcpp::init(0, nullptr);
    auto node = rclcpp::Node::make_shared("bench_pub");
    auto pub = node->create_publisher<std_msgs::msg::UInt8MultiArray>(
        "/axon_bench",
        rclcpp::QoS(rclcpp::KeepLast(1)).best_effort().durability_volatile());

    const int nsubs = env_nsubs();

    auto start_wait = std::chrono::steady_clock::now();
    while (pub->get_subscription_count() < nsubs) {
        if (std::chrono::steady_clock::now() - start_wait > std::chrono::seconds(10)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    std_msgs::msg::UInt8MultiArray msg;
    msg.data.resize(bytes);

    std::vector<uint8_t> src(bytes);
    for (int i = 0; i < (int)bytes; ++i) src[i] = (uint8_t)(i * 31 & 0xff);
    CUdeviceptr d_src = 0;
    if (gpu == 1) {
        init_cuda_ctx();
        check_cuda(cuMemAlloc(&d_src, bytes), "cuMemAlloc src");
        check_cuda(cuMemcpyHtoD(d_src, src.data(), bytes), "cuMemcpyHtoD src");
    }

    struct timeval tv0, tv1;
    gettimeofday(&tv0, nullptr);
    double cpu0 = cpu_seconds();

    for (int seq = 1; seq <= frames; ++seq) {
        std::uint64_t t0 = axon::rt_now_ns();
        if (gpu == 0) {
            std::memcpy(msg.data.data(), src.data(), bytes);
        } else {
            check_cuda(cuMemcpyDtoH(msg.data.data(), d_src, bytes), "cuMemcpyDtoH");
        }
        stamp_header_tail_vec(msg.data, t0, seq);
        pub->publish(msg);
        sleep_until(t0 + (std::uint64_t)(1e9 / rate_hz));
    }

    gettimeofday(&tv1, nullptr);
    double wall_s = (tv1.tv_sec - tv0.tv_sec) + (tv1.tv_usec - tv0.tv_usec) * 1e-6;
    double cpu1 = cpu_seconds();

    std::fprintf(stdout,
        "{\"role\":\"pub\",\"transport\":\"ros_dyn\",\"bytes\":%d,\"gpu\":%d,\"frames\":%d,\"cpu_s\":%.4f,\"wall_s\":%.4f,\"n_subs\":%d}\n",
        bytes, gpu, frames, cpu1 - cpu0, wall_s, nsubs);
    std::fflush(stdout);
    rclcpp::shutdown();
    return 0;
}

// ============ ROS dynamic consumer ============
static SubResult run_sub_ros_dyn(int bytes, int frames, int rate_hz, int gpu) {
    axon_bench::require_fits_shm_segment(bytes, "the UInt8MultiArray payload");
    rclcpp::init(0, nullptr);
    char name[64];
    snprintf(name, sizeof(name), "bench_sub_%d", (int)getpid());
    auto node = rclcpp::Node::make_shared(name);

    std_msgs::msg::UInt8MultiArray msg;
    rclcpp::MessageInfo info;
    auto sub = node->create_subscription<std_msgs::msg::UInt8MultiArray>(
        "/axon_bench",
        rclcpp::QoS(rclcpp::KeepLast(1)).best_effort().durability_volatile(),
        [](std_msgs::msg::UInt8MultiArray::ConstSharedPtr){});
    (void)sub;

    SubResult res;
    std::uint64_t last_seqno = 0;
    std::vector<double> latencies;
    int seen = 0;

    auto start = std::chrono::steady_clock::now();
    double deadline = 10.0 + frames * (2.0 / rate_hz);
    double cpu_first = 0, cpu_last = 0;
    std::uint64_t wall_last = 0;
    std::uint64_t wall_first = 0;

    if (gpu == 1) {
        init_cuda_ctx();
        CUdeviceptr d_dst = 0;
        check_cuda(cuMemAlloc(&d_dst, bytes), "cuMemAlloc dst");
        std_msgs::msg::UInt8MultiArray msg_buf;
        while ((std::chrono::steady_clock::now() - start).count() / 1e9 < deadline) {
            if (sub->take(msg_buf, info)) {
                check_cuda(cuMemcpyHtoD(d_dst, msg_buf.data.data(), bytes), "cuMemcpyHtoD dst");
                check_cuda(cuCtxSynchronize(), "cuCtxSynchronize");
                std::uint64_t now = axon::rt_now_ns();
                std::uint64_t head_seq = read_head_seqno(msg_buf.data.data(), msg_buf.data.size());
                std::uint64_t tail_seq = read_tail_seqno(msg_buf.data.data(), msg_buf.data.size());
                if (head_seq != tail_seq) ++res.torn;
                std::uint64_t t0;
                std::memcpy(&t0, msg_buf.data.data(), 8);
                if (head_seq > last_seqno) {
                    ++seen;
                    if (seen == 1) {
                        res.first_seq = head_seq;
                        cpu_first = cpu_seconds();
                        wall_first = axon::rt_now_ns();
                    }
                    if (seen > 20) latencies.push_back((now - t0) / 1000.0);
                    last_seqno = head_seq;
                    cpu_last = cpu_seconds();  // the CPU window ends at the last frame taken
                    wall_last = axon::rt_now_ns();
                }
            }
            if (seen >= frames || last_seqno >= (std::uint64_t)frames) break;
        }
        check_cuda(cuMemFree(d_dst), "cuMemFree");
    } else {
        while ((std::chrono::steady_clock::now() - start).count() / 1e9 < deadline) {
            if (sub->take(msg, info)) {
                std::uint64_t now = axon::rt_now_ns();
                std::uint64_t head_seq = read_head_seqno(msg.data.data(), msg.data.size());
                std::uint64_t tail_seq = read_tail_seqno(msg.data.data(), msg.data.size());
                if (head_seq != tail_seq) ++res.torn;
                std::uint64_t t0;
                std::memcpy(&t0, msg.data.data(), 8);
                if (head_seq > last_seqno) {
                    ++seen;
                    if (seen == 1) {
                        res.first_seq = head_seq;
                        cpu_first = cpu_seconds();
                        wall_first = axon::rt_now_ns();
                    }
                    if (seen > 20) latencies.push_back((now - t0) / 1000.0);
                    last_seqno = head_seq;
                    cpu_last = cpu_seconds();  // the CPU window ends at the last frame taken
                    wall_last = axon::rt_now_ns();
                }
            }
            if (seen >= frames || last_seqno >= (std::uint64_t)frames) break;
        }
    }

    res.frames_seen = seen;
    res.cpu_s = cpu_seconds();
    if (seen >= 1) {
        res.cpu_window_s = cpu_last - cpu_first;
        res.window_s = (wall_last - wall_first) / 1e9;
    }
    res.samples_us = std::move(latencies);
    print_sub_result("ros_dyn", bytes, gpu, frames, res);
    rclcpp::shutdown();
    return res;
}

// ============ ROS loaned producer ============
template<typename MsgT>
static int run_pub_ros_loan(int bytes, int frames, int rate_hz, int gpu) {
    rclcpp::init(0, nullptr);
    auto node = rclcpp::Node::make_shared("bench_pub");
    auto pub = node->create_publisher<MsgT>(
        "/axon_bench",
        rclcpp::QoS(rclcpp::KeepLast(1)).best_effort().durability_volatile());

    bool can_loan = pub->can_loan_messages();
    std::fprintf(stderr, "can_loan_messages=%d\n", can_loan ? 1 : 0);

    const int nsubs = env_nsubs();

    auto start_wait = std::chrono::steady_clock::now();
    while (pub->get_subscription_count() < nsubs) {
        if (std::chrono::steady_clock::now() - start_wait > std::chrono::seconds(10)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    std::vector<uint8_t> src(bytes);
    for (int i = 0; i < (int)bytes; ++i) src[i] = (uint8_t)(i * 31 & 0xff);
    CUdeviceptr d_src = 0;
    if (gpu == 1) {
        init_cuda_ctx();
        check_cuda(cuMemAlloc(&d_src, bytes), "cuMemAlloc src");
        check_cuda(cuMemcpyHtoD(d_src, src.data(), bytes), "cuMemcpyHtoD src");
    }

    struct timeval tv0, tv1;
    gettimeofday(&tv0, nullptr);
    double cpu0 = cpu_seconds();

    for (int seq = 1; seq <= frames; ++seq) {
        std::uint64_t t0 = axon::rt_now_ns();
        auto loaned = pub->borrow_loaned_message();
        if (gpu == 0) {
            std::memcpy(loaned.get().data.data(), src.data(), bytes);
        } else {
            check_cuda(cuMemcpyDtoH(loaned.get().data.data(), d_src, bytes), "cuMemcpyDtoH");
        }
        stamp_header_tail_arr(loaned.get().data, t0, seq);
        pub->publish(std::move(loaned));
        sleep_until(t0 + (std::uint64_t)(1e9 / rate_hz));
    }

    gettimeofday(&tv1, nullptr);
    double wall_s = (tv1.tv_sec - tv0.tv_sec) + (tv1.tv_usec - tv0.tv_usec) * 1e-6;
    double cpu1 = cpu_seconds();

    std::fprintf(stdout,
        "{\"role\":\"pub\",\"transport\":\"ros_loan\",\"bytes\":%d,\"gpu\":%d,\"frames\":%d,\"cpu_s\":%.4f,\"wall_s\":%.4f,\"n_subs\":%d}\n",
        bytes, gpu, frames, cpu1 - cpu0, wall_s, nsubs);
    std::fflush(stdout);
    rclcpp::shutdown();
    return 0;
}

// ============ ROS loaned consumer ============
template<typename MsgT>
static SubResult run_sub_ros_loan(int bytes, int frames, int rate_hz, int gpu) {
    rclcpp::init(0, nullptr);
    char name[64];
    snprintf(name, sizeof(name), "bench_sub_%d", (int)getpid());
    auto node = rclcpp::Node::make_shared(name);
    auto sub = node->create_subscription<MsgT>(
        "/axon_bench",
        rclcpp::QoS(rclcpp::KeepLast(1)).best_effort().durability_volatile(),
        [](MsgT::ConstSharedPtr){});

    // This transport only means something on the zero-copy path: refuse to run without loans.
    if (!sub->can_loan_messages()) {
        std::fprintf(stderr, "can_loan_messages=0: no loaned take on this rmw/type, aborting\n");
        std::exit(2);
    }
    std::fprintf(stderr, "can_loan_messages=1\n");

    const int wait_on = env_wait() ? 1 : 0;

    SubResult res;
    std::uint64_t last_seqno = 0;
    std::vector<double> latencies;
    int seen = 0;

    auto start = std::chrono::steady_clock::now();
    double deadline = 10.0 + frames * (2.0 / rate_hz);
    double cpu_first = 0, cpu_last = 0;
    std::uint64_t wall_last = 0;
    std::uint64_t wall_first = 0;

    CUdeviceptr d_dst = 0;
    const bool kernel_read = gpu == 1 && axon_bench::consumer_kernel_enabled();
    axon_bench::GpuReadKernel reader;
    if (gpu == 1) {
        init_cuda_ctx();
        check_cuda(cuMemAlloc(&d_dst, bytes), "cuMemAlloc dst");
        if (kernel_read) reader.init();
    }

    rclcpp::WaitSet ws;
    if (wait_on) {
        ws.add_subscription(sub);
    }

    rcl_subscription_t* handle = sub->get_subscription_handle().get();
    while ((std::chrono::steady_clock::now() - start).count() / 1e9 < deadline) {
        if (wait_on) {
            auto wr = ws.wait(std::chrono::milliseconds(100));
            if (wr.kind() != rclcpp::WaitResultKind::Ready) continue;
        }
        void* loaned = nullptr;
        rmw_message_info_t info = rmw_get_zero_initialized_message_info();
        if (rcl_take_loaned_message(handle, &loaned, &info, nullptr) == RCL_RET_OK) {
            auto& msg = *static_cast<MsgT*>(loaned);
            if (gpu == 1) {
                check_cuda(cuMemcpyHtoD(d_dst, msg.data.data(), bytes), "cuMemcpyHtoD dst");
                if (kernel_read) reader.launch(d_dst, bytes, 0);  // read the frame on the GPU
                check_cuda(cuCtxSynchronize(), "cuCtxSynchronize");
            }
            std::uint64_t now = axon::rt_now_ns();
            std::uint64_t head_seq = read_head_seqno(msg.data.data(), msg.data.size());
            std::uint64_t tail_seq = read_tail_seqno(msg.data.data(), msg.data.size());
            std::uint64_t t0;
            std::memcpy(&t0, msg.data.data(), 8);
            if (kernel_read) { t0 = reader.result()[0]; head_seq = reader.result()[1]; tail_seq = reader.result()[2]; }
            if (head_seq != tail_seq) ++res.torn;
            if (head_seq > last_seqno) {
                ++seen;
                if (seen == 1) {
                    res.first_seq = head_seq;
                    cpu_first = cpu_seconds();
                    wall_first = axon::rt_now_ns();
                }
                if (seen > 20) latencies.push_back((now - t0) / 1000.0);
                last_seqno = head_seq;
                cpu_last = cpu_seconds();  // the CPU window ends at the last frame taken
                wall_last = axon::rt_now_ns();
            }
            if (rcl_return_loaned_message_from_subscription(handle, loaned) != RCL_RET_OK) {
                std::fprintf(stderr, "rcl_return_loaned_message_from_subscription failed\n");
                std::exit(1);
            }
        }
        if (seen >= frames || last_seqno >= (std::uint64_t)frames) break;
    }
    if (gpu == 1) check_cuda(cuMemFree(d_dst), "cuMemFree");

    res.frames_seen = seen;
    res.cpu_s = cpu_seconds();
    if (seen >= 1) {
        res.cpu_window_s = cpu_last - cpu_first;
        res.window_s = (wall_last - wall_first) / 1e9;
    }
    res.samples_us = std::move(latencies);
    print_sub_result("ros_loan", bytes, gpu, frames, res);
    rclcpp::shutdown();
    return res;
}

// ============ Axon producer ============
static int run_pub_axon(int bytes, int frames, int rate_hz, int gpu) {
    if (gpu == 1) init_cuda_ctx();

    // Ring depth: 16 by default. AXON_BENCH_NBUF overrides it (a deep ring of large
    // buffers falls out of the last-level cache; ROS 2 keep-last(1) reuses one or two).
    const char* nbuf_env = std::getenv("AXON_BENCH_NBUF");
    const std::size_t nbuf = nbuf_env ? std::strtoul(nbuf_env, nullptr, 10) : 16;
    auto pool = TensorPool::create(
        TensorPoolConfig{nbuf, (size_t)bytes,
                         gpu == 1 ? PoolBackend::Accelerator : PoolBackend::Custom, nullptr});
    auto pub = TensorPublisher::create("bench/axon", *pool);
    // Like the ROS 2 producers: wait (up to 10 s) for BENCH_NSUBS subscribers, then 500 ms.
    const int nsubs_wait = env_nsubs();
    int accepted = 0;
    const auto hs_start = std::chrono::steady_clock::now();
    while (accepted < nsubs_wait && std::chrono::steady_clock::now() - hs_start < std::chrono::seconds(10)) {
        const int n = pub->handshake_pool();
        if (n > 0) accepted += n;
    }
    if (nsubs_wait > 1) std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // BENCH_CPU_LATENCY_US=<us> holds axon::CpuLatencyHold for the run, on BENCH_CPU_LATENCY_CPU
    // (default -1 = system-wide): the API form of run_ring_perf.sh's PMQOS=1.
    std::optional<axon::CpuLatencyHold> latency_hold;
    if (const char* us = std::getenv("BENCH_CPU_LATENCY_US")) {
        const char* c = std::getenv("BENCH_CPU_LATENCY_CPU");
        latency_hold = axon::CpuLatencyHold::acquire(std::atoi(us), c ? std::atoi(c) : -1);
        if (!latency_hold) {
            std::fprintf(stderr, "CpuLatencyHold::acquire failed: %s\n", std::strerror(errno));
            std::exit(2);
        }
    }

    // BENCH_SRC_OFFSET shifts the host source by that many bytes (diagnosing 4 KiB aliasing between
    // source and destination); BENCH_ADDR_LOG prints both addresses modulo 4096 once.
    const std::size_t src_off = std::getenv("BENCH_SRC_OFFSET") ? std::strtoul(std::getenv("BENCH_SRC_OFFSET"), nullptr, 10) : 0;
    std::vector<uint8_t> src_buf(bytes + src_off);
    uint8_t* const srcp = src_buf.data() + src_off;
    for (int i = 0; i < (int)bytes; ++i) srcp[i] = (uint8_t)(i * 31 & 0xff);
    CUdeviceptr d_src = 0;
    if (gpu == 1) {
        check_cuda(cuMemAlloc(&d_src, bytes), "cuMemAlloc src");
        check_cuda(cuMemcpyHtoD(d_src, srcp, bytes), "cuMemcpyHtoD src");
    }

    // BENCH_FRAME_LOG=<path> (host only): per frame, the payload write's duration and the cycles it
    // took on this thread (perf_event_open, needs kernel.perf_event_paranoid <= 1), written at exit.
    const char* frame_log = gpu == 0 ? std::getenv("BENCH_FRAME_LOG") : nullptr;
    int cyc_fd = -1;
    if (frame_log) {
        perf_event_attr pa{};
        pa.type = PERF_TYPE_HARDWARE; pa.size = sizeof(pa); pa.config = PERF_COUNT_HW_CPU_CYCLES;
        cyc_fd = (int)syscall(SYS_perf_event_open, &pa, 0, -1, -1, 0);
        if (cyc_fd < 0) { std::perror("perf_event_open"); std::exit(2); }
    }
    auto read_cycles = [&] { std::uint64_t v = 0; if (::read(cyc_fd, &v, 8) != 8) v = 0; return v; };
    std::vector<std::uint64_t> fl_ns, fl_cyc;
    // ... and, after each publish, this CPU's cpuidle usage/time counters, so the log shows which
    // idle states the sleep before each frame went through (diff consecutive lines).
    std::vector<std::string> fl_idle;
    auto read_idle = [] {
        std::string out;
        const int cpu = sched_getcpu();
        for (int st = 0; st < 8; ++st) {
            for (const char* k : {"usage", "time"}) {
                char path[96];
                std::snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpuidle/state%d/%s", cpu, st, k);
                FILE* f = std::fopen(path, "r");
                if (!f) return out;
                unsigned long long v = 0;
                if (std::fscanf(f, "%llu", &v) != 1) v = 0;
                std::fclose(f);
                out += " " + std::to_string(v);
            }
        }
        return out;
    };
    // BENCH_PREWAKE_US=<us>: sleep until that long before each frame, then busy-wait to the frame time.
    const std::uint64_t prewake_ns = std::getenv("BENCH_PREWAKE_US") ? std::strtoull(std::getenv("BENCH_PREWAKE_US"), nullptr, 10) * 1000 : 0;

    struct timeval tv0, tv1;
    gettimeofday(&tv0, nullptr);
    double cpu0 = cpu_seconds();

    for (int seq_i = 1; seq_i <= frames; ++seq_i) {
        std::uint64_t t0 = axon::rt_now_ns();
        const std::uint64_t seq = static_cast<std::uint64_t>(seq_i);
        AcquiredDescriptor a = pub->acquire_descriptor();

        if (gpu == 0) {
            if (seq_i == 1 && std::getenv("BENCH_ADDR_LOG"))
                std::fprintf(stderr, "addr src%%4096=%zu dst%%4096=%zu\n", (std::size_t)(uintptr_t)srcp % 4096,
                             (std::size_t)(uintptr_t)a.host_view % 4096);
            const std::uint64_t c0 = frame_log ? read_cycles() : 0, w0 = axon::rt_now_ns();
            std::memcpy(a.host_view, srcp, bytes);
            if (frame_log) { fl_ns.push_back(axon::rt_now_ns() - w0); fl_cyc.push_back(read_cycles() - c0); }
            std::memcpy(a.host_view, &t0, 8);
            std::memcpy((uint8_t*)a.host_view + 8, &seq, 8);
            std::memcpy((uint8_t*)a.host_view + bytes - 8, &seq, 8);
        } else {
            CUdeviceptr dp = reinterpret_cast<CUdeviceptr>(pool->device_ptr(a.buffer_index));
            check_cuda(cuMemcpyDtoD(dp, d_src, bytes), "cuMemcpyDtoD");
            uint8_t header[16];
            std::memcpy(header, &t0, 8);
            std::memcpy(header + 8, &seq, 8);
            check_cuda(cuMemcpyHtoD(dp, header, 16), "cuMemcpyHtoD header");
            check_cuda(cuMemcpyHtoD(dp + bytes - 8, &seq, 8), "cuMemcpyHtoD tail");
            // DtoD is asynchronous to the host: publish only once the payload has landed.
            check_cuda(cuCtxSynchronize(), "cuCtxSynchronize");
        }

        a.desc->rank = 1;
        a.desc->shape[0] = (uint32_t)bytes;
        a.desc->dtype = DType::U8;
        a.desc->offset = 0;
        a.desc->size = (uint64_t)bytes;
        a.desc->sync_fence_kind = SyncFenceKind::None;

        pub->publish(std::move(a));
        if (frame_log) fl_idle.push_back(read_idle());
        const std::uint64_t next = t0 + (std::uint64_t)(1e9 / rate_hz);
        if (prewake_ns && next > prewake_ns) {  // wake early and spin, so the core is busy when the frame starts
            sleep_until(next - prewake_ns);
            while (axon::rt_now_ns() < next) {}
        } else {
            sleep_until(next);
        }
    }

    gettimeofday(&tv1, nullptr);
    double wall_s = (tv1.tv_sec - tv0.tv_sec) + (tv1.tv_usec - tv0.tv_usec) * 1e-6;
    double cpu1 = cpu_seconds();
    if (frame_log) {
        if (FILE* f = std::fopen(frame_log, "w")) {
            for (std::size_t i = 0; i < fl_ns.size(); ++i)
                std::fprintf(f, "%zu %llu %llu%s\n", i + 1, (unsigned long long)fl_ns[i], (unsigned long long)fl_cyc[i],
                             i < fl_idle.size() ? fl_idle[i].c_str() : "");
            std::fclose(f);
        }
    }

    const int nsubs = env_nsubs();

    std::fprintf(stdout,
        "{\"role\":\"pub\",\"transport\":\"axon\",\"bytes\":%d,\"gpu\":%d,\"frames\":%d,\"cpu_s\":%.4f,\"wall_s\":%.4f,\"n_subs\":%d}\n",
        bytes, gpu, frames, cpu1 - cpu0, wall_s, nsubs);
    std::fflush(stdout);
    return 0;
}

// ============ Axon consumer ============
static SubResult run_sub_axon(int bytes, int frames, int rate_hz, int gpu) {
    SubResult res;
    // BENCH_SAMPLE_LOG=<path> (host only): "seq latency_us" per timed frame, written at exit.
    const char* sample_log = gpu == 0 ? std::getenv("BENCH_SAMPLE_LOG") : nullptr;
    std::vector<std::pair<std::uint64_t, double>> sl;

    if (gpu == 1) init_cuda_ctx();

    const int wait_on = env_wait() ? 1 : 0;

    auto sub = TensorSubscriber::create("bench/axon");
    int rc = sub->wait_handshake(15000);
    if (rc != 0) {
        std::fprintf(stderr, "axon handshake failed with %d\n", rc);
        res.cpu_s = cpu_seconds();
        res.samples_us = {};
        print_sub_result("axon", bytes, gpu, frames, res);
        return res;
    }

    std::uint64_t last_seqno = 0;
    std::vector<double> latencies;
    int seen = 0;

    auto start = std::chrono::steady_clock::now();
    double deadline = 10.0 + frames * (2.0 / rate_hz);
    double cpu_first = 0, cpu_last = 0;
    std::uint64_t wall_last = 0;
    std::uint64_t wall_first = 0;

    if (gpu == 0) {
        while ((std::chrono::steady_clock::now() - start).count() / 1e9 < deadline) {
            if (wait_on) {
                int r = sub->wait_for_update(100);
                if (r <= 0) continue;
            }
            auto v = sub->latest_view(8);
            if (v && v->seqno > last_seqno && v->data != nullptr) {
                std::uint64_t now = axon::rt_now_ns();
                std::uint64_t head_seq = read_head_seqno((const uint8_t*)v->data, v->shape.dims[0]);
                std::uint64_t tail_seq = read_tail_seqno((const uint8_t*)v->data, v->shape.dims[0]);
                if (head_seq != tail_seq) ++res.torn;
                std::uint64_t t0;
                std::memcpy(&t0, v->data, 8);
                if (head_seq > last_seqno) {
                    ++seen;
                    if (seen == 1) {
                        res.first_seq = head_seq;
                        cpu_first = cpu_seconds();
                        wall_first = axon::rt_now_ns();
                    }
                    if (seen > 20) latencies.push_back((now - t0) / 1000.0);
                    if (seen > 20 && sample_log) sl.emplace_back(head_seq, (now - t0) / 1000.0);
                    last_seqno = head_seq;
                    cpu_last = cpu_seconds();  // the CPU window ends at the last frame taken
                    wall_last = axon::rt_now_ns();
                }
            }
            if (seen >= frames || last_seqno >= (std::uint64_t)frames) break;
        }
    } else {
        if (gpu == 1) init_cuda_ctx();
        const bool kernel_read = axon_bench::consumer_kernel_enabled();
        axon_bench::GpuReadKernel reader;
        if (kernel_read) reader.init();
        while ((std::chrono::steady_clock::now() - start).count() / 1e9 < deadline) {
            if (wait_on) {
                int r = sub->wait_for_update(100);
                if (r <= 0) continue;
            }
            auto v = sub->latest_view(8);
            if (v && v->seqno > last_seqno && v->device_ptr != nullptr) {
                uint8_t hdr[16];
                CUdeviceptr dp = reinterpret_cast<CUdeviceptr>(v->device_ptr);
                uint8_t tail[8];
                if (kernel_read) {  // read the frame on the GPU instead of copying it back
                    reader.launch(dp, v->shape.dims[0], 0);
                    check_cuda(cuCtxSynchronize(), "cuCtxSynchronize");
                    const std::uint64_t r[3] = {reader.result()[0], reader.result()[1], reader.result()[2]};
                    std::memcpy(hdr, r, 16);
                    std::memcpy(tail, r + 2, 8);
                } else {
                    check_cuda(cuMemcpyDtoH(hdr, dp, 16), "cuMemcpyDtoH hdr");
                    check_cuda(cuMemcpyDtoH(tail, dp + v->shape.dims[0] - 8, 8), "cuMemcpyDtoH tail");
                }

                std::uint64_t head_seq, tail_seq, t0;
                std::memcpy(&t0, hdr, 8);
                std::memcpy(&head_seq, hdr + 8, 8);
                std::memcpy(&tail_seq, tail, 8);

                std::uint64_t now = axon::rt_now_ns();
                if (head_seq != tail_seq) { ++res.torn; continue; }  // not timed, as in the other cells
                if (head_seq > last_seqno) {
                    ++seen;
                    if (seen == 1) {
                        res.first_seq = head_seq;
                        cpu_first = cpu_seconds();
                        wall_first = axon::rt_now_ns();
                    }
                    if (seen > 20) latencies.push_back((now - t0) / 1000.0);
                    last_seqno = head_seq;
                    cpu_last = cpu_seconds();  // the CPU window ends at the last frame taken
                    wall_last = axon::rt_now_ns();
                }
            }
            if (seen >= frames || last_seqno >= (std::uint64_t)frames) break;
        }
    }

    res.frames_seen = seen;
    res.cpu_s = cpu_seconds();
    if (seen >= 1) {
        res.cpu_window_s = cpu_last - cpu_first;
        res.window_s = (wall_last - wall_first) / 1e9;
    }
    res.samples_us = std::move(latencies);
    if (sample_log) {
        if (FILE* f = std::fopen(sample_log, "w")) {
            for (auto& [q, us] : sl) std::fprintf(f, "%llu %.1f\n", (unsigned long long)q, us);
            std::fclose(f);
        }
    }
    print_sub_result("axon", bytes, gpu, frames, res);
    return res;
}

// ============ Dispatch ============
static int dispatch_pub(const char* transport_str, int bytes, int frames, int rate_hz, int gpu) {
    if (strcmp(transport_str, "ros_dyn") == 0) return run_pub_ros_dyn(bytes, frames, rate_hz, gpu);
    if (strcmp(transport_str, "ros_loan") == 0) {
#define X(n, T) if (bytes == n) return run_pub_ros_loan<axon_bench::msg::T>(bytes, frames, rate_hz, gpu);
        AXON_BENCH_SIZES(X)
#undef X
    }
    if (strcmp(transport_str, "axon") == 0) return run_pub_axon(bytes, frames, rate_hz, gpu);
    std::fprintf(stderr, "unknown transport: %s\n", transport_str);
    return 1;
}

static SubResult dispatch_sub(const char* transport_str, int bytes, int frames, int rate_hz, int gpu) {
    if (strcmp(transport_str, "ros_dyn") == 0) return run_sub_ros_dyn(bytes, frames, rate_hz, gpu);
    if (strcmp(transport_str, "ros_loan") == 0) {
#define X(n, T) if (bytes == n) return run_sub_ros_loan<axon_bench::msg::T>(bytes, frames, rate_hz, gpu);
        AXON_BENCH_SIZES(X)
#undef X
    }
    if (strcmp(transport_str, "axon") == 0) return run_sub_axon(bytes, frames, rate_hz, gpu);
    std::fprintf(stderr, "unknown transport: %s\n", transport_str);
    SubResult r; return r;
}

int main(int argc, char** argv) {
    if (argc != 7) {
        std::fprintf(stderr, "Usage: bench <role> <transport> <bytes> <frames> <rate_hz> <gpu>\n");
        std::fprintf(stderr, "  role: pub|sub\n");
        std::fprintf(stderr, "  transport: ros_dyn|ros_loan|axon\n");
        std::fprintf(stderr, "  bytes: 64|8640|150528|1048576|4194304|16777216|67108864\n");
        std::fprintf(stderr, "  gpu: 0|1\n");
        return 1;
    }

    const char* role = argv[1];
    const char* transport = argv[2];
    int bytes = atoi(argv[3]);
    int frames = atoi(argv[4]);
    int rate_hz = atoi(argv[5]);
    int gpu = atoi(argv[6]);

    bool known = false;
#define X(n, T) known = known || bytes == n;
    AXON_BENCH_SIZES(X)
#undef X
    if (!known) {
        std::fprintf(stderr, "bytes must be one of 64, 8640, 150528, 1048576, 4194304, 16777216, 67108864\n");
        return 1;
    }
    if (gpu != 0 && gpu != 1) {
        std::fprintf(stderr, "gpu must be 0 or 1\n");
        return 1;
    }

    if (strcmp(role, "pub") == 0) {
        return dispatch_pub(transport, bytes, frames, rate_hz, gpu);
    } else if (strcmp(role, "sub") == 0) {
        (void)dispatch_sub(transport, bytes, frames, rate_hz, gpu);
        return 0;
    } else {
        std::fprintf(stderr, "role must be pub or sub\n");
        return 1;
    }
}
