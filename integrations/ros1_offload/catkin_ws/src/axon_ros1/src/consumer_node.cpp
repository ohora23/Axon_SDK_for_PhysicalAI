// SPDX-License-Identifier: Apache-2.0
// axon ROS1 offload — consumer node.
//
// Receives the pool dma-buf FDs ONCE via the axon SCM_RIGHTS sidecar and mmaps
// them. Then, for every TensorDescriptor that arrives on the ROS1 topic, it
// reads the payload straight from the shared buffer (zero copy) and checks the
// seqno the producer stamped there. Also demonstrates the ROS pattern this
// enables: a per-message watchdog on the descriptor topic, plus a precise
// staleness (now - producer_publish_ts) available on every message.
//
// The sidecar + mmap bootstrap lives in the reusable, ROS-agnostic
// axon_bridge::Consumer; this node is just the ROS glue over it.

#include <atomic>
#include <cstring>
#include <memory>

#include <ros/ros.h>

#include "axon/rt.h"
#include "axon_ros1/axon_bridge.h"
#include "axon_ros1/TensorDescriptor.h"

namespace {
std::unique_ptr<axon_bridge::Consumer> g_bridge;
std::atomic<uint64_t> g_last_seen{0};
uint64_t g_frames = 0, g_payload_errors = 0;
uint64_t g_stale_sum_us = 0, g_stale_max_us = 0;

void on_desc(const axon_ros1::TensorDescriptor::ConstPtr& msg) {
    g_last_seen.store(msg->seqno, std::memory_order_relaxed);

    if (msg->pool_generation != g_bridge->pool_generation()) {
        ROS_WARN_THROTTLE(1.0, "pool_generation mismatch (%u vs %u) — re-handshake needed",
                          msg->pool_generation, g_bridge->pool_generation());
        return;
    }

    // Zero-copy read: the payload never travelled through ROS.
    const void* base = g_bridge->payload(msg->bo_handle, msg->offset);
    if (!base) return;
    uint64_t stamped = 0;
    std::memcpy(&stamped, base, sizeof(stamped));
    if (stamped != msg->seqno) ++g_payload_errors;

    uint64_t staleness_us = (axon::rt_now_ns() - msg->producer_publish_ts_ns) / 1000;
    g_stale_sum_us += staleness_us;
    if (staleness_us > g_stale_max_us) g_stale_max_us = staleness_us;
    ++g_frames;

    if (msg->seqno % 30 == 0)
        ROS_INFO("seqno=%lu buf=%lu payload_ok=%s staleness=%luus (payload via dma-buf, 0 copy)",
                 msg->seqno, msg->bo_handle, stamped == msg->seqno ? "yes" : "NO", staleness_us);
}
}  // namespace

int main(int argc, char** argv) {
    ros::init(argc, argv, "axon_consumer");
    ros::NodeHandle nh("~");
    std::string service = nh.param<std::string>("service", "ros1_offload");
    double watchdog_s = nh.param("watchdog_s", 0.5);

    // axon FD plane: connect the sidecar and receive + mmap the pool FDs once.
    g_bridge = axon_bridge::Consumer::create(service, 10000);
    if (!g_bridge) { ROS_FATAL("sidecar handshake failed (is the producer up?)"); return 1; }
    ROS_INFO("sidecar handshake: %zu dma-buf FDs, %zu B each, pool_gen=%u — attached.",
             g_bridge->buffer_count(), g_bridge->buffer_size(), g_bridge->pool_generation());

    ros::Subscriber sub = nh.subscribe("/axon/tensor_desc", 50, on_desc);

    // Watchdog on the descriptor topic — exactly the ROS pattern that keeps
    // working because descriptors still flow at full rate.
    uint64_t wd_last = 0;
    ros::Timer wd = nh.createTimer(ros::Duration(watchdog_s),
        [&](const ros::TimerEvent&) {
            uint64_t now = g_last_seen.load(std::memory_order_relaxed);
            if (now == wd_last && now != 0)
                ROS_WARN("watchdog: no new descriptor in %.1fs (seqno stuck at %lu)",
                         watchdog_s, now);
            wd_last = now;
        });

    ROS_INFO("consumer ready — reading payload zero-copy per descriptor.");
    ros::spin();

    // Use stdout (ROS_INFO is suppressed after shutdown) so the summary always prints.
    if (g_frames) {
        std::printf("\n────── axon ROS1 offload — consumer summary ──────\n"
                    "  frames read:      %lu\n"
                    "  payload errors:   %lu   (must be 0 — cross-process zero-copy)\n"
                    "  staleness:        mean=%luus  max=%luus\n"
                    "  payload path:     shared dma-buf (never serialized through ROS)\n"
                    "──────────────────────────────────────────────────\n",
                    g_frames, g_payload_errors, g_stale_sum_us / g_frames, g_stale_max_us);
        std::fflush(stdout);
    }
    g_bridge.reset();   // munmap the views + close the sidecar client
    return 0;
}
