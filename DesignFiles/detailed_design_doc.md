# Data-Centric Physical AI Robot Architecture — Detailed Design

> **References**: this document expands the mechanisms agreed upon in `data-centric-zero-copy-design-20260510.md` (Status: APPROVED v2, 2026-05-10), which fixes the direction, scope, differentiation, and risks/mitigations. On policy conflicts, the design doc wins.
>
> **Role of this document**: a detailed mechanism specification for the **data-centric robot software architecture** that overcomes the latency caused by ROS2 DDS serialization/deserialization and memory bouncing, while meeting the high-bandwidth / ultra-low-latency / RT-determinism demands of Physical AI.

---

## 0. Overview

### 0.1 Core Thesis (5 Planes)

| Plane | Mechanism | Output |
|---|---|---|
| Metadata | Iceoryx2 SHM queue carrying `TensorDescriptor` (fixed-size POD) | seqno · shape · offset metadata |
| FD | `SCM_RIGHTS` / `pidfd_getfd(2)` sidecar | dma-buf FDs, sync_file FDs |
| Memory | V4L2 → dma-buf export → accelerator import | no host copy |
| Time | Inference = non-RT worker, RT loop = seqlock zero-copy read | bounded staleness |
| Sync | `dma_resv` implicit / sync_file explicit | producer-only attach policy |

### 0.2 Data-Flow Diagram

```
[Sensor (camera/LiDAR)]
        │ V4L2 capture
        ▼
   [dma-buf FD] ────┐
        │           │ (1) SCM_RIGHTS / pidfd_getfd: deliver the FD once
        │ export    │      (at the subscribe handshake)
        ▼           ▼
[NPU/GPU driver import]
        │
        │ inference (non-RT worker, best-effort)
        ▼
[Inference output dma-buf]
        │ (2) Iceoryx2 SHM publish: TensorDescriptor metadata (≤ 144B)
        ▼
   ┌─────────────────────────────────────┐
   │ Iceoryx2 lock-free SHM queue        │
   └──────────────┬──────────────────────┘
                  │ (3) RT consumer: seqlock read of latest descriptor
                  ▼
   ┌──────────────────────────────────────┐
   │ RT consumer (mlockall + MAP_POPULATE)│
   │  - dma-buf view (zero-copy)          │
   │  - staleness measurement             │
   │  - fallback policy                   │
   └──────────────┬───────────────────────┘
                  │
                  ▼
        [1kHz RT control loop → motor command]
```

### 0.3 First-Build Scope

- **Single host, multi-process** (producer process + RT consumer process).
- **Single accelerator platform** — locked in during the **week 1-2 spike PoC**, between AMD XDNA and NVIDIA Jetson Orin.
- Apple Silicon is out of scope for the first build.
- Discrete NUMA / GPUDirect is a follow-up design.

---

## 1. UMA Architecture (Unified Memory)

An environment where CPU · GPU · NPU physically share the same system RAM. No PCIe P2P bottleneck; **cache coherency and dma-buf-based FD passing** are central.

### 1.1 Metadata Plane — Iceoryx2

#### 1.1.1 Role

- A lock-free SHM queue carrying **metadata only**. The payload (the actual tensor) is kept in a dma-buf outside the SHM queue.
- The publisher writes a `TensorDescriptor` directly into a SHM slot at publish time; the subscriber reads it from a lock-free memory-mapped queue.
- O(1) metadata transfer regardless of payload size.

#### 1.1.2 `TensorDescriptor` Definition (Iceoryx2 payload)

A fixed-size POD. Iceoryx2 has partial dynamic-payload support, but for determinism a fixed layout with max-rank 8 is enforced.

```c
// total: ~144 bytes (8-byte aligned)
typedef struct __attribute__((aligned(8))) {
    uint64_t bo_handle;              // Receiver-side identifier of the FD that
                                     // arrived via the sidecar; producer-allocated.
    uint64_t seqno;                  // Monotonically increasing version
                                     // (seqlock + ABA prevention)
    uint64_t pool_generation;        // Buffer-pool reallocation indicator
                                     // (triggers FD re-delivery)
    uint64_t capture_ts_ns;          // V4L2 hardware timestamp where supported,
                                     // otherwise CLOCK_MONOTONIC_RAW
    uint64_t producer_publish_ts_ns; // CLOCK_MONOTONIC_RAW measured immediately
                                     // before publish
    uint32_t shape[8];               // max rank 8
    uint64_t offset;                 // Tensor offset inside the dma-buf
    uint64_t size;                   // Tensor size in bytes
    uint64_t sync_fence_token;       // When sync_fence_kind ==
                                     // SyncFileViaSidecar: the producer-side id
                                     // of the sync_file FD delivered via the
                                     // sidecar
    uint8_t  rank;                   // Actual tensor rank
    uint8_t  dtype;                  // enum: U8/U16/F16/BF16/F32/F64 ...
    uint8_t  sync_fence_kind;        // enum: NONE / DMA_RESV_IMPLICIT /
                                     //       SYNC_FILE_VIA_SIDECAR
    uint8_t  reserved[5];
} TensorDescriptor;
```

Roughly 144 bytes — maps directly onto an Iceoryx2 fixed-size publisher.

#### 1.1.3 Publisher / Subscriber Wrappers

A thin wrapper sits on top of Iceoryx2's native publisher/subscriber. The user-facing API:

```cpp
// Publisher side
auto publisher = TensorPublisher::create(service_name);
publisher.handshake_pool(pool_fds);  // bulk SCM_RIGHTS delivery (see 1.3)
auto desc = publisher.acquire_descriptor();
fill_descriptor(desc, /* shape, dtype, ... */);
publisher.publish(desc);             // Iceoryx2 publish

// Subscriber side
auto subscriber = TensorSubscriber::create(service_name);
subscriber.attach_pool(received_fds);  // attach FDs received via the sidecar
auto view = subscriber.latest_view();  // seqlock-based zero-copy view
// view.data() = mmap'd region of the dma-buf (or imported accelerator handle)
```

### 1.2 Memory Plane — Linux DMA-BUF + Accelerator Import

#### 1.2.1 Zero-Copy Implementation

Through the Linux `dma-buf` subsystem, buffers are shared across hardware drivers **as file descriptors (FDs) without physical copies**.

#### 1.2.2 How it works (UMA on AMD XDNA, example)

1. **Capture**: V4L2 captures a camera frame and exports a dma-buf FD via `VIDIOC_EXPBUF`.
2. **Sidecar FD delivery**: the producer process delivers the FD to the consumer and accelerator-worker processes via SCM_RIGHTS at subscribe time (§1.3).
3. **NPU import**: the AMD XDNA driver imports the FD via `amd_xdna_import_dma_buf`, registering it in the device page table.
4. **Inference** (non-RT worker): the model runs. The input is the imported dma-buf; the output is written directly into another dma-buf.
5. **Metadata publish**: a `TensorDescriptor` is published through Iceoryx2, with the output `bo_handle` and an updated `seqno`.
6. **RT consumer read**: the RT loop reads the latest descriptor via seqlock, takes the zero-copy view of the attached dma-buf, and consumes it inside its 1kHz control loop.

#### 1.2.3 Code Example (UMA accelerator mapping)

```c
// [Pseudocode] UMA accelerator memory mapping (producer side)
int map_tensor_uma_producer(
    int v4l2_fd,
    size_t size,
    int *dma_buf_fd_out,
    void **cpu_ptr_out,
    void **npu_ptr_out)
{
    // 1. Export the V4L2 capture buffer as a dma-buf
    struct v4l2_exportbuffer expbuf = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .index = 0,
        .flags = O_RDWR,
    };
    if (ioctl(v4l2_fd, VIDIOC_EXPBUF, &expbuf) < 0) return -1;
    *dma_buf_fd_out = expbuf.fd;

    // 2. Map it into CPU user space (cache coherency must be maintained)
    *cpu_ptr_out = mmap(NULL, size, PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_POPULATE,
                        expbuf.fd, 0);
    if (*cpu_ptr_out == MAP_FAILED) return -1;

    // 3. Import into NPU device memory (AMD XDNA example)
    *npu_ptr_out = amd_xdna_import_dma_buf(device_handle, expbuf.fd, size);
    if (!*npu_ptr_out) return -1;

    return 0;
}

// [Pseudocode] CPU write → NPU read synchronization
int sync_cpu_to_npu(int dma_buf_fd) {
    struct dma_buf_sync sync_start = { .flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW };
    struct dma_buf_sync sync_end   = { .flags = DMA_BUF_SYNC_END   | DMA_BUF_SYNC_RW };

    // Notify the start of CPU cache flushing
    if (ioctl(dma_buf_fd, DMA_BUF_IOCTL_SYNC, &sync_start) < 0) return -1;
    // ... CPU-side write ...
    if (ioctl(dma_buf_fd, DMA_BUF_IOCTL_SYNC, &sync_end) < 0) return -1;
    // From here it is safe for the NPU side to read
    return 0;
}
```

#### 1.2.4 Synchronization — `dma_resv`

- `dma_resv` orders CPU cache flushes and NPU work start.
- Two modes:
  - **Implicit**: the kernel manages it automatically (`sync_fence_kind = DMA_RESV_IMPLICIT`). Simple, but waits can be non-deterministic.
  - **Explicit**: the producer creates a `sync_file` FD and delivers it via the sidecar (`sync_fence_kind = SYNC_FILE_VIA_SIDECAR`); the consumer polls/waits explicitly. Preferred in the RT environment.

### 1.3 FD Sidecar — `SCM_RIGHTS` / `pidfd_getfd(2)`

#### 1.3.1 Why it's needed

Iceoryx2 SHM queues **share memory only**, not the FD table. Writing an integer FD into a SHM slot means nothing in another process — FD tables are per-process.

→ **The dma-buf FD itself must travel through a separate channel.**

#### 1.3.2 Channel options

| Channel | Pros | Cons |
|---|---|---|
| `SCM_RIGHTS` (Unix domain socket) | Standard, available on every Linux | Requires a socket connection beforehand |
| `pidfd_getfd(2)` (Linux 5.6+) | No socket needed, just a pidfd | Requires the `PTRACE_MODE_ATTACH_REALCREDS` permission; relatively new |
| `memfd + F_ADD_SEALS` + name sharing | Path-shared workaround | Does not apply to dma-buf FDs (memfd-only) |

→ **Default to `SCM_RIGHTS`, with `pidfd_getfd` as a fallback.**

#### 1.3.3 Handshake Sequence

```
Producer                         Consumer
   │                                 │
   │  ─── (1) Iceoryx2 service ──>   │
   │       discovery                 │
   │                                 │
   │  <── (2) sidecar connect ───    │
   │       (Unix socket)             │
   │                                 │
   │  ─── (3) bulk-deliver ──>       │
   │       buffer pool (SCM_RIGHTS:  │
   │        FDs[N], pool_generation) │
   │                                 │ Consumer registers FDs in its
   │                                 │   cache, accelerator-imports each
   │                                 │   (or mmaps it)
   │                                 │
   │  ─── (4) Iceoryx2 publish ──>   │
   │       (TensorDescriptor:        │
   │        bo_handle, seqno,        │
   │        pool_generation)         │
   │                                 │ Consumer:
   │                                 │   if (cached_pool_gen == desc.pool_gen)
   │                                 │     → use cached handle
   │                                 │   else
   │                                 │     → stale, re-request via sidecar
```

#### 1.3.4 Pool Lifecycle

- The producer allocates a buffer pool once (e.g. 32 buffers) and delivers every FD in a single `SCM_RIGHTS` call.
- Subsequent publishes carry only metadata → sidecar calls are amortized.
- On reallocation `pool_generation` increments and the fresh FDs are delivered again.
- A `pool_generation` mismatch causes the consumer to re-request through the sidecar (publishes during this gap are treated as stale).

#### 1.3.5 RT Consumer Must Not Call close(2)

- `close(2)` is a syscall and has non-deterministic latency inside the RT loop.
- The non-RT side (subscribe worker) owns the FD lifecycle. The RT consumer reads zero-copy views of attached handles only.
- On pool retire the non-RT worker synchronizes with the RT loop before calling close.

### 1.4 Synchronization Policy Summary

| Situation | Mechanism |
|---|---|
| Sensor → NPU (right after capture) | `dma_resv` implicit (V4L2's capture-complete fence is already attached) |
| NPU → RT consumer (output) | `sync_file` explicit, fence FD via sidecar, RT polls in fast path |
| Pool reallocation | `pool_generation` bump + sidecar re-handshake to every consumer |
| FD-leak prevention | non-RT lifetime owner refcounts; RT must never call close |

---

## 2. Discrete NUMA Architecture (follow-up design)

> **Out of scope for the first build.** This section only sketches the direction.

### 2.1 Differences from UMA

- CPU and GPU/NPU have **physically separate memory pools** (e.g. x86_64 + an NVIDIA discrete GPU).
- Cross-device data movement requires **PCIe DMA** — UMA's "just pass dma-buf FDs" alone does not reach zero-copy.

### 2.2 Candidate mechanisms

- **GPUDirect RDMA**: direct DMA between NVIDIA GPUs, or NIC↔GPU. Requires peer-to-peer mapping.
- **PCIe P2P**: BAR-based direct DMA. Depends on BIOS settings (e.g. ACS disable).
- **Unified Memory** (CUDA UM): page-level automatic migration. Not zero-copy, but simpler. Determinism is weak.
- **NVLink / Infinity Fabric**: high-bandwidth interconnects allowing zero-copy across cards.

### 2.3 Relationship to the first build

- The `TensorDescriptor` abstraction from the UMA first build carries forward to the NUMA backend (let `bo_handle` point to a GPU memory handle).
- NUMA-specific synchronization may use a different fence mechanism (CUDA event, etc.). Add to `sync_fence_kind` later.

---

## 3. RT-Linux Scheduling Plane

### 3.1 RT / non-RT Region Separation

| Region | Work | Priority |
|---|---|---|
| RT | 1kHz control loop, seqlock read, dma-buf view access | `SCHED_FIFO`, prio 80-95 |
| non-RT | Inference worker, sidecar communication, V4L2 capture handling | `SCHED_OTHER`, or low RT prio |

**Core principle**: inference is not RT. Inference latency has a long tail through P99.99 and is affected by thermal throttling — it cannot be bounded deterministically. The RT loop guarantees only the **staleness bound of the inference result**.

### 3.2 Memory Pinning (page-fault prevention)

```c
// Call once when the RT process starts.
int rt_setup_memory(void) {
    // Lock all current/future pages, force fault-on-first-access
    if (mlockall(MCL_CURRENT | MCL_FUTURE | MCL_ONFAULT) < 0)
        return -1;
    // Pre-fault stack
    char stack_prefault[8192];
    memset(stack_prefault, 0, sizeof(stack_prefault));
    // Pre-fault heap (example)
    void *heap = malloc(1 * 1024 * 1024);
    memset(heap, 0, 1 * 1024 * 1024);
    return 0;
}
```

Force a fault at dma-buf attach time too — either mmap with `MAP_POPULATE` or read-touch the entire region right after attach.

### 3.3 RT Consumer Pattern (seqlock)

```c
// [Pseudocode] RT seqlock consumer
int rt_read_latest_view(
    SharedSlot *slot,
    int max_retry,
    TensorView *view_out,
    StaleStats *stats_out)
{
    for (int retry = 0; retry < max_retry; retry++) {
        uint64_t seq_before = atomic_load_acquire(&slot->seqno);
        if (seq_before & 1) continue;  // writer in progress (odd = locked)

        // Read body
        TensorDescriptor desc = slot->desc;  // copy a fixed-size POD
        // The dma-buf view comes from the pre-attached handle (cache hit guaranteed)
        view_out->data = lookup_attached_handle(desc.bo_handle, desc.pool_generation);
        view_out->shape = desc.shape;
        // ...

        uint64_t seq_after = atomic_load_acquire(&slot->seqno);
        if (seq_before == seq_after) {
            stats_out->retries = retry;
            stats_out->staleness_ns = monotonic_now() - desc.producer_publish_ts_ns;
            return 0;
        }
        // seqno changed → writer interleaved → retry
    }
    return -EAGAIN;  // fallback policy will be applied
}
```

### 3.4 Priority-Inversion Avoidance

Risk: while a non-RT inference worker holds dma-buf attach/detach (`dma_resv` lock) and the RT consumer touches the same object, there can be non-deterministic waiting.

**Mitigation**:
- **Only the producer / non-RT worker performs dma-buf attach/detach.** The RT consumer only receives an attached handle and reads a view.
- The RT side never holds the `dma_resv` lock directly.
- If sync_file polling is required, do it in **non-blocking** mode with a bounded retry (fall back on timeout).

### 3.5 Stale-Fallback Policy

When the RT loop exceeds the seqlock retry cap or a fence-wait times out:

| Policy | Meaning | Use case |
|---|---|---|
| `last_known_good` | Reuse the last valid view | Visual servo, locomotion |
| `zero_command` | Safe-stop command | When safety bounds are hit |
| `user_callback` | Call a user-provided policy | Domain-specific |

The user picks via `TensorSubscriber::set_fallback_policy(FallbackPolicy)`.

---

## 4. Data Lifecycle

### 4.1 Buffer Pool

- The producer pre-allocates `N` buffers at startup (V4L2 `VIDIOC_REQBUFS`, or a separate output pool for the accelerator).
- The pool is a ring. The producer calls `acquire_next()` to take a free buffer → captures/inferences → publishes → consumer consumes → ring advances.
- Pool size ≥ `inference_worst_case_ms × producer_rate_hz × safety_factor` (so the producer doesn't block when the RT loop falls behind).

### 4.2 FD Lifecycle State Machine

```
   ALLOCATED ──(SCM_RIGHTS deliver)──> SHARED
       │                                  │
       │ pool retire                      │ consumer attach
       ▼                                  ▼
    RETIRING <──(refcount=0)──── ATTACHED_BY_N
       │
       ▼
    CLOSED
```

- When the producer decides to retire, `pool_generation` increments and every consumer is notified.
- After every consumer detaches and acks, the producer closes.

### 4.3 V4L2 ↔ NPU ↔ RT End-to-End Sequence (UMA, normal path)

```
T+0ms    : V4L2 capture-complete (hardware timestamp)
T+0.1ms  : producer obtains the dma-buf FD (already shared via SC at boot)
T+0.2ms  : NPU import handle lookup (cache hit)
T+0.3ms  : NPU inference starts (non-RT)
T+10ms   : NPU inference completes, sync_file FD created
T+10.1ms : sync_file FD delivered via the sidecar
T+10.2ms : producer publishes (TensorDescriptor) on Iceoryx2
T+10.3ms : RT consumer seqlock-reads on the next 1kHz tick
T+10.4ms : RT consumer polls the sync_file (non-blocking) → ready
T+10.5ms : RT consumer accesses the dma-buf view, reads the inference result
T+10.6ms : 1kHz control command produced
```

**Worst-case staleness**: 11ms (capture → 1kHz consumer use). Detailed formula in §5.

---

## 5. Bounded Staleness Formula

### 5.1 Definition

```
staleness_worst_case = 
    sensor_capture_to_dma_buf_export_max          (T_cap)
  + producer_dma_buf_fence_wait_max               (T_fence_p)
  + inference_worst_case                          (T_inf)
  + descriptor_publish_max                        (T_pub)
  + sidecar_fd_handshake_amortized                (T_sc, ≈0 in steady state)
  + rt_consumer_seqlock_max_retry_cost            (T_rt_seq)
  + rt_consumer_dma_buf_view_access_max           (T_view, ≈0 with mlock)
```

### 5.2 How each term is measured

| Term | Tool | Normal range (assuming Jetson Orin) |
|---|---|---|
| `T_cap` | V4L2 hw ts → app-receive monotonic ts | 100–500 µs |
| `T_fence_p` | trace `DMA_BUF_IOCTL_SYNC` enter/exit (eBPF) | 10–100 µs |
| `T_inf` | per-model 10k stress + thermal soak | 5–15 ms (model-dependent) |
| `T_pub` | Iceoryx2's own publish metric | 1–10 µs |
| `T_sc` | only on the first handshake or on `pool_generation` mismatch | 0 (steady) / 1–5 ms (re-handshake) |
| `T_rt_seq` | custom atomic counter + retry distribution | 0 to a few hundred ns (steady) |
| `T_view` | with `MAP_POPULATE` + mlockall, traced via perf | ≈0 by guarantee |

### 5.3 Use in System-Safety Analysis

- The control-loop designer gets an explicit guarantee: "the inference result is fresh within N ms".
- Directly usable in visual-servo gain design, safe-stop distance computation, and similar.
- Comparison: ROS2 topics have a non-deterministic staleness distribution that depends on queue depth and processing load — not directly usable for safety analysis.

### 5.4 Measurement Infrastructure

- `cyclictest`: 24-hour 1kHz loop jitter measurement.
- `eBPF`: verify zero `copy_to_user` / `copy_from_user` calls (zero-copy verification).
- `bpftrace`: trace `dma_buf_ioctl` enter/exit times.
- `perf record`: verify zero page faults (during the RT loop).
- Custom atomic counters: seqlock retry distribution, sidecar re-handshake frequency.

---

## 6. Platform Backends

### 6.1 Candidate 1 — AMD AI Series (XDNA NPU)

**Status**: whether external dma-buf import has a production-grade path in the mainline driver is the key spike-PoC question for week 1-2.

| Component | API |
|---|---|
| Capture | V4L2 + `VIDIOC_EXPBUF` |
| Import | `amd_xdna_import_dma_buf` (pseudocode; the actual API is confirmed during the spike) |
| Sync | `dma_resv` implicit + `sync_file` explicit |
| Inference | XRT (Xilinx Runtime) or ROCm via XDNA |

**Risk**: XDNA is BO-based. Whether external dma-buf import exists in mainline — or requires a custom patch — is the spike's central question.

### 6.2 Candidate 2 — NVIDIA Jetson Orin

**Status**: production path is well validated. NVIDIA-specific.

| Component | API |
|---|---|
| Capture | V4L2 + `nvbufsurface` (NVMM) |
| Import | `cuMemImportFromShareableHandle` (CUDA) or `EGLStream` |
| Sync | CUDA event + `cuStreamWaitValue` |
| Inference | TensorRT or cuDNN |

**Pros**: GPUDirect RDMA, NVMM, EGLStream are all production-ready. Demo videos can be produced quickly.

**Cons**: NVIDIA ecosystem lock-in — weakens the "open source" differentiator. Position the README as "first build = one backend, more contributions welcome".

### 6.3 (Excluded) Apple Silicon

- macOS lacks V4L2.
- Asahi Linux lacks an ANE driver (as of 2026-05).
- Out of scope for the first build. Revisit when Asahi matures.

### 6.4 Week 1-2 Spike PoC Items

```
[ ] Acquire AMD AI Series (XDNA) board or Jetson Orin board
[ ] V4L2 camera capture → VIDIOC_EXPBUF smoke test
[ ] Deliver the dma-buf FD to another process via SCM_RIGHTS
[ ] Try external dma-buf import on the candidate accelerator driver
[ ] Verify tensor content via CPU mmap (eBPF check for zero copies)
[ ] 1kHz cyclictest baseline measurement
[ ] PoC succeeds → finalize the first target + enter formal design
[ ] PoC fails → switch candidate or revisit the design doc
```

---

## 7. API Surface

### 7.1 C++ Core API

```cpp
namespace dczc {  // data-centric zero-copy

class TensorPool {
public:
    static TensorPool create(size_t n_buffers, size_t buffer_size,
                             PoolBackend backend);
    int dma_buf_fd(size_t index) const;
    uint64_t generation() const;
    // ...
};

class TensorPublisher {
public:
    static TensorPublisher create(std::string_view service_name,
                                  TensorPool& pool);
    void handshake_pool();  // bulk SCM_RIGHTS delivery
    AcquiredDescriptor acquire_descriptor();
    void publish(AcquiredDescriptor&& desc);
};

class TensorSubscriber {
public:
    static TensorSubscriber create(std::string_view service_name);
    void wait_handshake();  // receive FDs via the sidecar
    std::optional<TensorView> latest_view(int max_retry = 8);
    void set_fallback_policy(FallbackPolicy policy);
};

struct TensorView {
    void* data;          // dma-buf host-mmap region (UMA)
    AcceleratorHandle accel_handle;  // imported handle
    Shape shape;
    DType dtype;
    uint64_t staleness_ns;
    uint64_t seqno;
};

}  // namespace dczc
```

### 7.2 Python Bindings (pybind11, Phase 1)

```python
import dczc

pool = dczc.TensorPool.create(n_buffers=32, buffer_size=4 * 1024 * 1024,
                              backend=dczc.PoolBackend.V4L2)
pub = dczc.TensorPublisher.create("camera/inference_out", pool)
pub.handshake_pool()

while True:
    desc = pub.acquire_descriptor()
    # ... fill desc.shape, run inference into the pool buffer ...
    pub.publish(desc)
```

### 7.3 Rust Crate (Phase 2)

A natural follow-up since Iceoryx2 itself is Rust. Lands after the Phase 1 release.

---

## 8. Measurement Infrastructure

### 8.1 Benchmark Items

| Metric | Target | Measurement |
|---|---|---|
| Sensor → command P99 latency | ≤ T_inf + 5 ms | end-to-end timestamp |
| 1kHz RT worst-case jitter | < 100 µs | cyclictest 24h |
| Inter-device memory copies | = 0 | eBPF traces of `copy_to_user`/`copy_from_user` |
| Page faults (during RT) | = 0 | perf record |
| Seqlock retry P99 | ≤ 1 | custom atomic counter |
| Staleness-bound formula | measured ≤ formula sum | each term measured separately |

### 8.2 ROS2 Baseline Comparison

On identical hardware/model/resolution/run length:
- ROS2 + Cyclone DDS (default)
- ROS2 + `rmw_iceoryx_cpp` (zero-copy messages)
- This architecture

Compared metrics: the six above. Results published in the README as graphs alongside the reproducibility spec.

### 8.3 Thermal Stress

- Validate the staleness formula under heat-gun-induced NPU/GPU throttle.
- Demonstrate that the formula's `inference_worst_case` term includes a thermal margin.

---

## 9. Risks and Mitigations Summary (integrating the design doc v2 §Reviewer-flagged Risks)

| ID | Risk | Mitigation location |
|---|---|---|
| R1 | Iceoryx2 SHM is not an FD-transport channel | §1.3 sidecar |
| R2 | AMD XDNA external dma-buf import path unclear | §6.4 spike PoC |
| R3 | Apple Silicon end-to-end zero-copy unrealistic | §6.3 excluded from the first build |
| R4 | dma-buf fence/sync semantics under-specified | §1.4 + `sync_fence_kind` field |
| R5 | Page faults break 1kHz determinism | §3.2 mlockall + MAP_POPULATE |
| R6 | dma_resv lock priority inversion | §3.4 producer-only attach |
| R7 | Buffer lifecycle / FD leaks | §4.2 state machine + RT close ban |
| R8 | Inference long tail (P99.99, thermal) | §5.1 `inference_worst_case` term + thermal margin |
| R9 | Stale infinite retry storms RT loop | §3.5 retry cap + fallback policy |
| R10 | Camera frame drop / NPU timeout | §3.5 fallback policy + per-subsystem timeouts |
| R11 | Benchmark cannot be reproduced | §8.2 environment spec pinned in the README |

---

## 10. Evolution Path

### 10.1 Phase 1 (12-14 weeks, scope of this design)

- Single host, multi-process, single accelerator backend.
- C++ core + Python bindings.
- Closed-loop mini demo + ROS2 baseline comparison + public release.

### 10.2 Phase 2 (after Phase 1 validation)

- First-class Rust crate (Iceoryx2 native).
- Second accelerator backend.
- LeRobot-compatible dataset export adapter.

### 10.3 Phase 3 (expansion)

- Discrete NUMA backend (PCIe P2P, GPUDirect RDMA).
- Optional graph-execution runtime (Approach B), built on Phase 1 abstractions.
- ROS2 RMW wrapper (Approach C) for adoption by existing ROS2 users.

### 10.4 Phase 4 (community)

- Multi-host distribution (integrated with zenoh or Iceoryx2 distributed mode).
- First-class PTP time synchronization.
- SLAM/VLA reference pipelines.

---

## Appendix A — Glossary

| Term | Meaning |
|---|---|
| dma-buf | Linux-kernel mechanism for cross-device buffer sharing (FD-based) |
| `dma_resv` | dma-buf synchronization object (a fence container) |
| `sync_file` | Exposes a dma-buf fence to user space as an FD |
| `SCM_RIGHTS` | Ancillary message that delivers FDs across processes over a Unix domain socket |
| `pidfd_getfd` | Linux 5.6+ syscall — fetch an FD from another process via its pidfd |
| seqlock | Lock-free reader-writer variant; the writer toggles between even and odd seqno values |
| Iceoryx2 | Rust-based shared-memory zero-copy messaging middleware |
| PREEMPT_RT | Linux RT patch providing hard real-time guarantees |
| UMA | Unified Memory Architecture — CPU and accelerator share the same RAM |
| XDNA | AMD AI Engine native NPU architecture |
| NVMM | NVIDIA Multimedia Memory — dma-buf-compatible bridge between V4L2 and CUDA on Jetson |
| TensorRT | NVIDIA GPU/Jetson inference runtime |

## Appendix B — Reference Documents

- `data-centric-zero-copy-design-20260510.md` — direction, scope, differentiation, risks (Status: APPROVED v2)
- `gemini-code-1778342991663.py` — initial design memo (Status: superseded by this document)
