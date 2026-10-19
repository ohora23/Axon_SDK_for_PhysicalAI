# Positioning & Competitive Analysis

*Where axon fits in the zero-copy landscape, what it does differently, and when to
reach for it instead of the alternatives.*

---

## TL;DR

axon does **not** invent a new mechanism. Sharing a buffer across processes and
accelerators without a CPU round-trip is built on well-established primitives —
Linux **dma-buf**, **CUDA VMM** export/import, `SCM_RIGHTS` FD passing, and the
`__cuda_array_interface__` / DLPack tensor conventions. Several production systems
already do zero-copy GPU data flow.

What axon offers is a **specific packaging** of those primitives that the mature
alternatives don't quite occupy:

- **Framework- and middleware-neutral.** Not bound to GStreamer, to ROS, or to a
  fixed operator graph. It is a small library that hands out a plain `device_ptr`
  / CUDA-array-interface, so the *same* transport feeds a PyTorch model, a CuPy
  kernel, and a hardware NVENC encoder — shown in `examples/vla_demo/` and
  `examples/nvenc_flywheel/`.
- **Real-time "latest-value-wins" semantics.** A seqlock metadata plane with a
  **measured, formula-bounded staleness** and no back-pressure — designed for a
  control loop that wants the *freshest* sample, not a lossless queue.
- **Thin and readable.** A core you can audit in an afternoon, not a multi-year
  framework.

Measured against the two ROS 2 transports that also share GPU memory between processes
(the official Lyrical `rosidl::Buffer` CUDA backend and ros2_cuda_ipc), axon delivers a GPU
frame to another process in **~25 µs**. The others take **~125 µs** in the GPU's default mode
and **35-56 µs with CUDA MPS**. Whether axon keeps that lead depends on the consumer and on MPS:

| consumer, at 1 MiB | without MPS | with MPS |
|---|---|---|
| copies the result out | axon 24 vs 124-126 µs (~5×) | axon 27 vs 41-56 µs (~1.5-2×; 1.3× at 4 MiB) |
| runs a kernel on the frame | axon **129** vs 122-124 µs (no lead) | axon **23** vs 44-49 µs (~2×; 1.6× at 4 MiB) |

Two costs make up the gap:

- **A descriptor over DDS, ~25-30 µs.** axon uses a shared-memory seqlock instead, so it always
  avoids this cost.
- **~60-100 µs for the GPU to switch from the producer's context to the consumer's.** MPS
  removes it, because the processes then share one context. A consumer that launches a kernel
  on the frame pays this switch with axon too (measured: 129 µs). axon's 25 µs ends at a
  24-byte copy back to the host, which does not pay it. So **for kernel consumers, axon's GPU
  lead needs MPS.**

axon's producer host-syncs before it publishes. Written naively, that halves throughput when
the producer also has CPU work. Pipelined, it costs no throughput in a transport-free probe,
only up to one CPU stage of latency. So it is a constraint on how the producer is written
([benchmarks/ros2_cpp](../benchmarks/ros2_cpp/README.md#gpu--gpu-against-the-other-gpu-zero-copy-transports)).

It is **not** the most mature, most complete, or most hardware-validated option.
It targets a niche between the heavyweight vendor frameworks and the raw kernel
primitives. Whether it is "the best" reduces to whether that niche matches your
problem.

---

## 1. The gap axon targets

Physical AI on a single robot needs three things at once along the
`sensor → accelerator → RT control` path: **high bandwidth**, **ultra-low
latency**, and **RT determinism**. Two families of tools each solve part of it:

- **General robotics middleware (ROS 2 + DDS)** copies each tensor several times
  per frame, so latency and CPU grow *with payload size* — exactly where robots
  move the most bytes (4K cameras, point clouds, embeddings). Staleness drifts
  with load, so it can't be used in safety analysis.
- **Message-level zero-copy IPC (Iceoryx2, eCAL)** gets a message to RAM with no
  copy, but **putting it onto an accelerator copies again**, and the RT-freshness
  guarantee is still missing.

axon closes both: keep the payload in a shared dma-buf / GPU allocation that every
process and accelerator references *in place*, and expose it to the RT loop through
a seqlock with a staleness that is measured per frame.

---

## 2. What axon does differently

1. **One transport, many consumers, no framework lock-in.** The producer publishes
   a descriptor; a consumer gets back a `device_ptr` or a `__cuda_array_interface__`
   object. That object is consumed *zero-copy* by whatever is downstream —
   `torch.as_tensor(...)`, CuPy, Numba, or a hardware encoder. The demos prove the
   same buffer flows into a GPT-2 prefill and into NVENC without changing the
   transport.

2. **Latest-value-wins, not a queue.** The default delivery model overwrites the
   oldest slot and never blocks the producer (see
   [ADR 0004](adr/0004-latest-value-rt-consumer.md) and the sizing note in
   [usage.md](usage.md)). For a control loop a stale sample is worse than a skipped
   one; most alternatives optimise for lossless throughput instead.

3. **Staleness you can put in a safety case.** The age of every frame the RT loop
   reads is measured and bounded by an explicit 7-term formula, not left implicit.

4. **Portable-by-design primitives.** dma-buf for the host path, CUDA VMM for the
   GPU path, with the backend abstracted so non-NVIDIA accelerators can be added.
   (Today the GPU path is CUDA-only — the portability is a design property, not yet
   a shipped feature.)

---

## 3. The landscape

### 3.1 NVIDIA frameworks — the closest production analogs

These already do zero-copy GPU data flow, and do it maturely. They are the real
competition for the vision/recording use cases.

- **Isaac ROS / NITROS** — passes GPU buffers between ROS 2 nodes on one host with
  no copy via type adaptation and negotiated NVMM/CUDA buffers. The most direct
  overlap with axon's planned ROS 2 wrapper. Coupled to ROS 2 and NVIDIA.
- **DeepStream** — camera decode (NVDEC) → inference (TensorRT) → encode (NVENC),
  all zero-copy on the GPU. Essentially a mature version of the
  `nvenc_flywheel` demo. A heavyweight GStreamer framework, NVIDIA-only.
- **Holoscan** — low-latency sensor→GPU processing SDK; closest in *spirit* to
  axon's goals, but organised as an operator graph and NVIDIA-centric.
- **NvSci (NvSciBuf / NvSciSync)** — the Jetson/DRIVE buffer + fence interop layer:
  allocate once, share across camera/GPU/encoder/DLA with explicit sync. The
  production-grade version of axon's R6 (GPU pool) + R2 (sync-fence).

### 3.2 IPC middleware — the transport layer

- **Iceoryx2 / eCAL** — true host shared-memory zero-copy pub/sub. axon *uses*
  Iceoryx2 as an optional metadata backend. They don't natively carry GPU buffers,
  and they are queue/throughput oriented. **Measured on the host** (iceoryx2 0.10, C++): within
  about 1 µs of axon from 64 B to 4 MiB, and faster from 16 MiB, where axon's time grows with its
  ring depth (16 MiB: 238 vs 474 µs; the likely cause, cache pressure, was not measured). On the
  host, axon has no latency edge over it.
- **DDS + shared memory (Cyclone, Fast DDS)** — zero-copy on host via loaned
  samples; the backbone under ROS 2. Cross-node capable, GPU-agnostic.

### 3.3 ROS 2 GPU buffer transports — the newest and closest analogs

- **`rosidl::Buffer` + `cuda_buffer_backend` (ROS 2 Lyrical, official).** Message fields such
  as `sensor_msgs/Image.data` can live in a pluggable storage backend. The CUDA backend
  allocates them in CUDA VMM, passes the FDs over a Unix socket, and synchronizes with CUDA
  IPC write and read events. It falls back to the CPU path automatically. A ROS 2
  maintainer described the `rcl::Buffer` work as "heavily inspired by Nitros" but generic
  ([ROS Discourse](https://discourse.openrobotics.org/t/ros2-cuda-ipc-zero-copy-gpu-data-sharing-between-ros-2-processes/57157)).
  It is the ROS-native answer, and for an all-ROS-2 system on Lyrical it is the default
  choice.
- **ros2_cuda_ipc** ([dskkato/ros2_cuda_ipc](https://github.com/dskkato/ros2_cuda_ipc), MIT).
  The same design as a standalone library for Humble/Jazzy, with explicit descriptors,
  buffer references held by each reader's `ReadHandle`, and a Python/DLPack subscriber.
- **Measured** (GPU → GPU, 147 KiB-4 MiB, p50): both are ~123-127 µs and flat in size, and axon
  is ~25 µs. With CUDA MPS they drop to 35-55 µs and axon stays at ~25 µs. Both send a DDS
  descriptor (~25-30 µs), which axon does not. Both also wait on a consumer-side CUDA IPC event,
  which costs ~60 µs without MPS. That ~60 µs is most likely the switch between the processes'
  GPU contexts, not the event itself: a kernel-launching axon consumer pays it too (129 µs
  without MPS, 23 µs with it). They also give things axon does
  not: an asynchronous producer, and buffers kept alive while a reader holds them instead
  of latest-value-wins. Both also carry typed ROS messages. `rosidl::Buffer` adds an
  automatic CPU fallback path; ros2_cuda_ipc needs explicit bridge nodes for CPU consumers.

### 3.4 Primitives — what everything is built on

- **dma-buf / PRIME** — the kernel mechanism for sharing buffers across
  drivers/processes (V4L2 → GPU → encoder). axon's host path is exactly this.
- **DLPack / `__cuda_array_interface__`** — the cross-framework zero-copy tensor
  conventions. axon exposes CAI; these are *conventions*, not transports.
- **Triton + CUDA shared memory** — lets serving clients pass GPU memory in without
  a host copy. Same idea, different (inference-serving) use case.

### 3.5 Comparison

| Solution | Zero-copy GPU across processes | Coupling | Cross-node | Delivery model | Maturity |
|---|---|---|---|---|---|
| **axon** | Yes (CUDA VMM), **~25 µs GPU→GPU** (copy consumer; a kernel consumer needs MPS for that, ~129 µs without) | **None** (plain `device_ptr` / CAI) | No (single host) | **Latest-value-wins, bounded staleness** | Pre-alpha, dev-PC verified |
| ROS 2 `rosidl::Buffer` CUDA backend | Yes (CUDA VMM + IPC events), ~124 µs GPU→GPU (~42-56 µs with MPS) | ROS 2 Lyrical+ | No | ROS pub/sub, CPU fallback | New (Lyrical, 0.1.x) |
| ros2_cuda_ipc | Yes (CUDA VMM + IPC events), ~125 µs GPU→GPU (~37-44 µs with MPS) | ROS 2 (Humble+) | No | ROS pub/sub, reader-held buffers | Experimental |
| Isaac ROS / NITROS | Yes | ROS 2 + NVIDIA | No | ROS pub/sub | Production |
| DeepStream | Yes | GStreamer + NVIDIA | Via streaming | Pipeline / queue | Production |
| Holoscan | Yes | Holoscan graph + NVIDIA | Yes (RDMA) | Operator graph | Production |
| NvSci | Yes | NVIDIA (Jetson/DRIVE) | No | Explicit fences | Production |
| Iceoryx2 / eCAL | No (host SHM only) | None | No | Queue | Production |
| DDS + SHM | No (host SHM only) | DDS/ROS | Yes | Queue / history | Production |

---

## 4. Honest limitations

- **Maturity and breadth.** DeepStream, Isaac ROS, Holoscan, and NvSci are
  multi-year, vendor-supported, and carry far more (batching, multi-GPU,
  GPUDirect/RDMA, formal sync). axon is a pre-alpha library verified on one dev PC.
- **Single host only.** No cross-node transport; the alternatives that span nodes
  (DDS, DeepStream over RTSP, Holoscan + RDMA) do something axon does not.
- **GPU path is CUDA-only today.** Vendor neutrality is a design goal, not a
  shipped backend.
- **The mechanism is not novel.** axon's value is packaging and semantics, not a
  new primitive.
- **The producer blocks on its GPU work.** axon's low GPU latency comes from host-synchronizing
  before publish; the event-based ROS 2 transports let the producer queue more GPU work
  instead. A producer that must not block wants their model.

---

## 5. When to choose axon

Reach for axon when **all** of these hold:

- **Single host.** One robot / one box, multiple processes.
- **Framework or vendor neutrality matters.** You want the same transport to feed
  PyTorch, CuPy, and a hardware encoder, and you don't want to marry GStreamer or
  ROS to get zero-copy onto the accelerator.
- **The consumer is a control loop.** You want the freshest sample with a measured
  staleness bound, not a lossless backlog.
- **You value a small, auditable core** over a large framework.

Prefer a mature alternative when:

- You are already all-in on **ROS 2** and want GPU zero-copy between nodes → the
  official **`rosidl::Buffer` CUDA backend** on Lyrical or later (ros2_cuda_ipc on older
  distros), or **Isaac ROS / NITROS**. Reach for axon there only if the RT latest-value
  semantics or the per-frame GPU latency matter to you. With MPS that gap is ~20-25 µs per
    frame for kernel consumers and ~10-30 µs for copying ones. Without MPS, it is ~100 µs only
  for consumers that copy results out, and none for
  consumers that run kernels.
- You want a batteries-included **vision pipeline** (decode → infer → encode) →
  **DeepStream** (or **Holoscan** for low-latency sensor fusion).
- You are on **Jetson/DRIVE** and want formally-verified buffer/sync interop →
  **NvSci**.
- You need **cross-node** transport → **DDS** / a streaming stack.

axon's bet is that the intersection above — single-host, framework-neutral,
RT-latest-value, thin — is a real and under-served spot between the heavyweight
vendor frameworks and the raw primitives.
