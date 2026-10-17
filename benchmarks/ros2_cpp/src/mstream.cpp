// SPDX-License-Identifier: Apache-2.0
// mstream — multi-stream CPU benchmark: the benchmarks/mock robot workload (4 cameras/depth +
// LiDAR + IMU + odometry + cmd_vel, ×scale on the heavy streams) over rclcpp (UInt8MultiArray),
// rclcpp loaned messages + Fast DDS data-sharing, and axon. Each consumer runs in its natural
// usage model: ROS 2 an event-driven executor, axon a 1 kHz loop reading the latest of every
// stream. See README.md.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <dirent.h>
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/u_int8_multi_array.hpp>

#include "axon_bench/msg/payload48.hpp"
#include "axon_bench/msg/payload64.hpp"
#include "axon_bench/msg/payload614400.hpp"
#include "axon_bench/msg/payload8640.hpp"

#include "axon/pool.h"
#include "axon/publisher.h"
#include "axon/rt.h"
#include "axon/subscriber.h"

using namespace axon;

namespace {

struct Stream {
    std::string name;
    std::size_t bytes;
    int hz;
    bool heavy;  // camera/depth: replicated by scale, latency sampled
};

// Same profile as benchmarks/mock/mock_common.py.
std::vector<Stream> make_streams(int scale) {
    struct Base { const char* name; std::size_t bytes; int hz; bool heavy; };
    static const Base base[] = {
        {"cam_front_rgb", 614400, 30, true}, {"cam_left_rgb", 614400, 30, true},
        {"cam_right_rgb", 614400, 30, true}, {"depth_front", 614400, 30, true},
        {"lidar_2d", 8640, 15, false},       {"imu", 64, 200, false},
        {"wheel_odom", 48, 100, false},      {"cmd_vel", 48, 50, false},
    };
    std::vector<Stream> out;
    for (const Base& b : base) {
        if (b.heavy && scale > 1)
            for (int k = 0; k < scale; ++k)
                out.push_back({std::string(b.name) + "_" + std::to_string(k), b.bytes, b.hz, true});
        else
            out.push_back({b.name, b.bytes, b.hz, b.heavy});
    }
    return out;
}

double cpu_seconds() {
    rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    return ru.ru_utime.tv_sec + ru.ru_stime.tv_sec + (ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) * 1e-6;
}

void sleep_until_ns(std::uint64_t target) {
    std::uint64_t now = rt_now_ns();
    if (now >= target) return;
    std::uint64_t d = target - now;
    timespec ts{static_cast<time_t>(d / 1000000000ull), static_cast<long>(d % 1000000000ull)};
    nanosleep(&ts, nullptr);
}

// Cache misses + instructions over a window, summed over every thread of this process that
// exists when start() runs (rclcpp / Fast DDS threads are created before the window opens).
// Enabled with MSTREAM_PERF=1; needs kernel.perf_event_paranoid <= 1. -1 when unavailable.
struct Counters {
    std::vector<int> fds[2];
    bool on = std::getenv("MSTREAM_PERF") != nullptr;

    static int open_one(pid_t tid, std::uint64_t config) {
        perf_event_attr a{};
        a.size = sizeof(a);
        a.type = PERF_TYPE_HARDWARE;
        a.config = config;
        a.disabled = 1;
        int fd = static_cast<int>(syscall(SYS_perf_event_open, &a, tid, -1, -1, 0));
        if (fd < 0) {  // retry without kernel-side counting
            a.exclude_kernel = 1;
            fd = static_cast<int>(syscall(SYS_perf_event_open, &a, tid, -1, -1, 0));
        }
        return fd;
    }
    void start() {
        if (!on) return;
        DIR* d = opendir("/proc/self/task");
        if (!d) return;
        while (dirent* e = readdir(d)) {
            const pid_t tid = std::atoi(e->d_name);
            if (tid <= 0) continue;
            const std::uint64_t cfg[2] = {PERF_COUNT_HW_CACHE_MISSES, PERF_COUNT_HW_INSTRUCTIONS};
            for (int k = 0; k < 2; ++k) {
                const int fd = open_one(tid, cfg[k]);
                if (fd < 0) continue;
                ioctl(fd, PERF_EVENT_IOC_RESET, 0);
                ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);
                fds[k].push_back(fd);
            }
        }
        closedir(d);
    }
    // {cache_misses, instructions}
    std::pair<long long, long long> stop() {
        long long v[2] = {-1, -1};
        for (int k = 0; k < 2; ++k) {
            if (fds[k].empty()) continue;
            v[k] = 0;
            for (int fd : fds[k]) {
                ioctl(fd, PERF_EVENT_IOC_DISABLE, 0);
                long long x = 0;
                if (read(fd, &x, sizeof(x)) == sizeof(x)) v[k] += x;
                close(fd);
            }
            fds[k].clear();
        }
        return {v[0], v[1]};
    }
};

double percentile(std::vector<double> v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    std::size_t rank = static_cast<std::size_t>(std::ceil(p / 100.0 * v.size()));
    return v[std::clamp<std::size_t>(rank, 1, v.size()) - 1];
}

// Payload: [0,8) publish ts, [8,16) seqno, last 8 bytes seqno again.
void stamp(std::uint8_t* p, std::size_t n, std::uint64_t t0, std::uint64_t seq) {
    std::memcpy(p, &t0, 8);
    std::memcpy(p + 8, &seq, 8);
    std::memcpy(p + n - 8, &seq, 8);
}

rclcpp::QoS qos() { return rclcpp::QoS(rclcpp::KeepLast(1)).best_effort().durability_volatile(); }

// ---------------------------------------------------------------- producer

// send(i, t0, seq) writes the whole payload of stream i and publishes it.
using SendFn = std::function<void(std::size_t, std::uint64_t, std::uint64_t)>;

int run_producer(const char* transport, int scale, const std::vector<Stream>& streams,
                 int seconds, const SendFn& send) {
    // One thread, earliest-deadline-first over all streams. Runs 1.5 s longer than the
    // consumer's window, which starts at the first frame it sees.
    const std::uint64_t start = rt_now_ns();
    const std::uint64_t end = start + static_cast<std::uint64_t>(seconds) * 1000000000ull + 1500000000ull;
    std::vector<std::uint64_t> due(streams.size(), start), seq(streams.size(), 0);
    if (std::getenv("MSTREAM_STAGGER"))  // spread first deadlines over one period (no bursts)
        for (std::size_t i = 0; i < streams.size(); ++i)
            due[i] += (1000000000ull / streams[i].hz) * i / streams.size();
    std::uint64_t sent = 0, bytes = 0;
    Counters ctr;
    ctr.start();
    const double cpu0 = cpu_seconds();
    for (;;) {
        std::size_t i = std::min_element(due.begin(), due.end()) - due.begin();
        if (due[i] >= end) break;
        sleep_until_ns(due[i]);
        send(i, rt_now_ns(), ++seq[i]);
        due[i] += 1000000000ull / streams[i].hz;
        ++sent;
        bytes += streams[i].bytes;
    }
    const double wall = (rt_now_ns() - start) * 1e-9, cpu = cpu_seconds() - cpu0;
    const auto [cm, ins] = ctr.stop();
    std::printf("{\"role\":\"pub\",\"transport\":\"%s\",\"scale\":%d,\"streams\":%zu,\"offered_MBps\":%.1f,"
                "\"cpu_s\":%.4f,\"wall_s\":%.3f,\"sent\":%llu,\"cache_misses\":%lld,\"instructions\":%lld}\n",
                transport, scale, streams.size(), bytes / wall / 1e6, cpu, wall,
                static_cast<unsigned long long>(sent), cm, ins);
    std::fflush(stdout);
    return 0;
}

void wait_for_subscribers(const std::function<bool()>& all_matched) {
    for (int k = 0; k < 200 && !all_matched(); ++k) rclcpp::sleep_for(std::chrono::milliseconds(50));
    rclcpp::sleep_for(std::chrono::milliseconds(500));  // discovery settle
}

int pub_ros_dyn(int scale, const std::vector<Stream>& streams, int seconds) {
    auto node = rclcpp::Node::make_shared("mstream_pub");
    std::vector<rclcpp::Publisher<std_msgs::msg::UInt8MultiArray>::SharedPtr> pubs;
    std::vector<std_msgs::msg::UInt8MultiArray> msgs(streams.size());
    std::vector<std::vector<std::uint8_t>> src(streams.size());
    for (std::size_t i = 0; i < streams.size(); ++i) {
        pubs.push_back(node->create_publisher<std_msgs::msg::UInt8MultiArray>("/ms/" + streams[i].name, qos()));
        msgs[i].data.resize(streams[i].bytes);
        src[i].resize(streams[i].bytes);
        for (std::size_t j = 0; j < src[i].size(); ++j) src[i][j] = static_cast<std::uint8_t>(j * 31);
    }
    wait_for_subscribers([&] {
        return std::all_of(pubs.begin(), pubs.end(), [](auto& p) { return p->get_subscription_count() > 0; });
    });
    return run_producer("ros_dyn", scale, streams, seconds, [&](std::size_t i, std::uint64_t t0, std::uint64_t s) {
        std::memcpy(msgs[i].data.data(), src[i].data(), streams[i].bytes);
        stamp(msgs[i].data.data(), streams[i].bytes, t0, s);
        pubs[i]->publish(msgs[i]);
    });
}

// Loaned-message publisher for one stream, typed by its fixed size.
struct LoanPub {
    std::function<bool()> matched;
    std::function<bool()> can_loan;
    std::function<void(const std::uint8_t*, std::uint64_t, std::uint64_t)> send;
};

template <typename MsgT>
LoanPub make_loan_pub(rclcpp::Node& node, const std::string& topic) {
    auto pub = node.create_publisher<MsgT>(topic, qos());
    return {[pub] { return pub->get_subscription_count() > 0; },
            [pub] { return pub->can_loan_messages(); },
            [pub](const std::uint8_t* src, std::uint64_t t0, std::uint64_t s) {
                auto loaned = pub->borrow_loaned_message();
                auto& data = loaned.get().data;
                std::memcpy(data.data(), src, data.size());
                stamp(data.data(), data.size(), t0, s);
                pub->publish(std::move(loaned));
            }};
}

LoanPub make_loan_pub_for(rclcpp::Node& node, const Stream& st) {
    const std::string topic = "/ms/" + st.name;
    switch (st.bytes) {
        case 614400: return make_loan_pub<axon_bench::msg::Payload614400>(node, topic);
        case 8640:   return make_loan_pub<axon_bench::msg::Payload8640>(node, topic);
        case 64:     return make_loan_pub<axon_bench::msg::Payload64>(node, topic);
        case 48:     return make_loan_pub<axon_bench::msg::Payload48>(node, topic);
    }
    std::fprintf(stderr, "no fixed-size message type for %zu bytes\n", st.bytes);
    std::exit(1);
}

int pub_ros_loan(int scale, const std::vector<Stream>& streams, int seconds) {
    auto node = rclcpp::Node::make_shared("mstream_pub");
    std::vector<LoanPub> pubs;
    std::vector<std::vector<std::uint8_t>> src(streams.size());
    for (std::size_t i = 0; i < streams.size(); ++i) {
        pubs.push_back(make_loan_pub_for(*node, streams[i]));
        if (!pubs.back().can_loan()) {
            std::fprintf(stderr, "ros_loan: %s cannot loan messages, aborting\n", streams[i].name.c_str());
            std::exit(2);
        }
        src[i].resize(streams[i].bytes);
        for (std::size_t j = 0; j < src[i].size(); ++j) src[i][j] = static_cast<std::uint8_t>(j * 31);
    }
    wait_for_subscribers([&] {
        return std::all_of(pubs.begin(), pubs.end(), [](auto& p) { return p.matched(); });
    });
    return run_producer("ros_loan", scale, streams, seconds, [&](std::size_t i, std::uint64_t t0, std::uint64_t s) {
        pubs[i].send(src[i].data(), t0, s);
    });
}

int pub_axon(int scale, const std::vector<Stream>& streams, int seconds) {
    std::vector<std::unique_ptr<TensorPool>> pools;
    std::vector<std::unique_ptr<TensorPublisher>> pubs;
    std::vector<std::vector<std::uint8_t>> src(streams.size());
    for (std::size_t i = 0; i < streams.size(); ++i) {
        pools.push_back(TensorPool::create(TensorPoolConfig{4, streams[i].bytes, PoolBackend::Custom, nullptr}));
        pubs.push_back(TensorPublisher::create("ms/" + streams[i].name, *pools.back()));
        if (pubs.back()->handshake_pool() < 1) {  // the consumer handshakes in the same order
            std::fprintf(stderr, "axon: no subscriber for %s\n", streams[i].name.c_str());
            std::exit(11);
        }
        src[i].resize(streams[i].bytes);
        for (std::size_t j = 0; j < src[i].size(); ++j) src[i][j] = static_cast<std::uint8_t>(j * 31);
    }
    return run_producer("axon", scale, streams, seconds, [&](std::size_t i, std::uint64_t t0, std::uint64_t s) {
        AcquiredDescriptor a = pubs[i]->acquire_descriptor();
        auto* p = static_cast<std::uint8_t*>(a.host_view);
        std::memcpy(p, src[i].data(), streams[i].bytes);
        stamp(p, streams[i].bytes, t0, s);
        a.desc->rank = 1;
        a.desc->shape[0] = static_cast<std::uint32_t>(streams[i].bytes);
        a.desc->dtype = DType::U8;
        a.desc->offset = 0;
        a.desc->size = streams[i].bytes;
        a.desc->sync_fence_kind = SyncFenceKind::None;
        pubs[i]->publish(std::move(a));
    });
}

// ---------------------------------------------------------------- consumer

// Counts frames and samples latency inside a window that opens at the first frame and
// lasts `seconds`; CPU is measured over the same window.
struct Tally {
    const std::vector<Stream>& streams;
    std::uint64_t window_ns;
    std::vector<std::uint64_t> last_seq, first_seq, seen;
    std::vector<double> cam_us;
    std::uint64_t torn = 0, open_ns = 0;
    double cpu_open = 0;
    Counters ctr;

    Tally(const std::vector<Stream>& s, int seconds)
        : streams(s), window_ns(static_cast<std::uint64_t>(seconds) * 1000000000ull),
          last_seq(s.size(), 0), first_seq(s.size(), 0), seen(s.size(), 0) {}

    bool open() const { return open_ns != 0; }
    bool done() const { return open_ns != 0 && rt_now_ns() >= open_ns + window_ns; }

    void frame(std::size_t i, const std::uint8_t* p, std::size_t n) {
        const std::uint64_t now = rt_now_ns();
        if (!open()) { open_ns = now; ctr.start(); cpu_open = cpu_seconds(); }
        if (now >= open_ns + window_ns) return;
        std::uint64_t t0, head, tail;
        std::memcpy(&t0, p, 8);
        std::memcpy(&head, p + 8, 8);
        std::memcpy(&tail, p + n - 8, 8);
        if (head != tail) { ++torn; return; }
        if (head <= last_seq[i]) return;
        if (first_seq[i] == 0) first_seq[i] = head;
        last_seq[i] = head;
        ++seen[i];
        if (streams[i].heavy) cam_us.push_back((now - t0) / 1000.0);
    }

    int report(const char* transport, int scale, int seconds) {
        if (std::getenv("MSTREAM_DEBUG"))
            for (std::size_t i = 0; i < streams.size(); ++i)
                std::fprintf(stderr, "%s seen=%llu expected=%d last_seq=%llu\n", streams[i].name.c_str(),
                             static_cast<unsigned long long>(seen[i]), streams[i].hz * seconds,
                             static_cast<unsigned long long>(last_seq[i]));
        const double cpu = cpu_seconds() - cpu_open, wall = (rt_now_ns() - open_ns) * 1e-9;
        const auto [cm, ins] = ctr.stop();
        double worst = 2.0;
        std::size_t worst_i = 0;
        std::uint64_t total = 0;
        for (std::size_t i = 0; i < streams.size(); ++i) {
            // delivery = distinct frames seen / frames sent in the window (seqno span)
            const std::uint64_t span = seen[i] ? last_seq[i] - first_seq[i] + 1 : streams[i].hz * seconds;
            const double d = static_cast<double>(seen[i]) / span;
            if (d < worst) { worst = d; worst_i = i; }
            total += seen[i];
        }
        std::printf("{\"role\":\"sub\",\"transport\":\"%s\",\"scale\":%d,\"cpu_s\":%.4f,\"wall_s\":%.3f,"
                    "\"seen\":%llu,\"torn\":%llu,\"worst_stream_delivery\":%.3f,"
                    "\"worst_stream\":\"%s\",\"cam_p50_us\":%.1f,\"cam_p99_us\":%.1f,"
                    "\"cache_misses\":%lld,\"instructions\":%lld}\n",
                    transport, scale, cpu, wall, static_cast<unsigned long long>(total),
                    static_cast<unsigned long long>(torn), worst, streams[worst_i].name.c_str(),
                    percentile(cam_us, 50), percentile(cam_us, 99), cm, ins);
        std::fflush(stdout);
        return open() ? 0 : 3;  // 3: never saw a frame
    }
};

// ROS 2's usage model: an event-driven executor; spin_once blocks until work arrives.
void spin_until_done(const rclcpp::Node::SharedPtr& node, const Tally& t) {
    rclcpp::executors::SingleThreadedExecutor exec;
    exec.add_node(node);
    const std::uint64_t give_up = rt_now_ns() + 20000000000ull;  // no producer within 20 s
    while (rclcpp::ok() && !t.done() && (t.open() || rt_now_ns() < give_up))
        exec.spin_once(std::chrono::milliseconds(50));
}

int sub_ros_dyn(int scale, const std::vector<Stream>& streams, int seconds) {
    auto node = rclcpp::Node::make_shared("mstream_sub_ros_dyn");
    Tally t(streams, seconds);
    std::vector<rclcpp::SubscriptionBase::SharedPtr> subs;
    for (std::size_t i = 0; i < streams.size(); ++i)
        subs.push_back(node->create_subscription<std_msgs::msg::UInt8MultiArray>(
            "/ms/" + streams[i].name, qos(), [&t, i](std_msgs::msg::UInt8MultiArray::ConstSharedPtr m) {
                t.frame(i, m->data.data(), m->data.size());
            }));
    spin_until_done(node, t);
    return t.report("ros_dyn", scale, seconds);
}

template <typename MsgT>
rclcpp::SubscriptionBase::SharedPtr make_loan_sub(rclcpp::Node& node, const std::string& topic,
                                                  Tally& t, std::size_t i) {
    // const-ref callback: rclcpp serves it from a loaned message when loans are enabled.
    return node.create_subscription<MsgT>(topic, qos(), [&t, i](const MsgT& m) {
        t.frame(i, m.data.data(), m.data.size());
    });
}

int sub_ros_loan(int scale, const std::vector<Stream>& streams, int seconds) {
    auto node = rclcpp::Node::make_shared("mstream_sub_ros_loan");
    Tally t(streams, seconds);
    std::vector<rclcpp::SubscriptionBase::SharedPtr> subs;
    for (std::size_t i = 0; i < streams.size(); ++i) {
        const std::string topic = "/ms/" + streams[i].name;
        switch (streams[i].bytes) {
            case 614400: subs.push_back(make_loan_sub<axon_bench::msg::Payload614400>(*node, topic, t, i)); break;
            case 8640:   subs.push_back(make_loan_sub<axon_bench::msg::Payload8640>(*node, topic, t, i)); break;
            case 64:     subs.push_back(make_loan_sub<axon_bench::msg::Payload64>(*node, topic, t, i)); break;
            case 48:     subs.push_back(make_loan_sub<axon_bench::msg::Payload48>(*node, topic, t, i)); break;
            default: std::fprintf(stderr, "no fixed-size type for %zu bytes\n", streams[i].bytes); return 1;
        }
        if (!subs.back()->can_loan_messages()) {
            std::fprintf(stderr, "ros_loan: %s cannot loan messages, aborting\n", streams[i].name.c_str());
            return 2;
        }
    }
    spin_until_done(node, t);
    return t.report("ros_loan", scale, seconds);
}

int sub_axon(int scale, const std::vector<Stream>& streams, int seconds) {
    std::vector<std::unique_ptr<TensorSubscriber>> subs;
    for (const Stream& s : streams) {
        subs.push_back(TensorSubscriber::create("ms/" + s.name));
        if (subs.back()->wait_handshake(15000) != 0) {
            std::fprintf(stderr, "axon: handshake failed for %s\n", s.name.c_str());
            return 11;
        }
    }
    Tally t(streams, seconds);
    // axon's usage model: a 1 kHz loop reads the latest of every stream each tick.
    std::uint64_t tick = rt_now_ns();
    const std::uint64_t give_up = tick + 20000000000ull;
    while (!t.done() && (t.open() || rt_now_ns() < give_up)) {
        for (std::size_t i = 0; i < streams.size(); ++i) {
            auto v = subs[i]->latest_view(8);
            if (v && v->data && v->seqno > t.last_seq[i])
                t.frame(i, static_cast<const std::uint8_t*>(v->data), streams[i].bytes);
        }
        tick += 1000000;
        sleep_until_ns(tick);
    }
    return t.report("axon", scale, seconds);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 5) {
        std::fprintf(stderr, "usage: mstream <pub|sub> <ros_dyn|ros_loan|axon> <scale> <seconds>\n");
        return 1;
    }
    const std::string role = argv[1], tr = argv[2];
    const int scale = std::max(1, std::atoi(argv[3])), seconds = std::max(1, std::atoi(argv[4]));
    const auto streams = make_streams(scale);
    const bool ros = tr != "axon";
    if (ros) rclcpp::init(0, nullptr);
    int rc = 1;
    if (role == "pub")
        rc = tr == "ros_dyn" ? pub_ros_dyn(scale, streams, seconds)
           : tr == "ros_loan" ? pub_ros_loan(scale, streams, seconds)
           : tr == "axon" ? pub_axon(scale, streams, seconds) : 1;
    else if (role == "sub")
        rc = tr == "ros_dyn" ? sub_ros_dyn(scale, streams, seconds)
           : tr == "ros_loan" ? sub_ros_loan(scale, streams, seconds)
           : tr == "axon" ? sub_axon(scale, streams, seconds) : 1;
    if (ros) rclcpp::shutdown();
    return rc;
}
