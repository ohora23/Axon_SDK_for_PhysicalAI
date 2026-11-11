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
| 6 | Jetson NvBufSurface | **camera → CUDA with no copy** across processes on a real Jetson + UVC camera; camera → TensorRT end to end: zero-copy saves the H2D copy (0.2-0.4 ms); GPU wake-up and camera transfer dominate | none | ✅ |
| 7 | 1 kHz RT jitter | `cyclictest` worst case on the Jetson: PREEMPT_RT + isolated CPUs 133 / 226 µs (idle / load) at 30 W, 74 / 148 µs at MAXN; RT alone keeps 1.1-1.2 ms spikes | sudo (once) | 🟡 |

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
workload at 297 MB/s, counted over the measured window, gives 0.87 M vs 3.66 M cache-misses per second (4.2×) and 0.024 B vs
0.127 B instructions per second (5.3×) for axon's 1 kHz loop; see
[`benchmarks/ros2_cpp`](../benchmarks/ros2_cpp/README.md#multi-stream-cpu-latency-delivery-and-cache-misses).

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

### Camera → inference, end to end (zero-copy against a copy path)

White paper: [Small cameras, big costs](whitepapers/axon-jetson-camera-inference-e2e.pdf) (PDF, built from
[`whitepapers/jetson-camera-e2e/`](whitepapers/jetson-camera-e2e/)).

[`examples/jetson_e2e`](../examples/jetson_e2e/) feeds the same camera to a ResNet-18
(TensorRT 8.5, FP16, 224×224) in another process, either straight from the `NvBufSurface`
pool (zerocopy: the driver writes each frame into the pool slot, no user-space copy) or through
the usual copies: V4L2 `MMAP` buffer → `memcpy` into a host pool → pageable `cudaMemcpyAsync`
H2D (copy). Both run the same YUYV→RGB pre-processing kernel and engine. PREEMPT_RT kernel,
consumer on isolated CPU 4, producer on CPUs 0-3, `MODE_30W_ALL`; 1,200 frames per cell (2 runs
× 600), none skipped. Raw per-frame CSV in
[`instrumentation/jetson/e2e/results/`](../instrumentation/jetson/e2e/results/)
(`*_warm`: table below; `*_nowarm`: the first runs, discussed under it). p50 (p99) in µs, with
the GPU woken before each frame is taken (`E2E_WARM_GPU=1`; it runs before the frame is read, so
it lands in publish → received, outside the frame-processing stages below):

| stage | zerocopy | copy | zerocopy, `jetson_clocks` | copy, `jetson_clocks` |
|---|---|---|---|---|
| capture → publish | 32,097 (36,164) | 32,204 (36,315) | 32,092 (36,155) | 32,194 (36,273) |
| frame on the GPU (H2D) | 0 | 423 (504) | 0 | 184 (263) |
| pre-processing kernel | 234 (275) | 237 (276) | 60 (109) | 62 (139) |
| inference | 10,819 (10,892) | 10,819 (10,874) | 1,684 (1,730) | 1,689 (1,752) |
| **received → inferred** | **11,056** (11,134) | 11,486 (11,575) | **1,746** (1,800) | 1,947 (2,115) |

- **Zero-copy saves the H2D copy, and nothing else:** 0.43 ms at the default clocks and 0.20 ms with
  `jetson_clocks`, from receive to inference (4 % and 10 %); over the whole path including the
  producer's `memcpy` (about 0.1 ms, in capture → publish) 0.5 and 0.3 ms. The pre-processing
  kernel takes the same time on the `NvBufSurface` as on a `cudaMalloc` buffer.
- **Waking the GPU costs more than the copy.** The GPU idles between 30 fps frames; the first GPU
  step after the idle pays about 0.9-1.1 ms at the default clocks and 0.3-0.35 ms with
  `jetson_clocks`.
  In the first runs (`*_nowarm`) that step was the kernel in zerocopy and the H2D copy in copy,
  so zerocopy's kernel looked 4.7× slower (1,163 against 245 µs) and copy's H2D 1,338 µs; an
  earlier version of this section read that as slow reads of the surface, which the warm runs
  rule out (the `*_nowarm` env.txt predates the clock record; the directory name is the only
  record of `jetson_clocks` there). Receive → inferred in those runs: 12,010 against 12,427 µs, 2,077 against 2,271 µs.
  With the warm-up, publish → received grows by the same amount (1,173 µs against 106 µs at the
  default clocks), because the warm-up runs there.
- **The camera dominates the total.** uvcvideo stamps the frame when its first USB packet
  arrives (`hwtimestamps=0`); about 32 ms then go to the USB 2.0 transfer of the frame before it
  can be published. The exposure happens before the timestamp and is not in these numbers.
- **GPU clock, not the transport, sets the inference time:** 10.8 ms at the default 30 W clocks
  (GPU at 115 MHz when idle, devfreq up to 905 MHz) against 1.7 ms with `jetson_clocks` (905 MHz
  fixed; `trtexec` alone: 1.6 ms).
- publish → received without the warm-up, about 106 µs, is mostly the 100 µs polling interval of
  `wait_for_update` on this build (JetPack 5 headers have no `futex_waitv`).
- With a higher-resolution or faster camera the H2D copy grows with the frame; a RealSense run
  is planned. The copy path here is pageable; pinned or mapped host memory would shorten it.

## 7. 1 kHz RT jitter on the Jetson (AGX Xavier) 🟡

Target: worst-case wake-up jitter **< 100 µs** for a 1 kHz control loop. Same board as §6.
Scripts: [`instrumentation/jetson/rt/`](../instrumentation/jetson/rt/); raw `cyclictest`
output, environment dumps and trace excerpts in [`results/`](../instrumentation/jetson/rt/results/).

`cyclictest -m -p 95 -a -i 1000` (SCHED_FIFO 95, `mlockall`, 1 ms period, one thread per
measured CPU), 5 min idle, then 5 min under `stress-ng --io 2 --vm 2 --vm-bytes 512M` plus
`--cpu 8` (all CPUs) or `--cpu 4` (isolated runs, where the scheduler keeps it on CPUs 0-3).

| Kernel and setup | CPUs measured | Avg | Worst idle / load | Samples > 1 ms, idle | Samples > 1 ms, load |
|---|---|---|---|---|---|
| stock `5.10.216-tegra` (`CONFIG_PREEMPT`), 30 W | 0-7 | 34-61 µs | 1,138 / 1,263 µs | 1 / 2.4 M | 177 / 2.4 M |
| PREEMPT_RT `5.10.216-rt108-tegra`, 30 W | 0-7 | 34-60 µs | 1,145 / 1,229 µs | 1 / 2.4 M | 116 / 2.4 M |
| PREEMPT_RT, 30 W + `jetson_clocks` | 0-7 | 31-55 µs | 1,191 / 1,160 µs | 3 / 2.4 M | 75 / 2.4 M |
| **PREEMPT_RT, 30 W, CPUs 4-7 isolated** | 4-7 | 34-37 µs | **133 / 226 µs** | 0 / 1.2 M | **0 / 1.2 M** |
| PREEMPT_RT, MAXN + `jetson_clocks`, 4-7 isolated, RT throttling off | 4-7 | 15-17 µs | **74 / 148 µs** | 0 / 1.2 M | 0 / 1.2 M |

Isolation = `isolcpus=managed_irq,domain,4-7 irqaffinity=0-3 kthread_cpus=0-3` on the RT
kernel's extlinux entry. 30 W = `MODE_30W_ALL` (CPUs at 1.19 GHz); MAXN runs them at 2.27 GHz
with EMC at 2,133 MHz. One run per row.

- **PREEMPT_RT alone does not remove the ~1.1-1.2 ms spikes.** Without isolation every CPU
  still hits 1.1-1.3 ms under load, on the RT kernel as on the stock one, with or without
  locked clocks (so not DVFS). The same size of spike also shows up idle, rarely (1-3 in
  2.4 M samples, with no `stress-ng` running).
- **Under load, isolating the RT CPUs removes them.** At the same power mode and clocks
  (30 W, no `jetson_clocks`), the isolated CPUs stay at or under 226 µs under load with no
  sample over 1 ms in 1.2 M, where the non-isolated runs' rate (75-177 in 2.4 M) would predict
  about 37-88. The isolated runs put a lighter load on the machine (`--cpu 4` on four CPUs
  against `--cpu 8` on eight), so part of the drop may be the load. Idle, the spike was too
  rare to tell (0 in 1.2 M against 1-3 in 2.4 M); a longer isolated idle run would settle it.
  MAXN with locked clocks halves the average and brings the worst to 148 µs under load and
  74 µs idle. The < 100 µs target is met idle at MAXN, not yet under this load.
- **What the trace shows** (`trace_break.sh`: `cyclictest -b 1000` stops ftrace at the first
  sample over 1 ms; sched, irq and IPI events only; under load). In the two saved captures
  (`results/trace_excerpt_{3,4}.txt`; two earlier ones, 1,132 µs and 1,129 µs, were overwritten
  before they were saved) the timer fired on time and `cyclictest` was switched in 9-15 µs
  after its wakeup; the time went after that, 1.16-1.23 ms with no traced event on that CPU.
  Both times it preempted the `stress-ng` I/O worker on that CPU, but the idle spikes show the
  stall does not need that task. Its cause is outside the events traced here; the capture
  threads ran at 1-4.5 ms periods (no `-d 0`), not all at 1 kHz.
- **A comparison point:** a [PREEMPT_RT report on Jetson AGX Orin](https://www.wimcorp.co.kr/tech/posts/embedded/preempt-rt-jetson-orin/)
  (NVIDIA's prebuilt RT kernel, MAXN + `jetson_clocks`) used the same isolation parameters and
  reports 113-159 µs worst under GPU, storage or EtherCAT load without isolation and 6-15 µs
  with it. The same step helps on both boards, but Orin shows no millisecond stall (47-56 µs
  under `stress-ng` without isolation), and Xavier's isolated worst at MAXN (148 µs) is about
  10× Orin's.

For an axon RT loop on Xavier: boot the RT kernel with the CPUs isolated, pin the loop to one
of them (`RtSetupConfig::cpu_id`), and keep camera, inference and storage work on the
housekeeping CPUs.

**Building and installing the RT kernel.** NVIDIA publishes prebuilt RT kernels only for r36
(Orin); for Xavier on r35, `build-rt.sh` cross-builds one from the L4T r35.6.4 public sources
with their bundled `rt-patches` (~10 min on a 9800X3D), and `install-rt.sh` adds it as an
extlinux entry. **Install it on the partition the firmware boots.** This board boots
`mmcblk0p1` (eMMC) and only mounts the NVMe as root (`root=/dev/nvme0n1p1`), so for three weeks
the RT entry sat in the NVMe `/boot/extlinux/extlinux.conf`, which nothing read: every boot ran
the stock kernel, and boots that did not come back on the network were blamed on the RT kernel.
They were a NIC-naming race (next paragraph). `install-rt.sh` now takes `BOOT_PART=` and
refuses when the running kernel is neither that partition's `/boot/Image` nor its `/boot/Image.rt`.
That guard cannot tell two partitions apart while both hold the same stock `Image` (a root fs
cloned from the boot partition), which is how this board started; `board_doctor.sh` breaks the
tie by which partition's DEFAULT entry boots the running kernel. Check with
`uname -v` against `strings /boot/Image | grep "Linux version"` on each candidate partition.
`apt upgrade` of `nvidia-l4t-kernel` has the same blind spot: it rewrote the NVMe `/boot`
(and reset `DEFAULT` to `primary` there) while the eMMC kernel stayed on the old release.

**Two NICs.** NetworkManager profiles bound by interface name (`eth0` = on-board LAN to a
second board, `eth1` = USB LAN to the network) swap when the USB adapter probes first, which
happened on some boots: the USB LAN then gets the other board's static address and the board
drops off the network. Bind the profiles to MAC addresses (`802-3-ethernet.mac-address`).

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
