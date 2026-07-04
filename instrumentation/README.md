# Hardware instrumentation — proving performance without a robot board

These tools demonstrate dczc's core claims on **this development PC's own hardware**
(RTX 5080 GPU, 16-core CPU, kernel tracing) — no robot/accelerator board required.
Every run goes through a **resource harness** so verification never freezes the box.

> 📄 Measured results with full context: [`docs/hardware-verification.md`](../docs/hardware-verification.md).

| Demo | Proves | Privilege | Status |
|---|---|---|---|
| `gpu/gpu_sidecar_demo` | dczc sidecar carries a **real GPU memory handle** → zero-copy GPU↔GPU across processes | none | ✅ verified |
| `perf/run_pagefaults.sh` | RT loop adds **0 page faults/frame** (mlockall/MAP_POPULATE) | none | ✅ verified |
| `perf/run_syscalls.sh` | dczc payload path is **0 transport syscalls/frame** vs ROS2's DDS | none | ✅ verified |
| `perf/run_membw.sh` | ROS2 burns **8.6× the cache-misses** copying payloads; dczc doesn't | perf (paranoid≤1) | ✅ verified |
| `ebpf/run_copy_compare.sh` | direct `copy_to_user` byte count | sudo (bpftrace) | tool |

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

## 3. Data-path syscalls = 0 per frame ✅

```bash
instrumentation/perf/run_syscalls.sh 1 2
```

`strace -f -c` (no sudo) counts transport syscalls. **Verified** (scale 1, ~964 frames,
8 streams):

| | dczc | ROS2 (Fast-RTPS) |
|---|---|---|
| total syscalls | **404** | 10,187 (**25×**) |
| transport (send/recv msg+to+from) | **16** (one-time handshake) | 516 |
| per-frame transport syscalls | **~0** (8 sendmsg / 964 frames) | ~0.5 |

dczc publishes each frame with a seqlock store into shared memory — after the one-time FD
handshake (8 sendmsg for 8 streams), the payload never touches a syscall. ROS2 runs the
DDS machinery for every frame.

## 4. Memory-subsystem cost — copies aren't free ✅

```bash
instrumentation/perf/run_membw.sh 2 4        # perf, no sudo when paranoid≤1
```

`perf stat` compares cache/instruction cost for the same delivered bytes. **Verified**
(scale 2 ≈ 148 MB/s, 4 s):

| counter | dczc | ROS2 | ratio |
|---|---|---|---|
| cache-references | 131 M | 925 M | 7.0× |
| cache-misses | **15.0 M** | **128.2 M** | **8.6×** |
| instructions | 4.9 B | 24.8 B | 5.1× |
| context-switches | 6,931 | 10,427 | 1.5× |

ROS2 serializes+copies every payload (both sides), spending ~8.6× the cache-misses and
~5× the instructions dczc does to move the same data. This is the memory-side companion to
the CPU result in [`benchmarks/mock`](../benchmarks/mock/README.md) (dczc flat vs ROS2
3.3× CPU at scale 4).

## 5. Direct copy_to_user byte count (sudo — optional)

```bash
sudo instrumentation/ebpf/run_copy_compare.sh 2 4
```

`bpftrace` counts `_copy_to_user`/`_copy_from_user` bytes by process. This is the only
tool that still needs root (bpftrace requires it); the §1–§4 results above already
quantify the copy cost without it.

---

*All demos are resource-bounded via `run_bounded.sh`. `perf` runs unprivileged once
`kernel.perf_event_paranoid` is ≤ 1 (`sudo sysctl kernel.perf_event_paranoid=1`);
only the raw `bpftrace` copy counter needs full root.*
