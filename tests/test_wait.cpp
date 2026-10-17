// SPDX-License-Identifier: Apache-2.0
// wait_for_update / wait_any: a consumer process blocks until the producer
// publishes, across the real sidecar + seqlock slot. Checks that the wait times
// out with no publish, wakes on a publish (promptly), reports the stream that
// changed, re-arms after latest_view(), returns at once for a frame that arrived
// before the wait, and keeps up with a stream of publishes. The two processes
// sequence themselves over a pipe, not with sleeps.

#include "axon/pool.h"
#include "axon/publisher.h"
#include "axon/rt.h"
#include "axon/subscriber.h"
#include "axon_test.h"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <ctime>

#include <sys/wait.h>
#include <unistd.h>

using namespace axon;

namespace {

constexpr std::size_t kBytes = 4096;

void sleep_ms(int ms) {
    struct timespec ts {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, nullptr);
}

void signal_fd(int fd) { char c = 1; (void)!write(fd, &c, 1); }
bool await_fd(int fd) { char c; return read(fd, &c, 1) == 1; }

// Payload: [0,8) publish time (rt_now_ns), [8,16) a sequence number.
void publish_one(TensorPublisher& pub, std::uint64_t seq) {
    AcquiredDescriptor a = pub.acquire_descriptor();
    const std::uint64_t now = rt_now_ns();
    std::memcpy(a.host_view, &now, sizeof(now));
    std::memcpy(static_cast<char*>(a.host_view) + 8, &seq, sizeof(seq));
    a.desc->rank = 1;
    a.desc->shape[0] = kBytes;
    a.desc->dtype = DType::U8;
    a.desc->size = kBytes;
    a.desc->sync_fence_kind = SyncFenceKind::None;
    pub.publish(std::move(a));
}

void reap(pid_t pid, bool& _axon_ok) {
    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status) && WEXITSTATUS(status) != 0)
        std::printf("    child exit code = %d\n", WEXITSTATUS(status));
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

// Child of the first test: two streams, the parent publishes on "b" only.
int wake_child(const char* a_name, const char* b_name, int ready_fd) {
    auto a = TensorSubscriber::create(a_name);
    auto b = TensorSubscriber::create(b_name);
    if (!a || !b) return 10;
    if (a->wait_handshake(4000) != 0 || b->wait_handshake(4000) != 0) return 11;

    if (a->wait_for_update(50) != 0) return 12;          // nothing published: times out

    TensorSubscriber* subs[2] = {a.get(), b.get()};
    signal_fd(ready_fd);                                 // about to sleep in wait_any
    if (TensorSubscriber::wait_any(subs, 2, 2000) != 1) return 13;
    const std::uint64_t woke = rt_now_ns();
    auto v = b->latest_view(8);
    if (!v) return 14;
    std::uint64_t published_at = 0;
    std::memcpy(&published_at, v->data, sizeof(published_at));
    if (woke - published_at > 20000000ull) return 15;    // > 20 ms: not a real wakeup

    if (b->wait_for_update(50) != 0) return 16;          // consumed: must block again

    // Infinite timeout, then a stream of publishes: every wait returns, none hangs.
    signal_fd(ready_fd);
    std::uint64_t last = 0;
    for (int k = 0; k < 200 && last < 100; ++k) {   // latest-value-wins: frames may be skipped
        if (b->wait_for_update(-1) != 1) return 17;
        auto w = b->latest_view(8);
        if (!w) return 18;
        std::uint64_t seq = 0;
        std::memcpy(&seq, static_cast<const char*>(w->data) + 8, sizeof(seq));
        if (seq <= last) return 19;                      // a wait returned with nothing new
        last = seq;
    }
    return last == 100 ? 0 : 20;
}

}  // namespace

AXON_TEST(wait_times_out_wakes_and_reports_stream) {
    const char* kA = "wait/a";
    const char* kB = "wait/b";
    auto pool_a = TensorPool::create(TensorPoolConfig{4, kBytes, PoolBackend::Custom, nullptr});
    auto pool_b = TensorPool::create(TensorPoolConfig{4, kBytes, PoolBackend::Custom, nullptr});
    REQUIRE(pool_a && pool_b);
    auto pub_a = TensorPublisher::create(kA, *pool_a);
    auto pub_b = TensorPublisher::create(kB, *pool_b);
    REQUIRE(pub_a && pub_b);

    int ready[2];
    REQUIRE(pipe(ready) == 0);
    pid_t pid = fork();
    REQUIRE(pid >= 0);
    if (pid == 0) { close(ready[0]); _exit(wake_child(kA, kB, ready[1])); }
    close(ready[1]);

    CHECK(pub_a->handshake_pool() >= 1);
    CHECK(pub_b->handshake_pool() >= 1);
    REQUIRE(await_fd(ready[0]));
    sleep_ms(20);                 // let the child get into futex_waitv
    publish_one(*pub_b, 1);

    REQUIRE(await_fd(ready[0]));  // child waits with no timeout now
    for (std::uint64_t s = 2; s <= 100; ++s) {
        publish_one(*pub_b, s);
        sleep_ms(1);
    }
    close(ready[0]);
    reap(pid, _axon_ok);
}

AXON_TEST(wait_returns_at_once_if_already_published) {
    const char* kS = "wait/already";
    auto pool = TensorPool::create(TensorPoolConfig{4, kBytes, PoolBackend::Custom, nullptr});
    REQUIRE(pool);
    auto pub = TensorPublisher::create(kS, *pool);
    REQUIRE(pub);

    int done[2];
    REQUIRE(pipe(done) == 0);
    pid_t pid = fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        close(done[1]);
        auto sub = TensorSubscriber::create(kS);
        if (!sub || sub->wait_handshake(4000) != 0) _exit(10);
        if (!await_fd(done[0])) _exit(11);                // the parent has published
        _exit(sub->wait_for_update(0) == 1 ? 0 : 12);     // must not block or miss it
    }
    close(done[0]);
    CHECK(pub->handshake_pool() >= 1);
    publish_one(*pub, 1);
    signal_fd(done[1]);
    close(done[1]);
    reap(pid, _axon_ok);
}

// publish() refuses a descriptor that does not fit, says why, and spends no seqno:
// the consumer's first frame is seqno 1 even after two rejected publishes.
AXON_TEST(publish_rejects_oversized_and_malformed_frames) {
    const char* kS = "wait/reject";
    auto pool = TensorPool::create(TensorPoolConfig{4, kBytes, PoolBackend::Custom, nullptr});
    REQUIRE(pool);
    auto pub = TensorPublisher::create(kS, *pool);
    REQUIRE(pub);

    int done[2];
    REQUIRE(pipe(done) == 0);
    pid_t pid = fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        close(done[1]);
        auto sub = TensorSubscriber::create(kS);
        if (!sub || sub->wait_handshake(4000) != 0) _exit(10);
        if (!await_fd(done[0])) _exit(11);
        auto v = sub->latest_view(8);
        _exit(v && v->seqno == 1 ? 0 : 12);
    }
    close(done[0]);
    CHECK(pub->handshake_pool() >= 1);

    AcquiredDescriptor a = pub->acquire_descriptor();
    const std::size_t buf = pool->buffer_size();
    a.desc->rank = 1;
    a.desc->shape[0] = static_cast<std::uint32_t>(buf + 1);
    a.desc->dtype = DType::U8;
    a.desc->size = buf + 1;                              // one byte past the buffer
    errno = 0;
    CHECK(pub->publish(std::move(a)) == -1);
    CHECK(errno == EMSGSIZE);

    AcquiredDescriptor b = pub->acquire_descriptor();
    b.desc->rank = 0;                                    // malformed
    b.desc->dtype = DType::U8;
    b.desc->size = 16;
    errno = 0;
    CHECK(pub->publish(std::move(b)) == -1);
    CHECK(errno == EINVAL);
    CHECK(pub->rejected_count() == 2);

    AcquiredDescriptor c = pub->acquire_descriptor();
    c.desc->rank = 1;
    c.desc->shape[0] = kBytes;
    c.desc->dtype = DType::U8;
    c.desc->size = kBytes;
    c.desc->sync_fence_kind = SyncFenceKind::None;
    CHECK(pub->publish(std::move(c)) == 0);

    signal_fd(done[1]);
    close(done[1]);
    reap(pid, _axon_ok);
}

AXON_TEST_MAIN("wait")
