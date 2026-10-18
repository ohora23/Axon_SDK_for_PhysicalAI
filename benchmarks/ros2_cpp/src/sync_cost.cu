// SPDX-License-Identifier: Apache-2.0
// sync_cost — what axon's "host-sync before publish" costs a GPU producer, and whether a
// consumer could instead wait on the GPU (cudaStreamWaitEvent on a CUDA IPC event) cheaply.
// Standalone, CUDA runtime only; no transport (axon's publish() itself is ~1 µs of CPU and
// is not the question here).
//
//   sync_cost pipeline <sync_naive|sync_pipelined|event> <kernel_us> <cpu_us> <frames>
//     A producer runs back to back: CPU work (cpu_us of spinning, e.g. preprocessing the
//     next frame), a GPU kernel (kernel_us of spinning, the "inference"), then a 1 MiB copy
//     into a 4-deep transport ring.
//       sync_naive:     enqueue i, cudaStreamSynchronize, publish i, CPU work for i+1.
//       sync_pipelined: CPU work, enqueue i, cudaEventSynchronize(i-1), publish i-1. Still
//                       axon semantics (a frame is published only once it is complete),
//                       but the GPU always has the next frame queued.
//       event:          CPU work, enqueue i, cudaEventRecord, publish i at once (what the
//                       event-based transports do); the producer waits only for the ring
//                       slot it is about to overwrite (frame i-4).
//     Each frame's GPU start and end are stamped with %globaltimer into mapped host memory.
//     Reports frames/s, the GPU's busy fraction, and the per-frame GPU idle time.
//
//   sync_cost ipcwait <rounds>
//     Producer process: a 300 µs kernel that stamps its end, cudaEventRecord on an IPC
//     event, and tells the consumer right away (the event is still pending). Consumer
//     process: cudaStreamWaitEvent on that IPC event, then a kernel that stamps its start;
//     its host never blocks. Reports the host cost of the cudaStreamWaitEvent call and the
//     GPU-side delay from the producer kernel's end to the consumer kernel's start, against
//     the same wait between two streams of one process with a normal event. A second phase
//     does it axon's way: the producer host-syncs, then tells the consumer, which launches
//     its kernel with no event; the same delay then includes the producer's sync wake-up and
//     the pipe, and shows whether the GPU-side cost belongs to the event or to switching
//     between the two processes' contexts.
#include <cuda_runtime_api.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

#define CK(call)                                                                       \
    do {                                                                               \
        cudaError_t e_ = (call);                                                       \
        if (e_ != cudaSuccess) {                                                       \
            std::fprintf(stderr, "%s:%d %s: %s\n", __FILE__, __LINE__, #call,          \
                         cudaGetErrorString(e_));                                      \
            std::exit(1);                                                              \
        }                                                                              \
    } while (0)

static unsigned long long now_ns() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC_RAW, &t);
    return t.tv_sec * 1000000000ull + t.tv_nsec;
}

static void cpu_spin_us(int us) {
    if (us <= 0) return;
    const unsigned long long end = now_ns() + us * 1000ull;
    while (now_ns() < end) {}
}

__device__ __forceinline__ unsigned long long gtimer() {
    unsigned long long t;
    asm volatile("mov.u64 %0, %%globaltimer;" : "=l"(t));
    return t;
}

// The "inference": one thread spinning for ns on the GPU clock, stamping start (and end).
__global__ void spin_kernel(unsigned long long ns, unsigned long long* t_start, unsigned long long* t_end) {
    const unsigned long long t0 = gtimer();
    if (t_start) *t_start = t0;
    while (gtimer() - t0 < ns) {}
    if (t_end) *t_end = gtimer();
}

__global__ void stamp_kernel(unsigned long long* t) { *t = gtimer(); }

static double pct(std::vector<double> v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    const size_t rank = std::min(v.size(), std::max<size_t>(1, (size_t)(p / 100.0 * v.size() + 0.999999)));
    return v[rank - 1];
}

// ---------------- pipeline ----------------
static int run_pipeline(const std::string& mode, int kernel_us, int cpu_us, int frames) {
    constexpr size_t kBytes = 1 << 20;
    constexpr int kRing = 4;
    CK(cudaSetDevice(0));
    cudaStream_t s;
    CK(cudaStreamCreateWithFlags(&s, cudaStreamNonBlocking));
    void* src;
    CK(cudaMalloc(&src, kBytes));
    CK(cudaMemset(src, 0x5a, kBytes));
    void* ring[kRing];
    cudaEvent_t done[kRing];
    for (int i = 0; i < kRing; ++i) {
        CK(cudaMalloc(&ring[i], kBytes));
        CK(cudaEventCreateWithFlags(&done[i], cudaEventDisableTiming));
    }
    unsigned long long *h_start, *h_end, *d_start, *d_end;  // mapped: the GPU writes, we read after
    CK(cudaHostAlloc(&h_start, frames * sizeof(unsigned long long), cudaHostAllocMapped));
    CK(cudaHostAlloc(&h_end, frames * sizeof(unsigned long long), cudaHostAllocMapped));
    CK(cudaHostGetDevicePointer(&d_start, h_start, 0));
    CK(cudaHostGetDevicePointer(&d_end, h_end, 0));

    auto enqueue = [&](int i) {
        spin_kernel<<<1, 1, 0, s>>>(kernel_us * 1000ull, d_start + i, nullptr);
        CK(cudaMemcpyAsync(ring[i % kRing], src, kBytes, cudaMemcpyDeviceToDevice, s));
        stamp_kernel<<<1, 1, 0, s>>>(d_end + i);  // the frame is complete here
        CK(cudaEventRecord(done[i % kRing], s));
    };
    volatile unsigned long long published = 0;  // stands in for TensorPublisher::publish
    auto publish = [&](int i) { published = i; };

    // Warm up (module load, first launches), then time.
    enqueue(0);
    CK(cudaStreamSynchronize(s));
    const unsigned long long t0 = now_ns();
    if (mode == "sync_naive") {
        for (int i = 0; i < frames; ++i) {
            cpu_spin_us(cpu_us);
            enqueue(i);
            CK(cudaStreamSynchronize(s));
            publish(i);
        }
    } else if (mode == "sync_pipelined") {
        for (int i = 0; i < frames; ++i) {
            cpu_spin_us(cpu_us);
            enqueue(i);
            if (i > 0) {
                CK(cudaEventSynchronize(done[(i - 1) % kRing]));
                publish(i - 1);
            }
        }
        CK(cudaEventSynchronize(done[(frames - 1) % kRing]));
        publish(frames - 1);
    } else if (mode == "event") {
        for (int i = 0; i < frames; ++i) {
            cpu_spin_us(cpu_us);
            if (i >= kRing) CK(cudaEventSynchronize(done[i % kRing]));  // slot of frame i-4 is free
            enqueue(i);
            publish(i);
        }
    } else {
        std::fprintf(stderr, "unknown mode %s\n", mode.c_str());
        return 1;
    }
    CK(cudaStreamSynchronize(s));
    const double wall_s = (now_ns() - t0) / 1e9;

    // GPU time per frame (kernel start to copy done), and the GPU's busy fraction over the
    // span from the first frame's start to the last frame's end.
    double gpu_sum_ns = 0;
    for (int i = 0; i < frames; ++i) gpu_sum_ns += double(h_end[i] - h_start[i]);
    const double span_ns = double(h_end[frames - 1] - h_start[0]);
    std::printf("{\"probe\":\"pipeline\",\"mode\":\"%s\",\"kernel_us\":%d,\"cpu_us\":%d,\"frames\":%d,"
                "\"fps\":%.1f,\"frame_us\":%.2f,\"gpu_frame_us\":%.2f,\"gpu_busy\":%.4f,\"gpu_idle_per_frame_us\":%.2f}\n",
                mode.c_str(), kernel_us, cpu_us, frames, frames / wall_s, wall_s * 1e6 / frames,
                gpu_sum_ns / frames / 1e3, gpu_sum_ns / span_ns, (span_ns - gpu_sum_ns) / (frames - 1) / 1e3);
    return 0;
}

// ---------------- ipcwait ----------------
static int run_ipcwait(int rounds) {
    constexpr int kEvents = 8;
    constexpr unsigned long long kProducerNs = 300000;  // 300 µs kernel, so the event is pending
    int handle_pipe[2], round_pipe[2], result_pipe[2];
    if (pipe(handle_pipe) || pipe(round_pipe) || pipe(result_pipe)) return 1;
    const pid_t pid = fork();  // before any CUDA call
    if (pid == 0) {  // consumer
        CK(cudaSetDevice(0));
        cudaStream_t s;
        CK(cudaStreamCreateWithFlags(&s, cudaStreamNonBlocking));
        cudaIpcEventHandle_t h[kEvents];
        if (read(handle_pipe[0], h, sizeof h) != (ssize_t)sizeof h) return 1;
        cudaEvent_t ev[kEvents];
        for (int k = 0; k < kEvents; ++k) CK(cudaIpcOpenEventHandle(&ev[k], h[k]));
        unsigned long long *h_start, *d_start;
        CK(cudaHostAlloc(&h_start, rounds * sizeof(unsigned long long), cudaHostAllocMapped));
        CK(cudaHostGetDevicePointer(&d_start, h_start, 0));
        stamp_kernel<<<1, 1, 0, s>>>(d_start);  // warm up the module
        CK(cudaStreamSynchronize(s));

        std::vector<double> enqueue_us;
        for (int i = 0; i < rounds; ++i) {
            int r;
            if (read(round_pipe[0], &r, sizeof r) != sizeof r) return 1;
            const unsigned long long t = now_ns();
            CK(cudaStreamWaitEvent(s, ev[r % kEvents], 0));
            enqueue_us.push_back((now_ns() - t) / 1e3);
            stamp_kernel<<<1, 1, 0, s>>>(d_start + r);
            // Not timed: keep the rounds apart so an event slot is never re-recorded while
            // a wait on it is still queued.
            CK(cudaStreamSynchronize(s));
        }
        unsigned long long *h_start2, *d_start2;  // phase 2: host-synced producer, no event
        CK(cudaHostAlloc(&h_start2, rounds * sizeof(unsigned long long), cudaHostAllocMapped));
        CK(cudaHostGetDevicePointer(&d_start2, h_start2, 0));
        for (int i = 0; i < rounds; ++i) {
            int r;
            if (read(round_pipe[0], &r, sizeof r) != sizeof r) return 1;
            stamp_kernel<<<1, 1, 0, s>>>(d_start2 + r);
            CK(cudaStreamSynchronize(s));
        }
        std::vector<unsigned long long> prod_end(2 * rounds);
        const size_t want = 2 * rounds * sizeof(unsigned long long);
        size_t got = 0;
        while (got < want) {
            ssize_t n = read(result_pipe[0], reinterpret_cast<char*>(prod_end.data()) + got, want - got);
            if (n <= 0) return 1;
            got += n;
        }
        std::vector<double> gpu_delay_us;
        std::vector<double> hostsync_delay_us;
        for (int i = 10; i < rounds; ++i) {
            gpu_delay_us.push_back((double(h_start[i]) - double(prod_end[i])) / 1e3);
            hostsync_delay_us.push_back((double(h_start2[i]) - double(prod_end[rounds + i])) / 1e3);
        }

        // Baseline in this process: the same wait between two streams with a normal event.
        cudaStream_t a, b;
        CK(cudaStreamCreateWithFlags(&a, cudaStreamNonBlocking));
        CK(cudaStreamCreateWithFlags(&b, cudaStreamNonBlocking));
        cudaEvent_t le;
        CK(cudaEventCreateWithFlags(&le, cudaEventDisableTiming));
        unsigned long long *h_e, *d_e;
        CK(cudaHostAlloc(&h_e, rounds * sizeof(unsigned long long), cudaHostAllocMapped));
        CK(cudaHostGetDevicePointer(&d_e, h_e, 0));
        std::vector<double> local_enqueue_us, local_delay_us;
        for (int i = 0; i < rounds; ++i) {
            spin_kernel<<<1, 1, 0, a>>>(kProducerNs, nullptr, d_e + i);
            CK(cudaEventRecord(le, a));
            const unsigned long long t = now_ns();
            CK(cudaStreamWaitEvent(b, le, 0));
            local_enqueue_us.push_back((now_ns() - t) / 1e3);
            stamp_kernel<<<1, 1, 0, b>>>(d_start + i);
            CK(cudaStreamSynchronize(b));
            if (i >= 10) local_delay_us.push_back((double(h_start[i]) - double(h_e[i])) / 1e3);
        }
        // ...and the host-synced handoff inside one process (no pipe): sync wake-up + launch.
        std::vector<double> local_hostsync_us;
        for (int i = 0; i < rounds; ++i) {
            spin_kernel<<<1, 1, 0, a>>>(kProducerNs, nullptr, d_e + i);
            CK(cudaStreamSynchronize(a));
            stamp_kernel<<<1, 1, 0, b>>>(d_start + i);
            CK(cudaStreamSynchronize(b));
            if (i >= 10) local_hostsync_us.push_back((double(h_start[i]) - double(h_e[i])) / 1e3);
        }
        std::printf("{\"probe\":\"ipcwait\",\"rounds\":%d,\"producer_kernel_us\":%llu,"
                    "\"ipc_wait_enqueue_p50_us\":%.2f,\"ipc_wait_enqueue_p99_us\":%.2f,"
                    "\"ipc_gpu_delay_p50_us\":%.2f,\"ipc_gpu_delay_p90_us\":%.2f,\"ipc_gpu_delay_max_us\":%.2f,"
                    "\"local_wait_enqueue_p50_us\":%.2f,\"local_gpu_delay_p50_us\":%.2f,\"local_gpu_delay_p90_us\":%.2f,"
                    "\"hostsync_gpu_delay_p50_us\":%.2f,\"hostsync_gpu_delay_p90_us\":%.2f,\"local_hostsync_gpu_delay_p50_us\":%.2f}\n",
                    rounds, kProducerNs / 1000, pct(enqueue_us, 50), pct(enqueue_us, 99),
                    pct(gpu_delay_us, 50), pct(gpu_delay_us, 90), pct(gpu_delay_us, 100),
                    pct(local_enqueue_us, 50), pct(local_delay_us, 50), pct(local_delay_us, 90),
                    pct(hostsync_delay_us, 50), pct(hostsync_delay_us, 90), pct(local_hostsync_us, 50));
        return 0;
    }

    CK(cudaSetDevice(0));  // producer
    cudaStream_t s;
    CK(cudaStreamCreateWithFlags(&s, cudaStreamNonBlocking));
    cudaEvent_t ev[kEvents];
    cudaIpcEventHandle_t h[kEvents];
    for (int k = 0; k < kEvents; ++k) {
        CK(cudaEventCreateWithFlags(&ev[k], cudaEventInterprocess | cudaEventDisableTiming));
        CK(cudaIpcGetEventHandle(&h[k], ev[k]));
    }
    if (write(handle_pipe[1], h, sizeof h) != (ssize_t)sizeof h) return 1;
    unsigned long long *h_end, *d_end;
    CK(cudaHostAlloc(&h_end, rounds * sizeof(unsigned long long), cudaHostAllocMapped));
    CK(cudaHostGetDevicePointer(&d_end, h_end, 0));
    for (int i = 0; i < rounds; ++i) {
        spin_kernel<<<1, 1, 0, s>>>(kProducerNs, nullptr, d_end + i);
        CK(cudaEventRecord(ev[i % kEvents], s));
        if (write(round_pipe[1], &i, sizeof i) != sizeof i) return 1;  // event still pending
        CK(cudaStreamSynchronize(s));
        usleep(2000);
    }
    unsigned long long *h_end2, *d_end2;
    CK(cudaHostAlloc(&h_end2, rounds * sizeof(unsigned long long), cudaHostAllocMapped));
    CK(cudaHostGetDevicePointer(&d_end2, h_end2, 0));
    for (int i = 0; i < rounds; ++i) {  // phase 2: axon's way, host-sync then notify
        spin_kernel<<<1, 1, 0, s>>>(kProducerNs, nullptr, d_end2 + i);
        CK(cudaStreamSynchronize(s));
        if (write(round_pipe[1], &i, sizeof i) != sizeof i) return 1;
        usleep(2000);
    }
    const ssize_t n = rounds * sizeof(unsigned long long);
    if (write(result_pipe[1], h_end, n) != n || write(result_pipe[1], h_end2, n) != n) return 1;
    int st;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
}

int main(int argc, char** argv) {
    if (argc == 6 && std::strcmp(argv[1], "pipeline") == 0)
        return run_pipeline(argv[2], std::atoi(argv[3]), std::atoi(argv[4]), std::atoi(argv[5]));
    if (argc == 3 && std::strcmp(argv[1], "ipcwait") == 0) return run_ipcwait(std::atoi(argv[2]));
    std::fprintf(stderr,
                 "usage: sync_cost pipeline <sync_naive|sync_pipelined|event> <kernel_us> <cpu_us> <frames>\n"
                 "       sync_cost ipcwait <rounds>\n");
    return 1;
}
