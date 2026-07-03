// SPDX-License-Identifier: Apache-2.0
// dczc — Metadata plane: Iceoryx2 backend (compiled only with DCZC_WITH_ICEORYX2)
//
// Replaces the POSIX-SHM seqlock stand-in with Iceoryx2's lock-free SHM queue —
// the metadata plane the design doc §1.1 specifies. TensorDescriptor is a
// fixed-size POD, so it maps directly onto a publish_subscribe<TensorDescriptor>
// service with zero serialization.
//
// Latest-value-wins semantics (to match the RT consumer's latest_view()): the
// subscriber uses a small buffer and read_latest() drains to the newest sample,
// caching it so reads between publishes still return the current value — exactly
// how the seqlock slot behaves.

#include "dczc/detail/metadata_channel.h"

#include <cstring>
#include <ctime>
#include <optional>
#include <string>

#include "iox2/iceoryx2.hpp"

namespace dczc::detail {

namespace {

constexpr iox2::ServiceType kSvc = iox2::ServiceType::Ipc;
using Node = iox2::Node<kSvc>;
using Service = iox2::PortFactoryPublishSubscribe<kSvc, TensorDescriptor, void>;
using Publisher = iox2::Publisher<kSvc, TensorDescriptor, void>;
using Subscriber = iox2::Subscriber<kSvc, TensorDescriptor, void>;

void sleep_ms(int ms) {
    struct timespec ts {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, nullptr);
}

// Iceoryx2 service names allow a broad character set; reuse the dczc convention.
std::string service_id(const std::string& service_name) {
    return "dczc/" + service_name;
}

Service open_service(Node& node, const std::string& service_name) {
    return node.service_builder(
                   iox2::ServiceName::create(service_id(service_name).c_str()).value())
        .publish_subscribe<TensorDescriptor>()
        .max_publishers(1)
        .max_subscribers(16)
        .history_size(1)
        .subscriber_max_buffer_size(4)
        .open_or_create()
        .value();
}

class Iceoryx2Channel final : public MetadataChannel {
public:
    Iceoryx2Channel(Node&& node, Service&& service, Publisher&& pub)
        : node_(std::move(node)), service_(std::move(service)),
          publisher_(std::move(pub)) {}
    Iceoryx2Channel(Node&& node, Service&& service, Subscriber&& sub)
        : node_(std::move(node)), service_(std::move(service)),
          subscriber_(std::move(sub)) {}

    void publish(const TensorDescriptor& desc) noexcept override {
        if (!publisher_.has_value()) return;
        auto sample = publisher_->loan_uninit();
        if (!sample.has_value()) return;  // no free slot — drop (latest-wins)
        auto initialized = std::move(sample.value()).write_payload(TensorDescriptor(desc));
        (void)iox2::send(std::move(initialized));  // best-effort on the RT path
    }

    bool read_latest(TensorDescriptor* out, int max_retry,
                     int* retries_out) noexcept override {
        (void)max_retry;
        if (retries_out) *retries_out = 0;
        if (!subscriber_.has_value()) return false;

        // Drain to the newest available sample; cache it.
        bool got_new = false;
        for (;;) {
            auto recv = subscriber_->receive();
            if (!recv.has_value()) break;
            auto& maybe = recv.value();
            if (!maybe.has_value()) break;
            last_ = maybe->payload();
            have_ = true;
            got_new = true;
        }
        (void)got_new;
        if (!have_) return false;
        *out = last_;
        return true;
    }

    bool has_data() const noexcept override { return have_; }

private:
    Node node_;
    Service service_;
    std::optional<Publisher>  publisher_;
    std::optional<Subscriber> subscriber_;
    TensorDescriptor last_{};
    bool have_ = false;
};

}  // namespace

MetadataChannel* make_iceoryx2_publisher(const std::string& service_name) {
    try {
        auto node = iox2::NodeBuilder().create<kSvc>().value();
        auto service = open_service(node, service_name);
        auto publisher = service.publisher_builder().create().value();
        return new Iceoryx2Channel(std::move(node), std::move(service),
                                   std::move(publisher));
    } catch (...) {
        return nullptr;  // fall back to the seqlock backend
    }
}

MetadataChannel* make_iceoryx2_subscriber(const std::string& service_name,
                                          int timeout_ms) {
    // Wait for the publisher to create the service first.
    int waited = 0;
    const int step = 20;
    for (;;) {
        try {
            auto node = iox2::NodeBuilder().create<kSvc>().value();
            auto service = open_service(node, service_name);
            auto subscriber = service.subscriber_builder().create().value();
            return new Iceoryx2Channel(std::move(node), std::move(service),
                                       std::move(subscriber));
        } catch (...) {
            if (waited >= timeout_ms) return nullptr;
            sleep_ms(step);
            waited += step;
        }
    }
}

}  // namespace dczc::detail
