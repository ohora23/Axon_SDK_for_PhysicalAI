# Hardware verification results

Measured evidence for axon's core claims, produced on a single workstation with
**no robot or accelerator board** — the machine's own RTX 5080 GPU, 16-thread CPU,
and kernel tracing. Every run is resource-bounded (`instrumentation/run_bounded.sh`)
so verification never freezes the desktop. Tooling lives in
[`instrumentation/`](../instrumentation/README.md).

> **Environment.** NVIDIA GeForce RTX 5080, 16 GB, compute 12.0, driver 580.167.08
> (open kernel module), CUDA 12.8 · AMD Ryzen 7 9800X3D (8-core/16-thread), 96 MB L3 ·
> kernel 6.17 (non-RT), 80 GB RAM, `/dev/udmabuf` present · `bpftrace`/`perf`/`strace`.
> Numbers are machine-specific; reproduce on your target.

## Summary

| # | Demo | Proves | Privilege | Status |
|---|---|---|---|---|
| 1 | GPU sidecar | axon sidecar carries a **real GPU memory handle** → zero-copy GPU↔GPU across processes | none | ✅ |
| 2 | page-fault | RT loop adds **0 page faults / frame** | none | ✅ |
| 3 | syscalls | axon payload path is **0 transport syscalls / frame** vs ROS2 (25× total) | none | ✅ |
| 4 | memory/cache | ROS2 spends **8.6× the cache-misses** copying payloads; axon doesn't | perf (paranoid≤1) | ✅ |
| 5 | eBPF copy | direct `copy_to_user` byte count | sudo | tool |
| 6 | Jetson NvBufSurface | **camera → CUDA with no copy** across processes on a real Jetson + UVC camera | none | ✅ |
| 7 | 1 kHz RT jitter | `cyclictest` worst case on the Jetson — stock-kernel baseline measured; PREEMPT_RT run pending | sudo (once) | 🟡 |

## 0. Resource harness (freeze protection)

`run_bounded.sh` wraps every command with a hard CPU-core pin (`taskset`), a cgroup
memory ceiling + no-swap (`systemd-run --user`), lowered priority (`nice`), and a
wall-clock `timeout`. The core pin is the real protection — even a runaway loop only
saturates the pinned cores (6 of 16 by default), leaving the desktop responsive.
Overridable via `AXON_CORES` / `AXON_MEM` / `AXON_TIMEOUT`.

```bash
AXON_CORES=0-3 AXON_MEM=4G AXON_TIMEOUT=40 instrumentation/run_bounded.sh <cmd...>
```

Verified enforcing: `MemoryMax=2G` → cgroup `memory.max = 2147483648`; `taskset -c 0-1`
→ affinity `0,1`, `nproc = 2`.

## 1. GPU accelerator — real zero-copy across processes (RTX 5080) ✅

The RTX 5080 *is* the accelerator. This turns the "accelerator import backend (needs a
board)" roadmap item into a live demo on the discrete GPU.

**Feasibility probe.** `CU_DEVICE_ATTRIBUTE_DMA_BUF_SUPPORTED = 0` on this driver config,
but `VIRTUAL_MEMORY_MANAGEMENT_SUPPORTED = 1` and `cuMemExportToShareableHandle`
(POSIX FD) succeeds. So the demo shares GPU memory via a **POSIX shareable FD** carried
by axon's sidecar — the proof (a GPU handle crossing the process boundary via the FD
plane, zero payload copy) is identical to the dma-buf case.

**Mechanism.** The producer allocates CUDA VMM memory, a kernel fills it
(`buf[i] = (i*MAGIC) ^ gen`), and the allocation is exported as a POSIX FD handed to the
consumer through **`axon::detail::send_fds`** (the real ① FD-plane primitive). The
consumer `recv_fds` → `cuMemImportFromShareableHandle` maps the *same physical GPU
memory* and a verify kernel checks the payload on-GPU. The tensor never leaves the GPU;
only a 32-byte commit record (`gen`, `ts`, `n`) crosses host↔device for sync.

```bash
cmake -S . -B build && cmake --build build -j        # builds libaxon.a
instrumentation/gpu/build.sh                           # nvcc → build/gpu_sidecar_demo
AXON_CORES=0-3 AXON_TIMEOUT=40 instrumentation/run_bounded.sh \
    ./build/gpu_sidecar_demo 8 200                     # 8 MB/frame × 200 frames
```

```
────── axon GPU sidecar demo (RTX 5080, real hardware) ──────
  payload per frame:   8.39 MB  (1048576 x uint64)
  frames validated:    200 / 200   (on-GPU checksum)
  corrupt frames:      0   (must be 0 — cross-process GPU integrity)
  host PAYLOAD copies:  0   (only a 32B commit record crosses host<->device)
  GPU data moved zero-copy: 1.68 GB across the process boundary
  commit->verify latency: mean=197.9us max=327.6us
  FD transport: axon::detail::send_fds / recv_fds (SCM_RIGHTS sidecar)
```

**Result.** axon's FD sidecar carried a real RTX 5080 GPU memory handle between processes;
the consumer GPU read the producer's GPU-written data with **0 host copies**, moving
1.68 GB zero-copy across the process boundary, 200/200 frames validated. Real-hardware
proof of design §2.3 (`bo_handle` → GPU memory handle).

## 2. RT page faults = 0 per frame ✅

`/usr/bin/time -v` (getrusage) — no sudo. The C++ closed-loop demo runs at 100 vs 2000
frames; if the loop faulted per frame the count would scale 20×.

```bash
instrumentation/perf/run_pagefaults.sh
```

| frames | Minor page-faults | Major (disk) faults |
|---|---|---|
| 100 | 2191 | 3 (one-time binary load) |
| 2000 (20×) | **2191** | **0** |

Identical minor faults at 20× the frames → the RT streaming loop adds **0 faults/frame**;
major faults stay 0. The `mlockall` / `MAP_POPULATE` / prefault path (design §3.2, §5.4)
holds — this is the $T_{view}\approx 0$ term of the bounded-staleness formula, measured.

## 3. Data-path syscalls = 0 per frame ✅

`strace -f -c` (tracing your own children needs no privilege).

```bash
instrumentation/perf/run_syscalls.sh 1 2
```

Measured (scale 1, ~964 frames, 8 streams, 2 s):

| | axon | ROS2 (Fast-RTPS) |
|---|---|---|
| total syscalls | **404** | 10,187 (**25×**) |
| transport (sendmsg+recvmsg+sendto+recvfrom) | **16** | 516 |
| per-frame transport syscalls | **~0** | ~0.5 |

axon's 16 transport syscalls are exactly the one-time FD handshake (8 streams × 1 sendmsg);
across 964 published frames it adds **0 transport syscalls** — each frame is a seqlock
store into shared memory. ROS2 runs 25× the syscalls (DDS machinery). Note: Fast-RTPS uses
SHM for large localhost payloads, so the payload copy shows up in §4's cache-misses rather
than as `sendmsg`.

## 4. Memory-subsystem cost — copies aren't free ✅

`perf stat` compares cache/instruction cost for the same delivered bytes (`perf` runs
unprivileged once `kernel.perf_event_paranoid ≤ 1`).

```bash
instrumentation/perf/run_membw.sh 2 4
```

Measured (scale 2 ≈ 148 MB/s, 4 s):

| counter | axon | ROS2 | ratio |
|---|---|---|---|
| cache-references | 131 M | 925 M | 7.0× |
| cache-misses | **15.0 M** | **128.2 M** | **8.6×** |
| instructions | 4.9 B | 24.8 B | 5.1× |
| context-switches | 6,931 | 10,427 | 1.5× |

These are rclpy (Python) numbers. In C++ against ROS 2's zero-copy path (loaned messages + Fast DDS data-sharing), the same
workload at 297 MB/s, counted over the measured window, gives 0.88 M vs 4.38 M cache-misses per second (5×) and 0.023 B vs
0.123 B instructions per second (5.3×); see
[`benchmarks/ros2_cpp`](../benchmarks/ros2_cpp/README.md#multi-stream-cpu-delivery-and-cache-misses).

ROS2 (rclpy) serializes+copies every payload (both sides), spending ~8.6× the cache-misses and
~5× the instructions axon uses to move the same data — the memory-side companion to the
CPU result in [`benchmarks/mock`](../benchmarks/mock/README.md) (axon flat vs ROS2 3.3×
CPU at scale 4).

## 5. Direct copy_to_user byte count (sudo — optional)

```bash
sudo instrumentation/ebpf/run_copy_compare.sh 2 4
```

`bpftrace` counts `_copy_to_user`/`_copy_from_user` bytes by process. The only tool that
still needs root; §1–§4 already quantify the copy cost without it.

## 6. Jetson — camera → CUDA zero-copy via NvBufSurface (AGX Xavier) ✅

The one section run on a real board, not the workstation above.

> **Environment.** NVIDIA Jetson AGX Xavier, L4T R35.6.4 (JetPack 5.1.6), kernel
> 5.10.216-tegra, CUDA 11.4 · Logitech Webcam Pro 9000 (UVC, YUYV 640×480).

```bash
instrumentation/jetson/build.sh          # on the board (needs jetson_multimedia_api)
instrumentation/jetson/nvbuf_probe /dev/video0
```

`nvbuf_probe` forks before any CUDA init; the parent allocates 4 `NvBufSurface`
(`NVBUF_MEM_SURFACE_ARRAY`, YUYV pitch-linear) and captures 30 frames, the child
receives the FDs over `SCM_RIGHTS`. Same result on 3/3 runs:

| # | Question | Result |
|---|---|---|
| P1 | Does UVC capture straight **into** an NvBufSurface? | ✅ accepted as `V4L2_MEMORY_DMABUF` targets (`bytesperline` = surface pitch 1280) |
| P2 | Does the FD resolve in another process? | ✅ via `NvBufSurfaceImport` + the producer's `NvBufSurfaceGetMapParams` (520 B POD). `NvBufSurfaceFromFd` **fails** cross-process |
| P3 | Can CUDA address the same memory with no copy? | ✅ EGLImage → `cuGraphicsEGLRegisterImage`: device hash == CPU hash on **30/30** frames; a GPU `cuMemsetD8` is read back by the producer |
| P4 | Is a plain `mmap()` of the FD coherent? | ✅ **30/30** — axon's existing host path works unchanged |

Also on the same board: the CUDA VMM accelerator pool (§1) passes `test_accel_e2e`, and the
week-1 spike PoC runs against the real camera (staleness 0.07–0.1 ms).

Pitfalls found: an imported surface has `numFilled = 0`, so `NvBufSurfaceSyncForCpu`
rejects index 0 until it is set to 1; JetPack 5 ships cmake 3.16 (axon needs ≥ 3.22).

## 7. 1 kHz RT jitter on the Jetson (AGX Xavier) 🟡

Target: worst-case wake-up jitter **< 100 µs** for a 1 kHz control loop. Same board as §6.
Scripts: [`instrumentation/jetson/rt/`](../instrumentation/jetson/rt/).

```bash
instrumentation/jetson/rt/run_jitter.sh baseline 300   # on the board: idle, then stress-ng
```

`cyclictest -m -p 95 -t -a -i 1000` (SCHED_FIFO 95, `mlockall`, one thread per CPU,
1 ms period), 5 min idle and 5 min under `stress-ng --cpu 8 --io 2 --vm 2`.
nvpmodel `MODE_30W_ALL`, governor `schedutil`.

**Baseline — stock kernel `5.10.216-tegra` (`CONFIG_PREEMPT`, not RT):**

| Condition | Avg (per CPU) | Worst | Samples > 1 ms |
|---|---|---|---|
| idle | 34–38 µs | **1,138 µs** | 1 / 2.4 M |
| stress-ng load | 57–61 µs | **1,263 µs** | 177 / 2.4 M |

The stock kernel misses the target by >10×, so the RT number needs a PREEMPT_RT kernel.
NVIDIA publishes prebuilt RT kernels only for r36 (Orin); for Xavier on r35,
`build-rt.sh` cross-builds one from the L4T r35.6.4 public sources with their bundled
`rt-patches` (`5.10.216-rt108-tegra`, ~10 min on a 9800X3D), and `install-rt.sh` adds it as
an extlinux entry next to the stock kernel. **Status:** the first boot of that kernel did
not come back on the network (Tailscale and LAN both silent); diagnosis needs the board's
console. The config diff against the stock kernel is only the RT switches — root-fs (NVMe,
PCIe) and the USB-Ethernet (`r8152`) drivers are built in on both.

Pitfall: the stock Jetson kernel has `CONFIG_RT_GROUP_SCHED=y` and systemd gives
`user.slice` `cpu.rt_runtime_us = 0`, so SCHED_FIFO fails with `EPERM` **even with**
`rtprio 99` in `limits.d` — an axon RT loop run from a login shell hits the same wall.
Fix: `echo 950000 | sudo tee /sys/fs/cgroup/cpu,cpuacct/user.slice/cpu.rt_runtime_us`
(not persistent), or a kernel without `RT_GROUP_SCHED` (`build-rt.sh` turns it off).

## Appendix — perf privilege setup

This host defaults to `kernel.perf_event_paranoid = 4` (strict). A Claude/agent shell has
no tty (so `sudo` can't prompt) and Ubuntu's `tty_tickets` isolates another terminal's
cached credentials, so instead of `sudo` we lower the knob:

```bash
sudo sysctl kernel.perf_event_paranoid=1              # temporary (resets on reboot)
# or persist: echo 'kernel.perf_event_paranoid=1' | sudo tee /etc/sysctl.d/99-perf.conf
```

Then `perf` hardware/software counters and `strace` run unprivileged; only raw tracepoints
(`/sys/kernel/tracing`) and `bpftrace` still require root.
