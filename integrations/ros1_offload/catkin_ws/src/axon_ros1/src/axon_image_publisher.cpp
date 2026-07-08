// SPDX-License-Identifier: Apache-2.0
// axon image_transport plugin — publisher side ("axon" transport).
//
// A drop-in image_transport transport: any node that does
//   image_transport::Publisher p = it.advertise("camera/image", 1);
// automatically offers the "axon" transport on "camera/image/axon". Instead of
// serializing the pixels onto the wire, the publisher writes them into an axon
// dma-buf pool (delivered cross-process by the SCM_RIGHTS sidecar) and puts only
// a small AxonImage descriptor on the topic. The sidecar service name is derived
// from the base topic, so publisher and subscriber rendezvous with no config.
//
// All the pool + mmap + sidecar work lives in the reusable axon_bridge::Producer.

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>

#include <image_transport/simple_publisher_plugin.h>
#include <pluginlib/class_list_macros.h>

#include "axon_ros1/axon_bridge.h"
#include "axon_ros1/AxonImage.h"

namespace axon_transport {

class AxonPublisher
    : public image_transport::SimplePublisherPlugin<axon_ros1::AxonImage> {
public:
    virtual ~AxonPublisher() {}
    virtual std::string getTransportName() const { return "axon"; }

protected:
    virtual void advertiseImpl(
        ros::NodeHandle& nh, const std::string& base_topic, uint32_t queue_size,
        const image_transport::SubscriberStatusCallback& user_connect_cb,
        const image_transport::SubscriberStatusCallback& user_disconnect_cb,
        const ros::VoidPtr& tracked_object, bool latch) {
        // Sidecar service == the base topic. The pool is created lazily on the
        // first frame, once we know the frame size.
        service_ = base_topic;
        typedef image_transport::SimplePublisherPlugin<axon_ros1::AxonImage> Base;
        Base::advertiseImpl(nh, base_topic, queue_size, user_connect_cb,
                            user_disconnect_cb, tracked_object, latch);
    }

    virtual void publish(const sensor_msgs::Image& image,
                         const PublishFn& publish_fn) const {
        const size_t frame_bytes = static_cast<size_t>(image.step) * image.height;
        if (frame_bytes == 0) return;

        // Lazily (re)create the pool sized to this frame.
        // ponytail: fixed frame size; a resolution change reallocs a new pool.
        if (!producer_ || producer_->buffer_size() < frame_bytes) {
            producer_ = axon_bridge::Producer::create(service_, /*n_buffers=*/8, frame_bytes);
            if (!producer_) {
                ROS_ERROR_THROTTLE(1.0, "axon transport: pool create failed for '%s'", service_.c_str());
                return;
            }
        }
        producer_->poll_new_consumers();

        ++seqno_;
        int idx = static_cast<int>(seqno_ % producer_->buffer_count());
        // Publisher-side copy into the dma-buf. ponytail: this boundary copy is
        // inherent unless the upstream node writes into an axon-backed Image
        // allocator; deferred. The cross-process hop below is still zero-copy.
        std::memcpy(producer_->buffer(idx), image.data.data(),
                    std::min(frame_bytes, image.data.size()));

        axon_ros1::AxonImage m;
        m.header = image.header;
        m.height = image.height;
        m.width = image.width;
        m.encoding = image.encoding;
        m.is_bigendian = image.is_bigendian;
        m.step = image.step;
        m.bo_handle = static_cast<uint64_t>(idx);
        m.seqno = seqno_;
        m.pool_generation = producer_->pool_generation();
        m.offset = 0;
        m.size = frame_bytes;
        publish_fn(m);
    }

private:
    std::string service_;
    mutable std::shared_ptr<axon_bridge::Producer> producer_;
    mutable uint64_t seqno_ = 0;
};

}  // namespace axon_transport

PLUGINLIB_EXPORT_CLASS(axon_transport::AxonPublisher, image_transport::PublisherPlugin)
