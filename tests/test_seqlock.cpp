// SPDX-License-Identifier: Apache-2.0
// Metadata plane — seqlock slot correctness under concurrency.
//
// A background writer hammers the slot while a reader loops; every consistent
// read must observe a self-consistent descriptor (seqno matches the encoded
// payload). Torn reads would show up as a mismatch.

#include "dczc/detail/metadata_channel.h"
#include "dczc/tensor_descriptor.h"
#include "dczc_test.h"

#include <atomic>
#include <thread>

using namespace dczc;
using namespace dczc::detail;

namespace {
// Encode seqno redundantly into several fields so a torn read is detectable.
TensorDescriptor make_desc(std::uint64_t s) {
    TensorDescriptor d {};
    d.seqno = s;
    d.capture_ts_ns = s;
    d.producer_publish_ts_ns = s;
    d.offset = s;
    d.size = s + 1;
    d.rank = 1;
    d.shape[0] = static_cast<std::uint32_t>(s);
    d.pool_generation = 1;
    return d;
}
bool consistent(const TensorDescriptor& d) {
    return d.capture_ts_ns == d.seqno &&
           d.producer_publish_ts_ns == d.seqno &&
           d.offset == d.seqno &&
           d.size == d.seqno + 1 &&
           d.shape[0] == static_cast<std::uint32_t>(d.seqno);
}
}  // namespace

DCZC_TEST(basic_publish_read) {
    const char* svc = "test_seqlock_basic";
    auto* pub = MetadataChannel::create_publisher(svc);
    REQUIRE(pub != nullptr);
    auto* sub = MetadataChannel::create_subscriber(svc, 1000);
    REQUIRE(sub != nullptr);

    CHECK(!sub->has_data());
    TensorDescriptor out;
    int retries = 0;
    CHECK(!sub->read_latest(&out, 8, &retries));  // nothing published yet

    pub->publish(make_desc(42));
    REQUIRE(sub->read_latest(&out, 8, &retries));
    CHECK(out.seqno == 42);
    CHECK(consistent(out));
    CHECK(retries == 0);

    delete sub;
    delete pub;
}

DCZC_TEST(latest_value_wins) {
    const char* svc = "test_seqlock_latest";
    auto* pub = MetadataChannel::create_publisher(svc);
    REQUIRE(pub != nullptr);
    auto* sub = MetadataChannel::create_subscriber(svc, 1000);
    REQUIRE(sub != nullptr);

    for (std::uint64_t s = 1; s <= 100; ++s) pub->publish(make_desc(s));

    TensorDescriptor out;
    int retries = 0;
    REQUIRE(sub->read_latest(&out, 8, &retries));
    CHECK(out.seqno == 100);  // only the newest survives
    CHECK(consistent(out));

    delete sub;
    delete pub;
}

DCZC_TEST(concurrent_no_torn_reads) {
    const char* svc = "test_seqlock_concurrent";
    auto* pub = MetadataChannel::create_publisher(svc);
    REQUIRE(pub != nullptr);
    auto* sub = MetadataChannel::create_subscriber(svc, 1000);
    REQUIRE(sub != nullptr);

    std::atomic<bool> stop {false};
    std::atomic<std::uint64_t> writes {0};
    std::thread writer([&] {
        std::uint64_t s = 1;
        while (!stop.load(std::memory_order_relaxed)) {
            pub->publish(make_desc(s++));
            writes.fetch_add(1, std::memory_order_relaxed);
        }
    });

    std::uint64_t good = 0, fell_back = 0, inconsistent = 0;
    for (int i = 0; i < 200000; ++i) {
        TensorDescriptor out;
        int retries = 0;
        if (sub->read_latest(&out, 8, &retries)) {
            ++good;
            if (!consistent(out)) ++inconsistent;  // must never happen
        } else {
            ++fell_back;
        }
    }
    stop.store(true);
    writer.join();

    std::printf("    writes=%lu good_reads=%lu fallbacks=%lu inconsistent=%lu\n",
                (unsigned long)writes.load(), (unsigned long)good,
                (unsigned long)fell_back, (unsigned long)inconsistent);
    CHECK(inconsistent == 0);  // the core seqlock guarantee
    CHECK(good > 0);

    delete sub;
    delete pub;
}

DCZC_TEST_MAIN("seqlock")
