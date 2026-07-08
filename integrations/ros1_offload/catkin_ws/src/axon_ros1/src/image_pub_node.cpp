// SPDX-License-Identifier: Apache-2.0
// Test publisher for the axon image_transport plugin. Uses the *standard*
// image_transport API — nothing axon-specific here — so it proves the transport
// is a real drop-in. Publishes a synthetic RGB8 image, stamping the seqno into
// the first pixels so the subscriber can verify a true zero-copy round-trip.

#include <cstring>
#include <vector>

#include <ros/ros.h>
#include <image_transport/image_transport.h>
#include <sensor_msgs/Image.h>

int main(int argc, char** argv) {
    ros::init(argc, argv, "axon_image_pub");
    ros::NodeHandle nh("~");
    int w = nh.param("width", 640);
    int h = nh.param("height", 480);
    double rate_hz = nh.param("rate_hz", 30.0);

    image_transport::ImageTransport it(nh);
    image_transport::Publisher pub = it.advertise("/camera/image", 1);

    const uint32_t step = static_cast<uint32_t>(w) * 3;   // rgb8
    ROS_INFO("axon image pub: %dx%d rgb8 @ %.0f Hz on /camera/image (offers 'axon' transport)",
             w, h, rate_hz);

    ros::Rate rate(rate_hz);
    uint64_t seq = 0;
    while (ros::ok()) {
        ++seq;
        sensor_msgs::Image img;
        img.header.stamp = ros::Time::now();
        img.header.seq = static_cast<uint32_t>(seq);
        img.height = static_cast<uint32_t>(h);
        img.width = static_cast<uint32_t>(w);
        img.encoding = "rgb8";
        img.is_bigendian = 0;
        img.step = step;
        img.data.assign(static_cast<size_t>(step) * h, static_cast<uint8_t>(seq & 0xff));
        // Stamp the full seqno into the first 8 bytes for an exact check.
        std::memcpy(img.data.data(), &seq, sizeof(seq));
        pub.publish(img);

        if (seq % 30 == 0) ROS_INFO("published frame seq=%lu", seq);
        ros::spinOnce();
        rate.sleep();
    }
    return 0;
}
