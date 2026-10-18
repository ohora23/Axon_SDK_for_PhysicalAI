// SPDX-License-Identifier: Apache-2.0
// ipc_event_probe — what it costs a consumer process to wait on a CUDA IPC event
// that a producer process recorded (the completion mechanism ros2_cuda_ipc and
// rosidl::Buffer's CUDA backend use). The producer records the event after a tiny
// memset and host-syncs, so the event is already complete when the consumer is told.
// The consumer stamps the time itself, right before cudaStreamWaitEvent, so pipe and
// scheduler wake-up are not counted; it reports wait + sync of its own stream, and the
// same call on an empty stream as the floor. Standalone (CUDA runtime only); fork
// before any CUDA call.
#include <cuda_runtime_api.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

static unsigned long long now_ns() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC_RAW, &t);
    return t.tv_sec * 1000000000ull + t.tv_nsec;
}

int main() {
    constexpr int kRounds = 300;
    int handle_pipe[2], time_pipe[2];
    if (pipe(handle_pipe) || pipe(time_pipe)) return 1;
    const pid_t pid = fork();
    if (pid == 0) {  // consumer
        cudaSetDevice(0);
        cudaStream_t s;
        cudaStreamCreateWithFlags(&s, cudaStreamNonBlocking);
        cudaIpcEventHandle_t h;
        if (read(handle_pipe[0], &h, sizeof h) != sizeof h) return 1;
        cudaEvent_t ev;
        if (cudaIpcOpenEventHandle(&ev, h) != cudaSuccess) { std::puts("cudaIpcOpenEventHandle failed"); return 1; }
        std::vector<double> wait_us, floor_us;
        for (int i = 0; i < kRounds; ++i) {
            unsigned long long ready;
            if (read(time_pipe[0], &ready, sizeof ready) != sizeof ready) return 1;
            const unsigned long long t = now_ns();
            if (cudaStreamWaitEvent(s, ev, 0) != cudaSuccess || cudaStreamSynchronize(s) != cudaSuccess) {
                std::puts("cudaStreamWaitEvent/cudaStreamSynchronize failed");
                return 1;
            }
            wait_us.push_back((now_ns() - t) / 1e3);
            const unsigned long long c = now_ns();
            cudaStreamSynchronize(s);
            floor_us.push_back((now_ns() - c) / 1e3);
        }
        std::sort(wait_us.begin(), wait_us.end());
        std::sort(floor_us.begin(), floor_us.end());
        std::printf("{\"probe\":\"ipc_event_wait\",\"wait_sync_p50_us\":%.1f,\"wait_sync_p90_us\":%.1f,\"empty_stream_sync_p50_us\":%.1f}\n",
                    wait_us[kRounds / 2], wait_us[kRounds * 9 / 10], floor_us[kRounds / 2]);
        return 0;
    }
    cudaSetDevice(0);  // producer
    cudaStream_t s;
    cudaStreamCreateWithFlags(&s, cudaStreamNonBlocking);
    cudaEvent_t ev;
    cudaEventCreateWithFlags(&ev, cudaEventInterprocess | cudaEventDisableTiming);
    cudaIpcEventHandle_t h;
    cudaIpcGetEventHandle(&h, ev);
    if (write(handle_pipe[1], &h, sizeof h) != sizeof h) return 1;
    void* d;
    cudaMalloc(&d, 4096);
    for (int i = 0; i < kRounds; ++i) {
        cudaMemsetAsync(d, i, 4096, s);
        cudaEventRecord(ev, s);
        cudaStreamSynchronize(s);
        const unsigned long long t = now_ns();
        if (write(time_pipe[1], &t, sizeof t) != sizeof t) return 1;
        usleep(10000);
    }
    int st;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
}
