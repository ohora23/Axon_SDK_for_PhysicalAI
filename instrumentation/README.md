# Hardware instrumentation — proving performance without a robot board

These tools demonstrate dczc's core claims on **this development PC's own hardware**
(RTX 5080 GPU, 16-core CPU, kernel tracing) — no robot/accelerator board required.
Every run goes through a **resource harness** so verification never freezes the box.

| Demo | Proves | Privilege | Status |
|---|---|---|---|
| `gpu/gpu_sidecar_demo` | dczc sidecar carries a **real GPU memory handle** → zero-copy GPU↔GPU across processes | none | ✅ verified |
| `perf/run_pagefaults.sh` | RT loop adds **0 page faults/frame** (mlockall/MAP_POPULATE) | none | ✅ verified |
| `ebpf/run_copy_compare.sh` | payload not copied through the kernel per frame | sudo (bpftrace) | tool |
| `perf/run_membw.sh` | ROS2 burns cache/memory bandwidth on copies; dczc doesn't | sudo (perf HW) | tool |

## 0. Resource harness (freeze protection)

`run_bounded.sh` wraps any command with a **hard CPU-core pin** (`taskset`), a cgroup
**memory ceiling** + no-swap (`systemd-run --user`), lowered priority, and a wall-clock
**timeout**. Even a runaway loop can only saturate the pinned cores — the desktop stays
responsive.

```bash
DCZC_CORES=0-5 DCZC_MEM=8G DCZC_TIMEOUT=120 instrumentation/run_bounded.sh <cmd...>
```

Defaults on a 16-core box: 6 cores, 8 GB, 120 s. Everything below routes through it.

## 1. GPU accelerator — real zero-copy across processes (RTX 5080) ✅

The RTX 5080 *is* an accelerator. This turns the "accelerator import backend (needs a
board)" roadmap item into a live demo on the discrete GPU.

**Path:** a producer allocates CUDA VMM memory, a kernel fills it, the allocation is
exported as a POSIX shareable FD, and that FD is delivered through dczc's SCM_RIGHTS
sidecar (`dczc::detail::send_fds`). The consumer imports the *same physical GPU memory*
and a kernel verifies the payload — the tensor never leaves the GPU; only a 32-byte
commit record crosses host↔device (for sync), never the payload. (design doc §2.3:
`bo_handle` → GPU memory handle.)

```bash
cmake -S . -B build && cmake --build build -j     # builds libdczc.a
instrumentation/gpu/build.sh                        # nvcc → build/gpu_sidecar_demo
DCZC_CORES=0-3 DCZC_TIMEOUT=40 instrumentation/run_bounded.sh \
    ./build/gpu_sidecar_demo 8 200                  # 8 MB/frame × 200 frames
```

**Verified output** (RTX 5080, driver 580 open module, CUDA 12.8):

```
frames validated:    200 / 200   (on-GPU checksum)
corrupt frames:      0
host PAYLOAD copies:  0   (only a 32B commit record crosses host<->device)
GPU data moved zero-copy: 1.68 GB across the process boundary
commit->verify latency: mean=197.9us max=327.6us
FD transport: dczc::detail::send_fds / recv_fds (SCM_RIGHTS sidecar)
```

> Note: `CU_DEVICE_ATTRIBUTE_DMA_BUF_SUPPORTED` was 0 on this driver config, so the demo
> uses CUDA VMM's POSIX shareable handle (also carried by the same sidecar). The proof —
> a GPU memory handle crossing the process boundary via dczc's FD plane, zero payload
> copy — is identical.

## 2. RT page faults = 0 per frame ✅

```bash
instrumentation/perf/run_pagefaults.sh
```

Uses `/usr/bin/time -v` (getrusage) — **no sudo**. Runs the C++ closed-loop demo at 100
vs 2000 frames; if the loop faulted per frame, faults would scale 20×.

**Verified:**

| frames | Minor page-faults | Major (disk) faults |
|---|---|---|
| 100 | 2191 | 3 (one-time binary load) |
| 2000 (20×) | **2191** | **0** |

Identical minor faults despite 20× the frames → the RT streaming loop adds **0 page
faults per frame**; major faults stay 0. The `mlockall` / `MAP_POPULATE` / prefault path
(design doc §3.2, §5.4) holds.

## 3. eBPF copy trace (sudo)

```bash
sudo -v                                   # cache credentials
instrumentation/ebpf/run_copy_compare.sh 2 4
```

`bpftrace` counts `_copy_to_user` / `_copy_from_user` bytes (and socket i/o) by process
while the dczc and ROS2 MockSystem workloads run (bounded). dczc's data path is pure
shared-memory (producer stores into the mmap'd dma-buf, RT consumer loads from it — no
per-frame kernel copy of the payload); ROS2 moves each frame through the DDS transport.

## 4. Memory-subsystem cost (sudo)

```bash
sudo -v
instrumentation/perf/run_membw.sh 2 4
```

`perf stat` compares cache-misses / LLC traffic / context-switches for the same workload.
"Copies aren't free": ROS2 serializes+copies every payload (both sides), spending cache
and memory bandwidth that dczc doesn't. Complements the CPU result already in
[`benchmarks/mock`](../benchmarks/mock/README.md) (dczc flat vs ROS2 3.3× CPU at scale 4).

---

*All demos are resource-bounded. The two privileged ones need `sudo` only because this
host has `kernel.perf_event_paranoid=4` and `bpftrace` requires root; lowering
`perf_event_paranoid` to `1` lets `perf` run unprivileged.*
