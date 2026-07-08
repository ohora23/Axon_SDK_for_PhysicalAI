// SPDX-License-Identifier: Apache-2.0
// axon image_transport plugin — subscriber side ("axon" transport).
//
// Any node that does
//   image_transport::Subscriber s =
//       it.subscribe("camera/image", 1, cb, {}, image_transport::TransportHints("axon"));
// receives frames whose pixels never travelled through ROS: the sidecar delivers
// the dma-buf pool once, then each AxonImage descriptor is turned back into a
// sensor_msgs::Image read straight from shared memory.
//
// The sidecar + mmap work lives in the reusable axon_bridge::Consumer.

#include <cstring>
#include <memory>
#include <string>

#include <image_transport/simple_subscriber_plugin.h>
#include <pluginlib/class_list_macros.h>
#include <sensor_msgs/Image.h>

#include "axon_ros1/axon_bridge.h"
#include "axon_ros1/AxonImage.h"

namespace axon_transport {

class AxonSubscriber
    : public image_transport::SimpleSubscriberPlugin<axon_ros1::AxonImage> {
public:
    virtual ~AxonSubscriber() {}
    virtual std::string getTransportName() const { return "axon"; }

protected:
    virtual void subscribeImpl(
        ros::NodeHandle& nh, const std::string& base_topic, uint32_t queue_size,
        const Callback& callback, const ros::VoidPtr& tracked_object,
        const image_transport::TransportHints& transport_hints) {
        service_ = base_topic;
        typedef image_transport::SimpleSubscriberPlugin<axon_ros1::AxonImage> Base;
        Base::subscribeImpl(nh, base_topic, queue_size, callback, tracked_object, transport_hints);
    }

    virtual void internalCallback(const axon_ros1::AxonImage::ConstPtr& m,
                                  const Callback& user_cb) {
        // Attach the sidecar on the first frame — the producer is up by now.
        if (!consumer_) {
            consumer_ = axon_bridge::Consumer::create(service_, 5000);
            if (!consumer_) {
                ROS_ERROR_THROTTLE(1.0, "axon transport: sidecar handshake failed for '%s'", service_.c_str());
                return;
            }
        }
        if (m->pool_generation != consumer_->pool_generation()) {
            // ponytail: drop on generation change (matches the offload node);
            // a re-handshake path is future work.
            ROS_WARN_THROTTLE(1.0, "axon transport: pool_generation mismatch (%u vs %u)",
                              m->pool_generation, consumer_->pool_generation());
            return;
        }

        const void* base = consumer_->payload(m->bo_handle, m->offset);
        if (!base) return;

        sensor_msgs::ImagePtr img(new sensor_msgs::Image());
        img->header = m->header;
        img->height = m->height;
        img->width = m->width;
        img->encoding = m->encoding;
        img->is_bigendian = m->is_bigendian;
        img->step = m->step;
        img->data.resize(m->size);
        // Subscriber-side copy out of the dma-buf into the Image the callback
        // expects. ponytail: this final copy is the image_transport boundary;
        // true end-to-end zero-copy needs a custom allocator aliasing the
        // dma-buf. The cross-process payload hop already avoided serialization.
        std::memcpy(img->data.data(), base, m->size);
        user_cb(img);
    }

private:
    std::string service_;
    std::shared_ptr<axon_bridge::Consumer> consumer_;
};

}  // namespace axon_transport

PLUGINLIB_EXPORT_CLASS(axon_transport::AxonSubscriber, image_transport::SubscriberPlugin)
