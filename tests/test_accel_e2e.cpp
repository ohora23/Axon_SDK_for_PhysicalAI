// SPDX-License-Identifier: Apache-2.0
// A-2 end-to-end: cross-process GPU zero-copy through the PUBLIC library API.
//
// The producer builds an Accelerator (CUDA VMM) pool + a TensorPublisher, writes
// a per-frame pattern straight into pool.device_ptr(idx), and publishes only the
// descriptor. The consumer uses TensorSubscriber — NO raw cuMemImport, NO sidecar
// calls — and reads latest_view()->device_ptr back off the GPU. A match proves
// the same physical device buffer crossed the process boundary via the library
// alone.
//
// Built only under AXON_WITH_CUDA; registered with ctest only under AXON_TEST_CUDA
// (opt-in, where a CUDA device is present). CUDA + fork() is unsafe once a context
// exists, so — like the R6 demo — we fork FIRST and each process inits CUDA on its
// own side.

#include "axon/pool.h"
#include "axon/publisher.h"
#include "axon/subscriber.h"
#include "axon_test.h"

#include <cstdint>
#include <ctime>
#include <vector>

#include <cuda.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace axon;

namespace {

constexpr std::uint64_t MAGIC   = 0x9E3779B97F4A7C15ull;
constexpr std::uint64_t N_ELEMS = 4096;
constexpr std::size_t   BYTES   = N_ELEMS * sizeof(std::uint64_t);
constexpr std::uint64_t kFinal  = 40;
const char* kService = "e2e/accel_stream";

void sleep_ms(int ms) {
    struct timespec t {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&t, nullptr);
}

// Frame `seq`, element `i` — a value that depends on both, so a stale or wrong
// buffer is detectable.
std::uint64_t elem(std::uint64_t seq, std::uint64_t i) { return (i * MAGIC) ^ seq; }

// Consumer process: library API only.
int run_consumer() {
    auto sub = TensorSubscriber::create(kService);
    if (!sub) return 10;
    if (sub->wait_handshake(5000) != 0) return 11;

    std::vector<std::uint64_t> host(N_ELEMS);
    for (int i = 0; i < 5000; ++i) {
        auto v = sub->latest_view(8);
        if (v && v->seqno >= kFinal) {
            // Must be device-backed: a device pointer and no host view.
            if (v->device_ptr == nullptr || v->data != nullptr) return 12;
            CUdeviceptr dp = reinterpret_cast<CUdeviceptr>(v->device_ptr);
            if (cuMemcpyDtoH(host.data(), dp, BYTES) != CUDA_SUCCESS) return 13;
            std::uint64_t seq = v->seqno;
            std::uint64_t bad = 0;
            for (std::uint64_t k = 0; k < N_ELEMS; ++k)
                if (host[k] != elem(seq, k)) ++bad;
            return bad == 0 ? 0 : 14;
        }
        sleep_ms(1);
    }
    return 15;  // never saw the final frame
}

}  // namespace

AXON_TEST(accel_cross_process_zero_copy) {
    // Fork BEFORE any CUDA init (CUDA + fork after context creation is unsafe).
    pid_t pid = fork();
    REQUIRE(pid >= 0);
    if (pid == 0) _exit(run_consumer());

    // Parent = producer. Pool creation inits CUDA on this side.
    auto pool = TensorPool::create(
        TensorPoolConfig{6, BYTES, PoolBackend::Accelerator, nullptr});
    REQUIRE(pool != nullptr);   // AXON_TEST_CUDA asserts a usable GPU is present
    auto pub = TensorPublisher::create(kService, *pool);
    REQUIRE(pub != nullptr);

    int accepted = pub->handshake_pool();
    CHECK(accepted >= 1);

    std::vector<std::uint64_t> stage(N_ELEMS);
    for (std::uint64_t s = 1; s <= kFinal; ++s) {
        AcquiredDescriptor a = pub->acquire_descriptor();
        REQUIRE(a.buffer_index >= 0);
        for (std::uint64_t k = 0; k < N_ELEMS; ++k) stage[k] = elem(s, k);
        CUdeviceptr dp =
            reinterpret_cast<CUdeviceptr>(pool->device_ptr(a.buffer_index));
        REQUIRE(dp != 0);
        CHECK(cuMemcpyHtoD(dp, stage.data(), BYTES) == CUDA_SUCCESS);

        a.desc->rank = 1;
        a.desc->shape[0] = static_cast<std::uint32_t>(N_ELEMS);
        a.desc->dtype = DType::I64;
        a.desc->offset = 0;
        a.desc->size = BYTES;
        a.desc->sync_fence_kind = SyncFenceKind::None;
        pub->publish(std::move(a));
        sleep_ms(2);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status));
    if (WIFEXITED(status) && WEXITSTATUS(status) != 0)
        std::printf("    accel consumer exit code = %d\n", WEXITSTATUS(status));
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

AXON_TEST_MAIN("accel_e2e")
