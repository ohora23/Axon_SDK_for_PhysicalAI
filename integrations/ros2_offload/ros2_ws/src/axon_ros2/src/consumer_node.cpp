// SPDX-License-Identifier: Apache-2.0
// axon ROS2 offload — consumer node.
//
// Receives the pool dma-buf FDs ONCE via the axon SCM_RIGHTS sidecar and mmaps
// them. Then, for every TensorDescriptor that arrives on the ROS2 topic, it reads
// the payload straight from the shared buffer (zero copy) and checks the seqno the
// producer stamped there. Every message also carries a precise, bounded staleness
// (now - producer_publish_ts) — the RT freshness guarantee that plain DDS /
// rmw_iceoryx does not provide.
//
// The sidecar + mmap bootstrap lives in the reusable, ROS-agnostic
// axon_bridge::Consumer (the SAME header the ROS1 wrapper uses).
//
// Exits after `frames` descriptors so the demo can assert a clean summary.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>

#include <rclcpp/rclcpp.hpp>

#include "axon/rt.h"
#include "axon_ros2/axon_bridge.h"
#include "axon_ros2/msg/tensor_descriptor.hpp"

class AxonConsumer : public rclcpp::Node {
public:
    AxonConsumer() : rclcpp::Node("axon_consumer") {
        const std::string service =
            declare_parameter<std::string>("service", "ros2_offload");
        const double connect_timeout_s =
            declare_parameter<double>("connect_timeout_s", 10.0);
        target_frames_ = declare_parameter<int>("frames", 90);

        bridge_ = axon_bridge::Consumer::create(
            service, static_cast<int>(connect_timeout_s * 1000));
        if (!bridge_) {
            RCLCPP_FATAL(get_logger(), "sidecar handshake failed (is the producer up?)");
            throw std::runtime_error("consumer bridge create failed");
        }
        RCLCPP_INFO(get_logger(),
            "sidecar handshake: %zu dma-buf FDs, %zu B each, pool_gen=%u — attached.",
            bridge_->buffer_count(), bridge_->buffer_size(), bridge_->pool_generation());

        // keep-last(1) best-effort — always process the freshest descriptor
        // (latest-value-wins), no stale backlog that would tear on an overwritten
        // pool buffer. Must match the producer's QoS to connect.
        sub_ = create_subscription<axon_ros2::msg::TensorDescriptor>(
            "/axon/tensor_desc", rclcpp::QoS(1).best_effort(),
            [this](axon_ros2::msg::TensorDescriptor::ConstSharedPtr m) { on_desc(*m); });

        RCLCPP_INFO(get_logger(),
            "consumer ready — reading payload zero-copy per descriptor (target %d frames).",
            target_frames_);
    }

    // Returns process exit code: 0 iff frames were read with no payload errors.
    int summarize() const {
        if (frames_ == 0) {
            std::printf("[consumer] no frames received\n");
            return 1;
        }
        std::printf("\n────── axon ROS2 offload — consumer summary ──────\n"
                    "  frames read:      %lu\n"
                    "  payload errors:   %lu   (must be 0 — cross-process zero-copy)\n"
                    "  staleness:        mean=%luus  max=%luus\n"
                    "  payload path:     shared dma-buf (never serialized through DDS)\n"
                    "──────────────────────────────────────────────────\n",
                    frames_, payload_errors_, stale_sum_us_ / frames_, stale_max_us_);
        std::fflush(stdout);
        return payload_errors_ == 0 ? 0 : 1;
    }

private:
    void on_desc(const axon_ros2::msg::TensorDescriptor& msg) {
        if (msg.pool_generation != bridge_->pool_generation()) {
            RCLCPP_WARN_THROTTLE(get_logger(), *get_clock(), 1000,
                "pool_generation mismatch (%lu vs %u) — re-handshake needed",
                msg.pool_generation, bridge_->pool_generation());
            return;
        }

        // Zero-copy read: the payload never travelled through DDS.
        const void* base = bridge_->payload(msg.bo_handle, msg.offset);
        if (!base) return;
        uint64_t stamped = 0;
        std::memcpy(&stamped, base, sizeof(stamped));
        if (stamped != msg.seqno) ++payload_errors_;

        const uint64_t staleness_us =
            (axon::rt_now_ns() - msg.producer_publish_ts_ns) / 1000;
        stale_sum_us_ += staleness_us;
        if (staleness_us > stale_max_us_) stale_max_us_ = staleness_us;
        ++frames_;

        if (msg.seqno % 30 == 0)
            RCLCPP_INFO(get_logger(),
                "seqno=%lu buf=%lu payload_ok=%s staleness=%luus (payload via dma-buf, 0 copy)",
                msg.seqno, msg.bo_handle, stamped == msg.seqno ? "yes" : "NO", staleness_us);

        if (frames_ >= static_cast<uint64_t>(target_frames_)) rclcpp::shutdown();
    }

    std::unique_ptr<axon_bridge::Consumer> bridge_;
    rclcpp::Subscription<axon_ros2::msg::TensorDescriptor>::SharedPtr sub_;
    int target_frames_ = 0;
    uint64_t frames_ = 0, payload_errors_ = 0;
    uint64_t stale_sum_us_ = 0, stale_max_us_ = 0;
};

int main(int argc, char** argv) {
    rclcpp::init(argc, argv);
    auto node = std::make_shared<AxonConsumer>();
    rclcpp::spin(node);
    rclcpp::shutdown();
    return node->summarize();
}
