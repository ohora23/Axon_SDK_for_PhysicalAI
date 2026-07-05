// SPDX-License-Identifier: Apache-2.0
// dczc ROS1 offload — producer node.
//
// Publishes a small TensorDescriptor on a ROS1 topic (the metadata/liveness
// plane) while the actual payload stays in a dczc dma-buf pool and its FDs are
// delivered once through the dczc SCM_RIGHTS sidecar (the FD plane). Each frame
// only stamps a seqno into the shared buffer and publishes the descriptor — no
// payload is serialized or copied through ROS.

#include <cstring>
#include <vector>

#include <sys/mman.h>
#include <unistd.h>

#include <ros/ros.h>

#include "dczc/pool.h"
#include "dczc/rt.h"
#include "dczc/detail/sidecar.h"
#include "dczc/tensor_descriptor.h"   // kWireVersion
#include "dczc/types.h"
#include "dczc_ros1/TensorDescriptor.h"

int main(int argc, char** argv) {
    ros::init(argc, argv, "dczc_producer");
    ros::NodeHandle nh("~");

    int n_buffers = nh.param("n_buffers", 8);
    int bytes = nh.param("bytes", 1 << 20);   // 1 MiB/frame default
    double rate_hz = nh.param("rate_hz", 30.0);
    std::string service = nh.param<std::string>("service", "ros1_offload");

    // dczc FD plane: a dma-buf pool + sidecar server.
    auto pool = dczc::TensorPool::create(
        {static_cast<size_t>(n_buffers), static_cast<size_t>(bytes),
         dczc::PoolBackend::Custom, nullptr});
    if (!pool) { ROS_FATAL("TensorPool::create failed"); return 1; }

    const auto& fds = pool->dma_buf_fds();
    std::vector<void*> host(fds.size(), nullptr);
    for (size_t i = 0; i < fds.size(); ++i) {
        host[i] = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fds[i], 0);
        if (host[i] == MAP_FAILED) { ROS_FATAL("mmap pool buffer failed"); return 1; }
    }

    auto* server = dczc::detail::SidecarServer::create(service);
    if (!server) { ROS_FATAL("SidecarServer::create failed"); return 1; }
    server->set_pool(fds, static_cast<uint32_t>(pool->generation()),
                     static_cast<uint64_t>(bytes), dczc::kWireVersion);

    ros::Publisher pub =
        nh.advertise<dczc_ros1::TensorDescriptor>("/dczc/tensor_desc", 10);

    ROS_INFO("dczc producer: %d buffers x %d B, %.0f Hz, sidecar '%s'. "
             "Publishing descriptors on /dczc/tensor_desc; payload stays in dma-buf.",
             n_buffers, bytes, rate_hz, service.c_str());

    ros::Rate rate(rate_hz);
    uint64_t seqno = 0;
    while (ros::ok()) {
        // Non-blocking: deliver the pool FDs to any consumer that just connected.
        server->accept_and_handshake(0);

        ++seqno;
        int idx = static_cast<int>(seqno % fds.size());
        // "Inference output": stamp the seqno into the shared buffer (offset 0).
        std::memcpy(host[idx], &seqno, sizeof(seqno));

        dczc_ros1::TensorDescriptor msg;
        msg.bo_handle = static_cast<uint64_t>(idx);
        msg.seqno = seqno;
        msg.pool_generation = pool->generation();
        msg.capture_ts_ns = dczc::rt_now_ns();
        msg.producer_publish_ts_ns = dczc::rt_now_ns();
        msg.shape = {1u, static_cast<uint32_t>(bytes)};
        msg.rank = 2;
        msg.dtype = static_cast<uint8_t>(dczc::DType::U8);
        msg.offset = 0;
        msg.size = static_cast<uint64_t>(bytes);
        pub.publish(msg);

        if (seqno % 30 == 0)
            ROS_INFO("published seqno=%lu (buffer %d), consumers=%d",
                     seqno, idx, server->connected_count());

        ros::spinOnce();
        rate.sleep();
    }

    for (size_t i = 0; i < host.size(); ++i)
        if (host[i] && host[i] != MAP_FAILED) munmap(host[i], bytes);
    delete server;
    return 0;
}
