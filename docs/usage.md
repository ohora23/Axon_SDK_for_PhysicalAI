# Usage & Examples

Hands-on examples for the `axon` API. Every snippet here maps to a **runnable,
tested example** in the repo — the file that verifies it is named next to each
section.

- Producer / consumer basics → [`examples/demo/axon_demo.cpp`](../examples/demo/axon_demo.cpp)
- Depth frame with padding + v2 metadata → [`examples/depth_filter/depth_filter_demo.cpp`](../examples/depth_filter/depth_filter_demo.cpp)
- Cross-process **GPU** zero-copy (R6) → [`examples/accelerator_pool/accelerator_pool_demo.cpp`](../examples/accelerator_pool/accelerator_pool_demo.cpp)
- Python (zero-copy NumPy) → [`python/tests/test_axon.py`](../python/tests/test_axon.py)

The mental model in one line: the **producer** owns a pool of shared buffers,
writes each frame straight into a pooled buffer, and publishes a tiny
descriptor; the **consumer** reads the most recent frame as a zero-copy view.
Only descriptors (≤256 B) and the pool FDs (once) ever cross the process
boundary.

---

## 1. Minimal producer / consumer (C++, no camera)

The `Custom` backend is memfd-backed — it needs no camera, no `/dev/udmabuf`,
and no special privileges, so it's the way to try the full data path on any
Linux box.

**Producer** (non-RT): allocate a pool, hand its FDs to consumers once, then
write each frame into a pooled buffer and publish.

```cpp
#include "axon/pool.h"
#include "axon/publisher.h"

using namespace axon;

auto pool = TensorPool::create({
    .n_buffers   = 8,
    .buffer_size = 640 * 480 * 2,          // one U16 frame
    .backend     = PoolBackend::Custom,    // memfd — works anywhere
    .v4l2_device = nullptr,
});
auto pub = TensorPublisher::create("camera/frames", *pool);

pub->handshake_pool();                     // SCM_RIGHTS: deliver pool FDs once

for (std::uint64_t seq = 0; running; ++seq) {
    AcquiredDescriptor a = pub->acquire_descriptor();
    if (a.buffer_index < 0 || !a.host_view) break;   // pool exhausted

    // Write the frame straight into the pooled dma-buf — no staging copy.
    auto* dst = static_cast<std::uint16_t*>(a.host_view);
    render_frame_into(dst, 640, 480);

    a.desc->rank = 2;
    a.desc->shape[0] = 480;                // H
    a.desc->shape[1] = 640;                // W
    a.desc->dtype = DType::U16;
    a.desc->offset = 0;
    a.desc->size = 640 * 480 * 2;

    if (pub->publish(std::move(a)) != 0)   // publishes the descriptor only
        perror("publish");                 // EMSGSIZE: size/offset past the buffer; EINVAL: bad field
}
```

`publish()` refuses a descriptor it cannot publish safely and says so: it
returns -1 with `errno = EMSGSIZE` when `[offset, offset + size)` does not fit
in the pool buffer, `EINVAL` for another malformed field. Nothing is published
and no seqno is spent, so consumers see no gap; `rejected_count()` counts these.

**Consumer**: attach to the same service name, then read the latest frame.
`latest_view()` never blocks and never copies the payload.

```cpp
#include "axon/subscriber.h"

using namespace axon;

auto sub = TensorSubscriber::create("camera/frames");
if (sub->wait_handshake(/*timeout_ms=*/5000) != 0) return 1;   // non-RT

while (running) {
    auto v = sub->latest_view(/*max_retry=*/8);
    if (v) {
        const auto* px = static_cast<const std::uint16_t*>(v->data);  // zero-copy
        use(px, v->shape.dims[1], v->shape.dims[0]);   // W, H
        // v->seqno, v->staleness_ns are always available.
    }
}
```

> Start the producer first — the consumer's `wait_handshake()` connects to the
> sidecar and waits up to `timeout_ms` for the pool FDs. The repo's demos do
> this with a single binary that `fork()`s, which is the version covered by
> `ctest`.

---

## 2. RT consumer pattern (bounded staleness)

The RT loop touches nothing but `latest_view()`: no allocation, no syscalls,
no blocking. Do the locking/prefault once, up front.

```cpp
#include "axon/subscriber.h"
#include "axon/rt.h"

using namespace axon;

auto sub = TensorSubscriber::create("camera/inference_out");
sub->wait_handshake();                                  // non-RT: attach + import
sub->set_fallback_policy(FallbackPolicy::LastKnownGood);

rt_setup_memory_and_sched();      // mlockall + prefault + SCHED_FIFO (once)

while (rt_tick()) {               // your 1 kHz tick
    auto v = sub->latest_view(8);
    if (v) {
        control_step(v->data, v->shape);
        if (v->staleness_ns > kDeadlineNs) flag_stale();   // measured, per-frame
    }
    // On max_retry exceeded the fallback policy is applied internally;
    // fallback_invocation_count() lets you monitor it.
}
```

`staleness_ns` is the measured age of the frame you just read — the quantity
the [7-term bound](../DesignFiles/detailed_design_doc.md#5-bounded-staleness-formula)
caps. It's yours to use directly in safety logic.

### Event-driven consumer (wake on publish)

A fixed-rate loop makes a frame wait for the next tick: half a period on
average (~0.5 ms at 1 kHz). A non-RT consumer that should react to each frame
— perception, logging, a recorder — can block until something is published:

```cpp
std::vector<TensorSubscriber*> subs = {cam_front.get(), cam_left.get(), lidar.get()};
for (;;) {
    int i = TensorSubscriber::wait_any(subs.data(), subs.size(), /*timeout_ms=*/100);
    if (i < 0) continue;                      // timeout (errno == ETIMEDOUT) or error
    for (auto* s : subs)                      // take every stream that changed
        if (auto v = s->latest_view(8)) handle(*v);
}
// one stream: sub->wait_for_update(timeout_ms) -> 1 update, 0 timeout, -1 error
```

The wait sleeps on a futex in the metadata slot (`futex_waitv` for several
streams, Linux ≥ 5.16), so it costs no CPU while idle. `latest_view()` re-arms
it: a wait returns at once if a frame arrived since this subscriber's last
`latest_view()`. The publisher only makes a `FUTEX_WAKE` syscall while someone
is waiting, so `publish()` stays syscall-free for RT producers with no
event-driven consumer. On older kernels (Jetson L4T r35 is 5.10), with more
than 128 streams, or on the Iceoryx2 backend it falls back to polling every
100 µs. Keep `wait_any` out of the RT loop itself: the RT loop stays on
`latest_view()` at its own rate.

### Keep a low-rate producer's core awake (CPU wake-up latency)

A producer that sleeps between frames, say a 30 Hz camera, lets its core drop into a deep
idle state. It then wakes into a lower clock and writes the frame slowly. Measured on a Ryzen
9800X3D, a 30 Hz producer writing 16 MiB on its own core gave these numbers
(one PC, 2 runs per setting across two sessions; benchmarks/ros2_cpp, "The producer's clock
decides low-rate host numbers"):

| idle states allowed | p50 | clock |
|---|---|---|
| all (default) | 397-424 µs | 3.1-3.5 GHz |
| up to C2 (20 µs limit) | 418-428 µs | 3.0-3.6 GHz |
| up to C1 (**1 µs limit**) | 254-271 µs system-wide, 241-242 µs on the producer's CPU only | 4.6-5.0 GHz |
| polling only (0 µs) | 234-260 µs | 5.2 GHz |

`CpuLatencyHold` sets such a limit for as long as it lives. **1 µs on the producer's CPU** keeps
most of the gain at the median: idle cores halt instead of spinning, as they would with polling
(0), and the other cores keep their deep idle states.

The averages hide a tail. Per frame, most writes ran at either the full clock or half of it,
and under the 1 µs limit 26-30 % of frames still ran below 4 GHz: p50 212 µs, but **p90 419 µs**.
Of the limits, only polling (0 µs) removed it. On that machine, a core that had been busy for
2 ms before the write always ran it at full clock, so a producer that cares about its tail can
instead wake early and spin: sleeping until 2 ms
before each frame, then busy-waiting to the frame time, gave p50 210 µs and p90 212-216 µs with
no limit at all, for about 6 % of a core at 30 Hz. Spinning less than the work itself does
nothing (250 µs: p90 423-424 µs). The numbers are for one Ryzen 9800X3D with `amd-pstate-epp`;
measure the ramp on the target with `benchmarks/ros2_cpp/run_frame_clock_probe.sh`.

| producer setup | p50 | p90 |
|---|---|---|
| no limit | 419-430 µs | 424-549 µs |
| 1 µs limit on its CPU | 212-213 µs | 419 µs |
| 0 µs limit on its CPU | 210-212 µs | 215-221 µs |
| no limit, wake 2 ms early and spin | 210-211 µs | 212-216 µs |

`rt_sleep_until_prewarmed(deadline_ns, spin_us)` does that: it sleeps until `spin_us` before
the deadline (on the `rt_now_ns()` clock), busy-waits the rest and returns `rt_now_ns()`. Through
it, the same producer measured p50 212 µs and p90 220-221 µs with `spin_us = 2000` (2 runs).

```cpp
std::uint64_t next = rt_now_ns();
for (;;) {
    next += 33'333'333;                       // 30 Hz
    rt_sleep_until_prewarmed(next, 2000);     // asleep until 2 ms before, then spinning
    write_and_publish_frame();
}
```

```python
nxt = axon.rt_now_ns()
while True:
    nxt += 33_333_333
    axon.rt_sleep_until_prewarmed(nxt, 2000)  # releases the GIL while it waits
    write_and_publish_frame()
```

The spin is busy CPU time on that core every period. Combine it with pinning, and size `spin_us`
from a measurement on the target; a CPU whose clock does not drop needs none.

```cpp
RtSetupConfig cfg;
cfg.set_cpu_affinity = true;
cfg.cpu_id = 6;              // pin the producer...
cfg.cpu_latency_us = 1;      // ...and keep that core out of C2 and deeper, for the process lifetime
rt_setup_memory_and_sched(cfg);

// or scoped, for one phase:
if (auto hold = CpuLatencyHold::acquire(/*max_latency_us=*/1, /*cpu=*/6)) { run_capture(); }
else perror("CpuLatencyHold");   // EACCES: see below
```

```python
with axon.cpu_latency_hold(1, cpu=6):
    run_capture()
```

- **`cpu = -1`** applies the limit system-wide through `/dev/cpu_dma_latency`. That needs root
  by default. The kernel drops the request when the fd closes, including when the process dies.
  Every core then stays awake, which costs the most power.
- **`cpu >= 0`** applies it to that core only, through `cpuN/power/pm_qos_resume_latency_us`. The
  hold restores the old value on destruction. A process killed without unwinding leaves the
  limit in place. The attribute holds one request: give each CPU one owner, and nest holds on
  it only LIFO.
- Without root, both paths need [`tools/udev/99-axon-cpu-latency.rules`](../tools/udev/99-axon-cpu-latency.rules),
  which grants a dedicated `axon-rt` group. Any member can keep every core awake, so keep that
  group to the RT users.
- A hold belongs to the process that took it: a `fork()`ed child neither restores nor closes it.
  Holds are not thread-safe: take and release them from one thread. A second
  `rt_setup_memory_and_sched` call replaces the first call's hold.
- In Python, prefer `with`. A hold left to the garbage collector may never be released at
  interpreter exit, which matters for the per-CPU path.
- Pin the producer and an RT consumer to different physical cores. `rt_core_siblings(cpu)` lists
  a core's SMT siblings.
- The C-state names and exit latencies of a machine are in
  `/sys/devices/system/cpu/cpu0/cpuidle/state*/{name,latency}`. Pick the limit just below the
  first state that hurts, then measure; `benchmarks/ros2_cpp/run_clock_probe.sh` shows how.

---

## Delivery semantics & choosing `n_buffers`

axon is **latest-value-wins**, by design: `acquire_descriptor()` round-robins
over the pool and overwrites the oldest slot, so the producer is **wait-free**
(it never blocks on a slow consumer) and a consumer always reads the *freshest*
frame — the right default for RT control, where a stale sample is worse than a
skipped one. There is **no lossless / back-pressure mode**: axon does not
guarantee a consumer observes every frame.

Only the **descriptor** is seqlock-protected; the **payload buffer is not**. So
if a consumer keeps using a view for longer than it takes the producer to lap
the ring and rewrite that same slot, it can read a **torn frame**. `n_buffers`
is what sizes that safety margin:

```
n_buffers  ≥  ceil( T_hold × f_produce ) + M + margin
```

- **`T_hold`** — the longest time a consumer keeps *touching* the buffer behind
  one `latest_view()` (its read / copy-out / processing on that slot), in
  seconds.
- **`f_produce`** — producer publish rate (frames/s).
- **`M`** — number of concurrent consumers each holding a view at once.
- **`margin`** — 1–2 slots of headroom for jitter.

Example: a 120 fps producer with a consumer that spends up to 20 ms on a frame
and one extra recorder → `ceil(0.020 × 120) + 2 + 1 = 6`. The demos default to
**4–8**, which covers typical single-host inference; raise it if a consumer does
heavy per-frame work or you run many consumers. Cost is RAM (`n_buffers × buffer_size`), and,
for host buffers of several MiB, cache. As the ring, the producer's source and the readers'
traffic approach the last-level cache size, more of the producer's write goes to DRAM. At
16 MiB on a 96 MB-L3 CPU, 4 buffers filled 15 % of each frame from DRAM against 0.5-1.3 % with
1-2, and p50 was 1.6-1.7× higher than with 2 or 1 (benchmarks/ros2_cpp, "Why ring depth costs at 16 MiB"). So for large
host frames, use the smallest ring the formula allows.

**If you truly need every frame intact** (e.g. lossless recording), don't push
back on the RT path — record the *raw topic* alongside axon. A recorder has to
copy the bytes into its own store anyway, so it gains little from zero-copy;
keeping it off the shared ring keeps the producer wait-free for the live
consumers that do benefit.

---

## 3. Python (zero-copy NumPy)

The Python module is named `axon` (built with `-DAXON_BUILD_PYTHON=ON`; add
`build/python` to `PYTHONPATH`). Publishing takes a NumPy array; a view hands
back a NumPy array that points **straight at the dma-buf** — no copy.

```python
import numpy as np
import axon

pool = axon.TensorPool.create(n_buffers=8, buffer_size=640 * 480 * 2,
                              backend=axon.PoolBackend.Custom)
pub  = axon.TensorPublisher.create("camera/frames", pool)
pub.handshake_pool()

frame = np.zeros((480, 640), dtype=np.uint16)
pub.publish(frame, axon.DType.U16)          # bytes copied into the pool buffer once

# --- in the consumer process ---
sub = axon.TensorSubscriber.create("camera/frames")
sub.wait_handshake(5000)
view = sub.latest_view(8)
if view is not None:
    arr = view.data                          # np.ndarray aliasing the dma-buf (read-only)
    print(view.seqno, view.staleness_ns, arr.shape)
```

See [`python/tests/test_axon.py`](../python/tests/test_axon.py) for the tested
round-trip.

---

## 4. Cross-process GPU zero-copy (accelerator pool, R6)

With `-DAXON_WITH_CUDA=ON`, a pool can hold **CUDA VMM device buffers**. Each is
exported as a POSIX FD (returned by `dma_buf_fds()`), so the same sidecar
handshake carries a live GPU allocation to another process — the payload never
leaves the GPU.

```cpp
auto pool = TensorPool::create({
    .n_buffers   = 4,
    .buffer_size = 2 * 1024 * 1024,
    .backend     = PoolBackend::Accelerator,   // CUDA VMM device memory
    .v4l2_device = nullptr,
});

void* dptr = pool->device_ptr(0);              // CUdeviceptr for producer-side kernels
int   fd   = pool->dma_buf_fds()[0];           // POSIX export handle for the sidecar
```

The consumer imports the **same physical allocation** with
`cuMemImportFromShareableHandle` (device memory is not host-mmap-able, so this
path does not use the host-mmap subscriber). The full, verified round-trip —
producer fills the device buffer, a second process imports and validates it
on-GPU — is [`examples/accelerator_pool/accelerator_pool_demo.cpp`](../examples/accelerator_pool/accelerator_pool_demo.cpp).

```bash
cmake -S . -B build -DAXON_WITH_CUDA=ON
cmake --build build -j
./build/examples/accelerator_pool/axon_accelerator_pool_demo
# -> device zero-copy: OK (262144/262144 elems match, 2.0 MiB)
```

---

## 5. Jetson: camera → CUDA zero-copy (NvBufSurface pool)

With `-DAXON_ENABLE_JETSON=ON` (JetPack ≥ 5, headers from `/usr/src/jetson_multimedia_api`)
a pool holds **NvBufSurface** buffers. Unlike the CUDA VMM pool (§4) these are real
dma-bufs, so one buffer is at the same time:

- a **V4L2 capture target** — a UVC camera writes into it directly (`V4L2_MEMORY_DMABUF`),
- a **host view** — consumers mmap it through the normal subscriber path (`TensorView::data`),
- a **CUDA view** — with `-DAXON_WITH_CUDA=ON` the consumer also gets `TensorView::device_ptr`
  (imported via `NvBufSurfaceImport` → EGLImage → CUDA during `wait_handshake()`, so
  `latest_view()` stays syscall-free).

```cpp
TensorPoolConfig cfg{6, 0, PoolBackend::NvBufSurface, nullptr};  // buffer_size 0: surface decides
cfg.width  = 640;
cfg.height = 480;
cfg.fourcc = V4L2_PIX_FMT_YUYV;            // YUYV, GREY or RGBA32 (single-plane)
auto pool = TensorPool::create(cfg);

pool->row_pitch();      // surface pitch — set it as the descriptor's row_pitch
pool->dma_buf_fds();    // hand these to VIDIOC_QBUF (memory = V4L2_MEMORY_DMABUF)
pool->device_ptr(i);    // producer-side CUDA pointer (mapped on first call)
```

The camera decides which buffer it filled, so publish that one:
`a.buffer_index = v4l2_buf.index;` after `acquire_descriptor()`, and keep the last
published buffer out of the camera queue until the next frame is published (the
consumer may still be reading it). Ship YUYV as rank 2 `[H, W·2]` U8 — axon treats the
last dimension as one pitched row. The full, verified loop is
[`examples/jetson_camera/`](../examples/jetson_camera/README.md); the probe that
established each step is in [hardware-verification §6](hardware-verification.md#6-jetson--camera--cuda-zero-copy-via-nvbufsurface-agx-xavier-).

The pool handshake carries a per-buffer import blob for this backend, which is why the
wire version is **4**.

---

## Building against axon

In-tree (what the examples do): link the `axon` target.

```cmake
add_subdirectory(path/to/axon)          # or find_package once installed
add_executable(my_app my_app.cpp)
target_link_libraries(my_app PRIVATE axon)   # brings include dirs + rt + pthread
```

Backend/feature flags (all default **off**, so the core stays dependency-free):

| Flag | Adds |
|---|---|
| `-DAXON_BUILD_PYTHON=ON`   | pybind11 module `axon` (zero-copy NumPy) |
| `-DAXON_WITH_ICEORYX2=ON`  | Iceoryx2 lock-free SHM metadata backend ([docs/metadata-backends.md](metadata-backends.md)) |
| `-DAXON_WITH_CUDA=ON`      | `PoolBackend::Accelerator` CUDA VMM device pool (§4) |
| `-DAXON_ENABLE_JETSON=ON`  | `PoolBackend::NvBufSurface` Jetson pool; with CUDA also the device view (§5) |

Run the tested examples directly:

```bash
cmake -S . -B build && cmake --build build -j
(cd build && ctest --output-on-failure)          # 9/9
./build/examples/demo/axon_demo                  # producer+consumer (fork), measured
./build/examples/depth_filter/axon_depth_filter  # padded U16 depth + v2 metadata
```
