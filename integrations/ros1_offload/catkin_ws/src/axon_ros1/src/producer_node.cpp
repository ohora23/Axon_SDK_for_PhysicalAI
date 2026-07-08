// SPDX-License-Identifier: Apache-2.0
// axon ROS1 offload — producer node.
//
// Publishes a small TensorDescriptor on a ROS1 topic (the metadata/liveness
// plane) while the actual payload stays in an axon dma-buf pool and its FDs are
// delivered once through the axon SCM_RIGHTS sidecar (the FD plane). Each frame
// only stamps a seqno into the shared buffer and publishes the descriptor — no
// payload is serialized or copied through ROS.
//
// The pool + mmap + sidecar bootstrap lives in the reusable, ROS-agnostic
// axon_bridge::Producer; this node is just the ROS glue over it.

#include <cstring>

#include <ros/ros.h>

#include "axon/rt.h"
#include "axon/types.h"
#include "axon_ros1/axon_bridge.h"
#include "axon_ros1/TensorDescriptor.h"

int main(int argc, char** argv) {
    ros::init(argc, argv, "axon_producer");
    ros::NodeHandle nh("~");

    int n_buffers = nh.param("n_buffers", 8);
    int bytes = nh.param("bytes", 1 << 20);   // 1 MiB/frame default
    double rate_hz = nh.param("rate_hz", 30.0);
    std::string service = nh.param<std::string>("service", "ros1_offload");

    auto bridge = axon_bridge::Producer::create(
        service, static_cast<size_t>(n_buffers), static_cast<size_t>(bytes));
    if (!bridge) { ROS_FATAL("axon_bridge::Producer::create failed"); return 1; }

    ros::Publisher pub =
        nh.advertise<axon_ros1::TensorDescriptor>("/axon/tensor_desc", 10);

    ROS_INFO("axon producer: %d buffers x %d B, %.0f Hz, sidecar '%s'. "
             "Publishing descriptors on /axon/tensor_desc; payload stays in dma-buf.",
             n_buffers, bytes, rate_hz, service.c_str());

    ros::Rate rate(rate_hz);
    uint64_t seqno = 0;
    while (ros::ok()) {
        // Non-blocking: deliver the pool FDs to any consumer that just connected.
        bridge->poll_new_consumers();

        ++seqno;
        int idx = static_cast<int>(seqno % bridge->buffer_count());
        // "Inference output": stamp the seqno into the shared buffer (offset 0).
        std::memcpy(bridge->buffer(idx), &seqno, sizeof(seqno));

        axon_ros1::TensorDescriptor msg;
        msg.bo_handle = static_cast<uint64_t>(idx);
        msg.seqno = seqno;
        msg.pool_generation = bridge->pool_generation();
        msg.capture_ts_ns = axon::rt_now_ns();
        msg.producer_publish_ts_ns = axon::rt_now_ns();
        msg.shape = {1u, static_cast<uint32_t>(bytes)};
        msg.rank = 2;
        msg.dtype = static_cast<uint8_t>(axon::DType::U8);
        msg.offset = 0;
        msg.size = static_cast<uint64_t>(bytes);
        pub.publish(msg);

        if (seqno % 30 == 0)
            ROS_INFO("published seqno=%lu (buffer %d), consumers=%d",
                     seqno, idx, bridge->connected_count());

        ros::spinOnce();
        rate.sleep();
    }

    return 0;   // bridge dtor munmaps the pool and tears down the sidecar server
}
