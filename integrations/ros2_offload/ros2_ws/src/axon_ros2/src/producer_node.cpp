// SPDX-License-Identifier: Apache-2.0
// axon ROS2 offload — producer node.
//
// Publishes a small TensorDescriptor on a ROS2 topic (the metadata/liveness
// plane) while the payload stays in an axon dma-buf pool whose FDs are delivered
// once through the axon SCM_RIGHTS sidecar (the FD plane). Each frame stamps a
// seqno into the shared buffer and publishes only the descriptor — no payload is
// serialized or copied through DDS.
//
// The pool + mmap + sidecar bootstrap lives in the reusable, ROS-agnostic
// axon_bridge::Producer (the SAME header the ROS1 wrapper uses); this node is
// only the rclcpp glue over it.

#include <chrono>
#include <cstring>
#include <memory>

#include <rclcpp/rclcpp.hpp>

#include "axon/rt.h"
#include "axon/types.h"
#include "axon_ros2/axon_bridge.h"
#include "axon_ros2/msg/tensor_descriptor.hpp"

using namespace std::chrono_literals;

class AxonProducer : public rclcpp::Node {
public:
    AxonProducer() : rclcpp::Node("axon_producer") {
        // Ring depth must exceed the consumer's worst-case lag or a slot laps
        // mid-read (latest-value-wins torn frame). 32 (the sidecar's max FDs per
        // handshake) >> the few-frame lag seen over DDS; see the sizing note in
        // docs/usage.md.
        const int n_buffers = declare_parameter<int>("n_buffers", 32);
        bytes_ = declare_parameter<int>("bytes", 1 << 20);   // 1 MiB/frame
        const double rate_hz = declare_parameter<double>("rate_hz", 30.0);
        const std::string service =
            declare_parameter<std::string>("service", "ros2_offload");

        bridge_ = axon_bridge::Producer::create(
            service, static_cast<size_t>(n_buffers), static_cast<size_t>(bytes_));
        if (!bridge_) {
            RCLCPP_FATAL(get_logger(), "axon_bridge::Producer::create failed");
            throw std::runtime_error("producer bridge create failed");
        }

        // keep-last(1) best-effort: mirror axon's latest-value-wins semantics so
        // the consumer always sees the freshest descriptor and never builds a
        // stale backlog that would read an already-overwritten pool buffer.
        pub_ = create_publisher<axon_ros2::msg::TensorDescriptor>(
            "/axon/tensor_desc", rclcpp::QoS(1).best_effort());

        RCLCPP_INFO(get_logger(),
            "axon producer: %d buffers x %d B, %.0f Hz, sidecar '%s'. "
            "Publishing descriptors on /axon/tensor_desc; payload stays in dma-buf.",
            n_buffers, bytes_, rate_hz, service.c_str());

        const auto period = std::chrono::duration<double>(1.0 / rate_hz);
        timer_ = create_wall_timer(
            std::chrono::duration_cast<std::chrono::nanoseconds>(period),
            [this] { tick(); });
    }

private:
    void tick() {
        // Non-blocking: deliver the pool FDs to any consumer that just connected.
        bridge_->poll_new_consumers();

        ++seqno_;
        const int idx = static_cast<int>(seqno_ % bridge_->buffer_count());
        // "Inference output": stamp the seqno into the shared buffer (offset 0).
        std::memcpy(bridge_->buffer(idx), &seqno_, sizeof(seqno_));

        axon_ros2::msg::TensorDescriptor msg;
        msg.bo_handle = static_cast<uint64_t>(idx);
        msg.seqno = seqno_;
        msg.pool_generation = bridge_->pool_generation();
        msg.capture_ts_ns = axon::rt_now_ns();
        msg.producer_publish_ts_ns = axon::rt_now_ns();
        msg.shape = {1u, static_cast<uint32_t>(bytes_)};
        msg.rank = 2;
        msg.dtype = static_cast<uint8_t>(axon::DType::U8);
        msg.offset = 0;
        msg.size = static_cast<uint64_t>(bytes_);
        pub_->publish(msg);

        if (seqno_ % 30 == 0)
            RCLCPP_INFO(get_logger(), "published seqno=%lu (buffer %d), consumers=%d",
                        seqno_, idx, bridge_->connected_count());
    }

    std::unique_ptr<axon_bridge::Producer> bridge_;
    rclcpp::Publisher<axon_ros2::msg::TensorDescriptor>::SharedPtr pub_;
    rclcpp::TimerBase::SharedPtr timer_;
    int bytes_ = 0;
    uint64_t seqno_ = 0;
};

int main(int argc, char** argv) {
    rclcpp::init(argc, argv);
    rclcpp::spin(std::make_shared<AxonProducer>());
    rclcpp::shutdown();
    return 0;   // bridge dtor munmaps the pool and tears down the sidecar server
}
