// SPDX-License-Identifier: Apache-2.0
// End-to-end: a producer process and an RT-consumer process exchange tensors
// with zero host copies, through the real sidecar (FD plane) + seqlock slot
// (metadata plane) + Custom dma-buf pool.
//
// The producer stamps each frame's seqno into the buffer payload; the consumer
// reads it back through its own mmap of the *same* dma-buf. Matching bytes prove
// the data never left the shared buffer.

#include "axon/pool.h"
#include "axon/publisher.h"
#include "axon/subscriber.h"
#include "axon/rt.h"
#include "axon_test.h"

#include <cstdint>
#include <cstring>

#include <ctime>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace axon;

namespace {

constexpr int   kNBuffers  = 8;
constexpr std::size_t kBufBytes = 64 * 1024;
constexpr std::uint64_t kFinalSeqno = 50;
const char* kService = "e2e/tensor_stream";
const char* kFenceService = "e2e/fence_stream";

void sleep_ms(int ms) {
    struct timespec ts {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, nullptr);
}

// ---- consumer process ----
int run_consumer() {
    auto sub = TensorSubscriber::create(kService);
    if (!sub) return 10;
    if (sub->wait_handshake(4000) != 0) return 11;
    if (sub->pool_handshake_count() != 1) return 12;

    // Poll the RT read until we observe the final frame the producer promised.
    std::uint64_t seen = 0;
    bool content_ok = false;
    for (int i = 0; i < 4000; ++i) {
        auto v = sub->latest_view(8);
        if (v && v->seqno >= kFinalSeqno) {
            seen = v->seqno;
            // Payload check: first 8 bytes encode the seqno (zero-copy proof).
            if (v->data) {
                std::uint64_t stamped = 0;
                std::memcpy(&stamped, v->data, sizeof(stamped));
                content_ok = (stamped == v->seqno);
            }
            // Staleness must be measured and sane (< 5 s).
            if (v->staleness_ns == 0 || v->staleness_ns > 5ULL * 1000000000ULL) {
                return 13;
            }
            break;
        }
        sleep_ms(1);
    }
    if (seen < kFinalSeqno) return 14;
    if (!content_ok) return 15;
    return 0;
}

// ---- R2 sync-fence: a memfd whose first byte encodes `token` stands in for a
// real sync_file fence. If the consumer's surfaced sync_fd maps to that byte,
// the *same* fence object crossed the boundary (option B: drain_fences ->
// latest_view surfaces it without a syscall).
int make_token_fence(std::uint64_t token) {
    int fd = memfd_create("axon_fence", MFD_CLOEXEC);
    if (fd < 0) return -1;
    if (ftruncate(fd, 64) < 0) { close(fd); return -1; }
    void* p = mmap(nullptr, 64, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { close(fd); return -1; }
    static_cast<unsigned char*>(p)[0] = static_cast<unsigned char>(token);
    munmap(p, 64);
    return fd;
}

int run_fence_consumer() {
    auto sub = TensorSubscriber::create(kFenceService);
    if (!sub) return 10;
    if (sub->wait_handshake(4000) != 0) return 11;

    for (int i = 0; i < 4000; ++i) {
        sub->drain_fences();               // non-RT: pull fences off the socket
        auto v = sub->latest_view(8);      // RT: surfaces the fence, no syscall
        if (v && v->seqno >= kFinalSeqno) {
            if (v->sync_fd < 0) return 12;                 // fence must be surfaced
            void* p = mmap(nullptr, 64, PROT_READ, MAP_SHARED, v->sync_fd, 0);
            if (p == MAP_FAILED) return 13;
            bool same = (static_cast<unsigned char*>(p)[0] ==
                         static_cast<unsigned char>(v->seqno));
            munmap(p, 64);
            return same ? 0 : 14;          // right fence object crossed?
        }
        sleep_ms(1);
    }
    return 15;
}

}  // namespace

AXON_TEST(sync_fence_surfaces_to_consumer) {
    auto pool = TensorPool::create(
        TensorPoolConfig{kNBuffers, kBufBytes, PoolBackend::Custom, nullptr});
    REQUIRE(pool != nullptr);
    auto pub = TensorPublisher::create(kFenceService, *pool);
    REQUIRE(pub != nullptr);

    pid_t pid = fork();
    REQUIRE(pid >= 0);
    if (pid == 0) _exit(run_fence_consumer());

    int accepted = pub->handshake_pool();
    CHECK(accepted >= 1);

    for (std::uint64_t s = 1; s <= kFinalSeqno; ++s) {
        AcquiredDescriptor a = pub->acquire_descriptor();
        REQUIRE(a.buffer_index >= 0);
        std::memcpy(a.host_view, &s, sizeof(s));
        a.desc->rank = 1;
        a.desc->shape[0] = static_cast<std::uint32_t>(kBufBytes);
        a.desc->dtype = DType::U8;
        a.desc->offset = 0;
        a.desc->size = kBufBytes;
        a.desc->sync_fence_kind = SyncFenceKind::SyncFileViaSidecar;
        a.desc->sync_fence_token = s;

        int fence = make_token_fence(s);
        REQUIRE(fence >= 0);
        pub->publish(std::move(a), fence);
        close(fence);   // SCM_RIGHTS dup'd it to the consumer; drop our copy
        sleep_ms(1);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status));
    if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
        std::printf("    fence consumer exit code = %d\n", WEXITSTATUS(status));
    }
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

AXON_TEST(producer_consumer_zero_copy_stream) {
    auto pool = TensorPool::create(
        TensorPoolConfig{kNBuffers, kBufBytes, PoolBackend::Custom, nullptr});
    REQUIRE(pool != nullptr);

    auto pub = TensorPublisher::create(kService, *pool);
    REQUIRE(pub != nullptr);

    pid_t pid = fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        // Child must not run parent destructors (they unlink the shm/socket).
        _exit(run_consumer());
    }

    // Parent = producer. Wait for the consumer to connect, then bulk-deliver FDs.
    int accepted = pub->handshake_pool();
    CHECK(accepted >= 1);

    for (std::uint64_t s = 1; s <= kFinalSeqno; ++s) {
        AcquiredDescriptor a = pub->acquire_descriptor();
        REQUIRE(a.buffer_index >= 0);
        REQUIRE(a.host_view != nullptr);

        // Write the frame's identity into the buffer (stand-in for an inference
        // output written straight into the dma-buf).
        std::memcpy(a.host_view, &s, sizeof(s));

        a.desc->rank = 1;
        a.desc->shape[0] = static_cast<std::uint32_t>(kBufBytes);
        a.desc->dtype = DType::U8;
        a.desc->offset = 0;
        a.desc->size = kBufBytes;
        a.desc->sync_fence_kind = SyncFenceKind::None;

        pub->publish(std::move(a));
        sleep_ms(1);
    }

    // Keep the final frame's buffer intact and let the consumer settle.
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status));
    if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
        std::printf("    consumer exit code = %d\n", WEXITSTATUS(status));
    }
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

AXON_TEST(late_joiner_gets_pool) {
    // A subscriber that connects after streaming has begun still receives the
    // pool bundle via publish()'s non-blocking accept.
    auto pool = TensorPool::create(
        TensorPoolConfig{4, 4096, PoolBackend::Custom, nullptr});
    REQUIRE(pool != nullptr);
    auto pub = TensorPublisher::create("e2e/late_joiner", *pool);
    REQUIRE(pub != nullptr);

    pid_t pid = fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        sleep_ms(150);  // join late
        auto sub = TensorSubscriber::create("e2e/late_joiner");
        int rc = (sub && sub->wait_handshake(3000) == 0) ? 0 : 20;
        _exit(rc);
    }

    // Stream for a while; publish() keeps polling for new consumers.
    pub->handshake_pool();  // no consumer yet — returns 0
    for (std::uint64_t s = 1; s <= 300; ++s) {
        AcquiredDescriptor a = pub->acquire_descriptor();
        a.desc->rank = 1; a.desc->shape[0] = 1; a.desc->dtype = DType::U8;
        a.desc->offset = 0; a.desc->size = 4096;
        pub->publish(std::move(a));
        sleep_ms(2);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

AXON_TEST_MAIN("e2e")
