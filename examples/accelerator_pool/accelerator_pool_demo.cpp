// SPDX-License-Identifier: Apache-2.0
// axon R6 demo — cross-process device zero-copy through the *library* API.
//
// The producer builds a PoolBackend::Accelerator pool (CUDA VMM device
// buffers). It writes a known pattern into pool.device_ptr(0) and ships the
// buffer's POSIX export handle (pool.dma_buf_fds()[0]) over the axon sidecar.
// The consumer — a separate process, standing in for a different framework —
// imports the SAME physical GPU allocation and reads it back. Only the FD and a
// 16-byte descriptor cross the socket; the payload never leaves the GPU.
//
// Driver API only (cuMem*), so it builds with g++ + libcuda (no nvcc).

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cuda.h>

#include "axon/pool.h"
#include "axon/detail/sidecar.h"

using namespace axon;

namespace {

constexpr std::uint64_t MAGIC = 0x9E3779B97F4A7C15ull;
constexpr std::uint64_t SEED  = 0xA5A5A5A5ull;
constexpr std::uint64_t N_ELEMS = 256 * 1024;                 // 2 MiB payload
constexpr std::size_t   BYTES   = N_ELEMS * sizeof(std::uint64_t);

struct Meta { std::uint64_t n_elems; std::uint64_t bytes; };

#define CK(expr) do { CUresult r_ = (expr); if (r_ != CUDA_SUCCESS) { \
    const char* s_ = nullptr; cuGetErrorString(r_, &s_); \
    std::fprintf(stderr, "%s:%d %s -> %s\n", __FILE__, __LINE__, #expr, s_ ? s_ : "?"); \
    return -1; } } while (0)

int run_producer(int sock) {
    auto pool = TensorPool::create(
        TensorPoolConfig{4, BYTES, PoolBackend::Accelerator, nullptr});
    if (!pool) { std::fprintf(stderr, "producer: Accelerator pool create failed\n"); return 20; }

    void* dptr = pool->device_ptr(0);
    if (!dptr || pool->dma_buf_fds().empty()) {
        std::fprintf(stderr, "producer: no device_ptr/export fd\n"); return 21;
    }

    // Fill the device buffer with a checkable pattern (host staging -> HtoD).
    std::vector<std::uint64_t> host(N_ELEMS);
    for (std::uint64_t i = 0; i < N_ELEMS; ++i) host[i] = (i * MAGIC) ^ SEED;
    CK(cuMemcpyHtoD(reinterpret_cast<CUdeviceptr>(dptr), host.data(), BYTES));

    // Hand the export FD (not the bytes) to the consumer.
    int fd = pool->dma_buf_fds()[0];
    Meta meta{N_ELEMS, BYTES};
    if (detail::send_fds(sock, &fd, 1, &meta, sizeof(meta)) != 0) {
        std::perror("producer: send_fds"); return 22;
    }

    char ack = 0;
    ssize_t rr = read(sock, &ack, 1);   // hold GPU memory alive until consumer is done
    (void)rr;
    return ack == 1 ? 0 : 23;
}

int run_consumer(int sock) {
    CUdevice dev; CUcontext ctx;
    CK(cuInit(0));
    CK(cuDeviceGet(&dev, 0));
    CK(cuDevicePrimaryCtxRetain(&ctx, dev));
    CK(cuCtxSetCurrent(ctx));

    int fds[detail::kMaxSidecarFds]; int n = 0; Meta meta{};
    if (detail::recv_fds(sock, fds, detail::kMaxSidecarFds, &n, &meta, sizeof(meta)) != 0 || n != 1) {
        std::fprintf(stderr, "consumer: recv_fds failed (n=%d)\n", n); return 30;
    }

    // Import the SAME physical GPU memory and map it into this process.
    CUmemGenericAllocationHandle h;
    CK(cuMemImportFromShareableHandle(&h, reinterpret_cast<void*>(static_cast<std::intptr_t>(fds[0])),
                                      CU_MEM_HANDLE_TYPE_POSIX_FILE_DESCRIPTOR));
    CUmemAllocationProp prop = {};
    prop.type = CU_MEM_ALLOCATION_TYPE_PINNED;
    prop.location.type = CU_MEM_LOCATION_TYPE_DEVICE;
    prop.location.id = 0;
    prop.requestedHandleTypes = CU_MEM_HANDLE_TYPE_POSIX_FILE_DESCRIPTOR;
    std::size_t gran = 0;
    CK(cuMemGetAllocationGranularity(&gran, &prop, CU_MEM_ALLOC_GRANULARITY_MINIMUM));
    std::size_t sz = ((meta.bytes + gran - 1) / gran) * gran;

    CUdeviceptr ptr;
    CK(cuMemAddressReserve(&ptr, sz, 0, 0, 0));
    CK(cuMemMap(ptr, sz, 0, h, 0));
    CUmemAccessDesc acc = {};
    acc.location.type = CU_MEM_LOCATION_TYPE_DEVICE; acc.location.id = 0;
    acc.flags = CU_MEM_ACCESS_FLAGS_PROT_READWRITE;
    CK(cuMemSetAccess(ptr, sz, &acc, 1));

    // Read back and verify — proves it is the producer's buffer, no host copy of
    // the payload ever crossed the socket.
    std::vector<std::uint64_t> host(meta.n_elems);
    CK(cuMemcpyDtoH(host.data(), ptr, meta.bytes));
    std::uint64_t bad = 0;
    for (std::uint64_t i = 0; i < meta.n_elems; ++i)
        if (host[i] != ((i * MAGIC) ^ SEED)) ++bad;

    char ack = bad == 0 ? 1 : 0;
    ssize_t wr = write(sock, &ack, 1);
    (void)wr;

    std::printf("\n────── axon R6 accelerator-pool demo (consumer) ──────\n");
    std::printf("  imported export FD from sidecar (payload stayed on GPU)\n");
    std::printf("  device zero-copy: %s (%llu/%llu elems match, %.1f MiB)\n",
                bad == 0 ? "OK" : "MISMATCH",
                (unsigned long long)(meta.n_elems - bad), (unsigned long long)meta.n_elems,
                static_cast<double>(meta.bytes) / 1048576.0);
    std::printf("──────────────────────────────────────────────────────\n");
    std::fflush(stdout);
    return bad == 0 ? 0 : 31;
}

}  // namespace

int main() {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) { std::perror("socketpair"); return 1; }

    std::fprintf(stderr, "R6 accelerator-pool demo: %.1f MiB device buffer, cross-process import\n",
                 static_cast<double>(BYTES) / 1048576.0);

    pid_t pid = fork();
    if (pid < 0) { std::perror("fork"); return 1; }
    if (pid == 0) { ::close(sv[0]); _exit(run_consumer(sv[1])); }

    ::close(sv[1]);
    int prc = run_producer(sv[0]);
    int status = 0; waitpid(pid, &status, 0);
    int crc = WIFEXITED(status) ? WEXITSTATUS(status) : 99;
    return (prc == 0 && crc == 0) ? 0 : 1;
}
