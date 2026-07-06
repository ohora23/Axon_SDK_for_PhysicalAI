// SPDX-License-Identifier: Apache-2.0
// axon GPU sidecar demo — real-hardware zero-copy across processes.
//
// Proves that axon's FD sidecar (the ① FD plane) carries a *GPU memory handle*
// between processes, so a consumer's GPU reads a producer's GPU-written tensor
// with no host staging copy (design doc §2.3: bo_handle -> GPU memory handle).
//
// Path: producer allocates CUDA VMM memory, a kernel fills it, the allocation is
// exported as a POSIX shareable FD, and that FD is delivered through axon's
// SCM_RIGHTS sidecar (axon::detail::send_fds). The consumer imports the same
// physical GPU memory and a kernel verifies the payload — the tensor never
// leaves the GPU; only a 32-byte commit record is copied (for sync), never the
// payload.
//
// This is the accelerator plane on real hardware (RTX 5080) without a robot board.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>

#include <cuda.h>
#include <cuda_runtime.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "axon/detail/sidecar.h"

#define CKC(x) do { CUresult r=(x); if(r!=CUDA_SUCCESS){ const char*s; cuGetErrorString(r,&s); \
    fprintf(stderr,"CUDA drv FAIL %s: %s\n",#x,s); _exit(40);} } while(0)
#define CKR(x) do { cudaError_t r=(x); if(r!=cudaSuccess){ \
    fprintf(stderr,"CUDA rt FAIL %s: %s\n",#x,cudaGetErrorString(r)); _exit(41);} } while(0)

namespace {

constexpr uint64_t MAGIC = 0x9E3779B97F4A7C15ull;

// Payload pattern: deterministic from (index, gen) so the consumer can verify
// on-GPU without any host copy of the payload.
__global__ void fill_kernel(uint64_t* buf, uint64_t n, uint64_t gen) {
    uint64_t i = blockIdx.x * (uint64_t)blockDim.x + threadIdx.x;
    if (i < n) buf[i] = (i * MAGIC) ^ gen;
}

// Count mismatches vs the expected pattern; result stays tiny (one uint64).
__global__ void verify_kernel(const uint64_t* buf, uint64_t n, uint64_t gen,
                              uint64_t* mismatches) {
    uint64_t i = blockIdx.x * (uint64_t)blockDim.x + threadIdx.x;
    if (i < n) {
        if (buf[i] != ((i * MAGIC) ^ gen)) atomicAdd(
            reinterpret_cast<unsigned long long*>(mismatches), 1ull);
    }
}

// 32-byte commit record kept in a tiny separate GPU alloc; only THIS is copied
// host<->device for cross-process sync — never the payload.
struct Commit { uint64_t gen; uint64_t ts_ns; uint64_t n_elems; uint64_t _pad; };

struct FdMeta { uint64_t n_elems; uint64_t bytes; };

uint64_t now_ns() {
    timespec t; clock_gettime(CLOCK_MONOTONIC_RAW, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec;
}

// Allocate `bytes` of device VMM memory with a POSIX-FD shareable handle.
CUdeviceptr vmm_alloc(size_t bytes, CUmemGenericAllocationHandle* h, size_t* aligned) {
    CUmemAllocationProp prop = {};
    prop.type = CU_MEM_ALLOCATION_TYPE_PINNED;
    prop.location.type = CU_MEM_LOCATION_TYPE_DEVICE;
    prop.location.id = 0;
    prop.requestedHandleTypes = CU_MEM_HANDLE_TYPE_POSIX_FILE_DESCRIPTOR;
    size_t gran = 0;
    CKC(cuMemGetAllocationGranularity(&gran, &prop, CU_MEM_ALLOC_GRANULARITY_MINIMUM));
    size_t sz = ((bytes + gran - 1) / gran) * gran;
    CKC(cuMemCreate(h, sz, &prop, 0));
    CUdeviceptr ptr;
    CKC(cuMemAddressReserve(&ptr, sz, 0, 0, 0));
    CKC(cuMemMap(ptr, sz, 0, *h, 0));
    CUmemAccessDesc acc = {};
    acc.location.type = CU_MEM_LOCATION_TYPE_DEVICE;
    acc.location.id = 0;
    acc.flags = CU_MEM_ACCESS_FLAGS_PROT_READWRITE;
    CKC(cuMemSetAccess(ptr, sz, &acc, 1));
    *aligned = sz;
    return ptr;
}

int run_producer(int sock, uint64_t n_elems, int gens) {
    CKR(cudaSetDevice(0));
    CKR(cudaFree(0));  // ensure primary context is current for driver calls

    size_t bytes = n_elems * sizeof(uint64_t);
    CUmemGenericAllocationHandle payload_h, commit_h;
    size_t asz = 0, csz = 0;
    CUdeviceptr payload = vmm_alloc(bytes, &payload_h, &asz);
    CUdeviceptr commit = vmm_alloc(sizeof(Commit), &commit_h, &csz);

    // Export both handles as POSIX FDs and hand them to the consumer through the
    // axon SCM_RIGHTS sidecar.
    int pfd = -1, cfd = -1;
    CKC(cuMemExportToShareableHandle(&pfd, payload_h, CU_MEM_HANDLE_TYPE_POSIX_FILE_DESCRIPTOR, 0));
    CKC(cuMemExportToShareableHandle(&cfd, commit_h, CU_MEM_HANDLE_TYPE_POSIX_FILE_DESCRIPTOR, 0));
    int fds[2] = {pfd, cfd};
    FdMeta meta{n_elems, bytes};
    if (axon::detail::send_fds(sock, fds, 2, &meta, sizeof(meta)) != 0) {
        perror("send_fds"); return 30;
    }

    uint64_t threads = 256, blocks = (n_elems + threads - 1) / threads;
    Commit* commit_host;
    CKR(cudaMallocHost(&commit_host, sizeof(Commit)));  // pinned staging for 32B record

    for (int g = 1; g <= gens; ++g) {
        fill_kernel<<<blocks, threads>>>((uint64_t*)payload, n_elems, (uint64_t)g);
        CKR(cudaGetLastError());
        CKR(cudaDeviceSynchronize());
        // Publish the commit record LAST (after payload is visible on-GPU).
        commit_host->gen = g;
        commit_host->ts_ns = now_ns();
        commit_host->n_elems = n_elems;
        CKC(cuMemcpyHtoD(commit, commit_host, sizeof(Commit)));  // 32B, not payload
        timespec s{0, 2 * 1000 * 1000};  // 2ms cadence (~500 gen/s)
        nanosleep(&s, nullptr);
    }
    // Hold the memory alive until the consumer is done.
    char ack = 0;
    (void)read(sock, &ack, 1);
    return 0;
}

int run_consumer(int sock, int gens) {
    CKR(cudaSetDevice(0));
    CKR(cudaFree(0));

    int fds[axon::detail::kMaxSidecarFds];
    int n = 0;
    FdMeta meta{};
    if (axon::detail::recv_fds(sock, fds, axon::detail::kMaxSidecarFds, &n, &meta, sizeof(meta)) != 0
        || n != 2) {
        fprintf(stderr, "consumer: recv_fds failed (n=%d)\n", n); return 31;
    }

    // Import the SAME physical GPU memory the producer allocated.
    CUmemGenericAllocationHandle payload_h, commit_h;
    CKC(cuMemImportFromShareableHandle(&payload_h, (void*)(intptr_t)fds[0],
                                       CU_MEM_HANDLE_TYPE_POSIX_FILE_DESCRIPTOR));
    CKC(cuMemImportFromShareableHandle(&commit_h, (void*)(intptr_t)fds[1],
                                       CU_MEM_HANDLE_TYPE_POSIX_FILE_DESCRIPTOR));
    size_t gran = 0;
    CUmemAllocationProp prop = {};
    prop.type = CU_MEM_ALLOCATION_TYPE_PINNED;
    prop.location.type = CU_MEM_LOCATION_TYPE_DEVICE;
    prop.location.id = 0;
    prop.requestedHandleTypes = CU_MEM_HANDLE_TYPE_POSIX_FILE_DESCRIPTOR;
    CKC(cuMemGetAllocationGranularity(&gran, &prop, CU_MEM_ALLOC_GRANULARITY_MINIMUM));
    size_t asz = ((meta.bytes + gran - 1) / gran) * gran;
    size_t csz = ((sizeof(Commit) + gran - 1) / gran) * gran;

    auto map = [&](CUmemGenericAllocationHandle h, size_t sz) {
        CUdeviceptr p; CKC(cuMemAddressReserve(&p, sz, 0, 0, 0));
        CKC(cuMemMap(p, sz, 0, h, 0));
        CUmemAccessDesc a = {};
        a.location.type = CU_MEM_LOCATION_TYPE_DEVICE; a.location.id = 0;
        a.flags = CU_MEM_ACCESS_FLAGS_PROT_READWRITE;
        CKC(cuMemSetAccess(p, sz, &a, 1));
        return p;
    };
    CUdeviceptr payload = map(payload_h, asz);
    CUdeviceptr commit = map(commit_h, csz);

    uint64_t* d_mism; CKR(cudaMalloc(&d_mism, sizeof(uint64_t)));
    Commit* commit_host; CKR(cudaMallocHost(&commit_host, sizeof(Commit)));
    uint64_t threads = 256, blocks = (meta.n_elems + threads - 1) / threads;

    int validated = 0, corrupt = 0, last_gen = 0;
    uint64_t lat_sum = 0, lat_max = 0;
    uint64_t deadline = now_ns() + 30ull * 1000000000ull;
    while (validated < gens && now_ns() < deadline) {
        // Read only the 32-byte commit record (sync), never the payload.
        CKC(cuMemcpyDtoH(commit_host, commit, sizeof(Commit)));
        if ((int)commit_host->gen != last_gen && commit_host->gen != 0) {
            last_gen = (int)commit_host->gen;
            uint64_t zero = 0; CKR(cudaMemcpy(d_mism, &zero, 8, cudaMemcpyHostToDevice));
            verify_kernel<<<blocks, threads>>>((uint64_t*)payload, meta.n_elems,
                                               commit_host->gen, d_mism);
            CKR(cudaDeviceSynchronize());
            uint64_t mism = 0; CKR(cudaMemcpy(&mism, d_mism, 8, cudaMemcpyDeviceToHost));
            uint64_t lat = now_ns() - commit_host->ts_ns;
            if (mism == 0) { ++validated; lat_sum += lat; if (lat > lat_max) lat_max = lat; }
            else ++corrupt;
        } else {
            timespec s{0, 200 * 1000}; nanosleep(&s, nullptr);  // 200us poll
        }
    }

    double gb = (double)meta.bytes * validated / 1e9;
    printf("\n────── axon GPU sidecar demo (RTX 5080, real hardware) ──────\n");
    printf("  payload per frame:   %.2f MB  (%llu x uint64)\n",
           meta.bytes / 1e6, (unsigned long long)meta.n_elems);
    printf("  frames validated:    %d / %d   (on-GPU checksum)\n", validated, gens);
    printf("  corrupt frames:      %d   (must be 0 — cross-process GPU integrity)\n", corrupt);
    printf("  host PAYLOAD copies:  0   (only a 32B commit record crosses host<->device)\n");
    printf("  GPU data moved zero-copy: %.2f GB across the process boundary\n", gb);
    if (validated) printf("  commit->verify latency: mean=%.1fus max=%.1fus\n",
                          (lat_sum / (double)validated) / 1e3, lat_max / 1e3);
    printf("  FD transport: axon::detail::send_fds / recv_fds (SCM_RIGHTS sidecar)\n");
    printf("─────────────────────────────────────────────────────────────\n");

    char ack = 1; (void)write(sock, &ack, 1);
    fflush(stdout);  // child _exit()s without flushing stdio buffers
    return (validated >= gens && corrupt == 0) ? 0 : 32;
}

}  // namespace

int main(int argc, char** argv) {
    uint64_t mb = (argc > 1) ? strtoull(argv[1], nullptr, 10) : 8;
    int gens = (argc > 2) ? atoi(argv[2]) : 100;
    uint64_t n_elems = (mb * 1024 * 1024) / sizeof(uint64_t);

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) { perror("socketpair"); return 1; }

    fprintf(stderr, "axon GPU demo: %llu MB/frame x %d frames, FD via axon sidecar\n",
            (unsigned long long)mb, gens);

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 1; }
    if (pid == 0) { close(sv[0]); _exit(run_consumer(sv[1], gens)); }
    close(sv[1]);
    int prc = run_producer(sv[0], n_elems, gens);
    int status = 0; waitpid(pid, &status, 0);
    int crc = WIFEXITED(status) ? WEXITSTATUS(status) : 99;
    return (prc == 0 && crc == 0) ? 0 : 1;
}
