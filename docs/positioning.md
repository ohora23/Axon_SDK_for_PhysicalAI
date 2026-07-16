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
  and they are queue/throughput oriented.
- **DDS + shared memory (Cyclone, Fast DDS)** — zero-copy on host via loaned
  samples; the backbone under ROS 2. Cross-node capable, GPU-agnostic.

### 3.3 Primitives — what everything is built on

- **dma-buf / PRIME** — the kernel mechanism for sharing buffers across
  drivers/processes (V4L2 → GPU → encoder). axon's host path is exactly this.
- **DLPack / `__cuda_array_interface__`** — the cross-framework zero-copy tensor
  conventions. axon exposes CAI; these are *conventions*, not transports.
- **Triton + CUDA shared memory** — lets serving clients pass GPU memory in without
  a host copy. Same idea, different (inference-serving) use case.

### 3.4 Comparison

| Solution | Zero-copy GPU across processes | Coupling | Cross-node | Delivery model | Maturity |
|---|---|---|---|---|---|
| **axon** | Yes (CUDA VMM) | **None** (plain `device_ptr` / CAI) | No (single host) | **Latest-value-wins, bounded staleness** | Pre-alpha, dev-PC verified |
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

- You are already all-in on **ROS 2** and want GPU zero-copy between nodes →
  **Isaac ROS / NITROS**.
- You want a batteries-included **vision pipeline** (decode → infer → encode) →
  **DeepStream** (or **Holoscan** for low-latency sensor fusion).
- You are on **Jetson/DRIVE** and want formally-verified buffer/sync interop →
  **NvSci**.
- You need **cross-node** transport → **DDS** / a streaming stack.

axon's bet is that the intersection above — single-host, framework-neutral,
RT-latest-value, thin — is a real and under-served spot between the heavyweight
vendor frameworks and the raw primitives.
