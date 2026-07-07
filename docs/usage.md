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

    pub->publish(std::move(a));            // publishes the descriptor only
}
```

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

Run the tested examples directly:

```bash
cmake -S . -B build && cmake --build build -j
(cd build && ctest --output-on-failure)          # 9/9
./build/examples/demo/axon_demo                  # producer+consumer (fork), measured
./build/examples/depth_filter/axon_depth_filter  # padded U16 depth + v2 metadata
```
