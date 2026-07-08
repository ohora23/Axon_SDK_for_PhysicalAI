// SPDX-License-Identifier: Apache-2.0
// Test subscriber for the axon image_transport plugin. Standard image_transport
// API with TransportHints("axon"), so the pixels arrive via the dma-buf sidecar,
// not over the wire. Verifies the seqno stamped by the publisher survives the
// zero-copy round-trip.

#include <cstdio>
#include <cstring>

#include <ros/ros.h>
#include <image_transport/image_transport.h>
#include <sensor_msgs/Image.h>

namespace {
uint64_t g_frames = 0, g_ok = 0, g_bad = 0;

// The publisher stamps a per-frame seqno into the first 8 bytes and fills the
// whole image with byte (seqno & 0xff). We verify the frame is *internally
// coherent* end-to-end — the bulk pixels match the stamped seqno — which proves
// a complete, untorn frame arrived through the dma-buf. (We can't compare to
// header.seq: roscpp overwrites it with the sub-topic's own publish count.)
void on_image(const sensor_msgs::ImageConstPtr& img) {
    ++g_frames;
    const size_t n = img->data.size();
    if (n >= 16) {
        uint64_t stamped = 0;
        std::memcpy(&stamped, img->data.data(), sizeof(stamped));
        const uint8_t expect = static_cast<uint8_t>(stamped & 0xff);
        const bool ok = stamped != 0 &&
                        img->data[n / 2] == expect && img->data[n - 1] == expect;
        ok ? ++g_ok : ++g_bad;
        if (g_frames % 30 == 0)
            ROS_INFO("recv %ux%u %s seqno=%lu payload_ok=%s (bulk pixels via axon dma-buf)",
                     img->width, img->height, img->encoding.c_str(),
                     (unsigned long)stamped, ok ? "yes" : "NO");
    } else {
        ++g_bad;
    }
}
}  // namespace

int main(int argc, char** argv) {
    ros::init(argc, argv, "axon_image_sub");
    ros::NodeHandle nh("~");

    image_transport::ImageTransport it(nh);
    image_transport::Subscriber sub = it.subscribe(
        "/camera/image", 5, on_image, ros::VoidPtr(),
        image_transport::TransportHints("axon"));

    ROS_INFO("axon image sub: subscribed /camera/image with transport 'axon'.");
    ros::spin();

    if (g_frames) {
        std::printf("\n────── axon image_transport (M2) — subscriber summary ──────\n"
                    "  frames received:  %lu\n"
                    "  payload ok:       %lu\n"
                    "  payload errors:   %lu   (must be 0 — pixels via dma-buf, not the wire)\n"
                    "────────────────────────────────────────────────────────────\n",
                    g_frames, g_ok, g_bad);
        std::fflush(stdout);
    } else {
        std::printf("axon image sub: NO frames received (plugin not loaded?)\n");
        std::fflush(stdout);
    }
    return 0;
}
