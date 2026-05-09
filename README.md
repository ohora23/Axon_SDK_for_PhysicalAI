# dczc — Data-Centric Zero-Copy for Physical AI

> **One-liner**: Not another middleware on top of ROS2. We extend the **sensor → accelerator → RT control loop** path with end-to-end zero-copy that doesn't break, and a **bounded staleness that is measured and guaranteed**.

[![Status: Pre-alpha](https://img.shields.io/badge/status-pre--alpha-orange)]()
[![License: Apache 2.0](https://img.shields.io/badge/license-Apache%202.0-blue)](LICENSE)
[![Platform: Linux + PREEMPT_RT](https://img.shields.io/badge/platform-Linux%20%2B%20PREEMPT__RT-success)]()

> ⚠️ **Status**: design phase. Week 1-2 spike PoC in progress. APIs and implementation are still in flux. This README is a draft written from the perspective of the public release.

---

## The Pitch (60 seconds)

ROS2 + Iceoryx2 integrations provide zero-copy at the **message middleware level**. A message can reach RAM zero-copy, but moving it onto an accelerator (NPU/GPU) typically requires another copy.

`dczc` adds **two missing planes** on top of that:

1. **FD plane** — pass dma-buf FDs directly through a `SCM_RIGHTS` / `pidfd_getfd(2)` sidecar. The dma-buf exported by V4L2 capture is imported by the accelerator driver — **no host memory copy**.
2. **Time plane** — inference runs in a non-RT worker; the RT control loop reads results via a seqlock as a zero-copy view. The staleness bound is computed by an **explicit formula** (7 terms) and is therefore directly usable in safety analysis.

---

## Data Flow at a Glance

```mermaid
flowchart LR
    SENSOR["📷 Sensor"] --> CAPTURE["V4L2 Capture<br/>VIDIOC_EXPBUF"]
    CAPTURE --> DMABUF["dma-buf"]
    DMABUF --> NPU["NPU/GPU Import<br/>+ Inference"]
    NPU --> OUT["Output dma-buf"]
    OUT --> META["Iceoryx2:<br/>TensorDescriptor (meta only)"]
    META --> RT["RT seqlock read<br/>(1kHz)"]
    OUT -.->|"zero-copy view"| RT
    RT --> CTL["Control Loop"]
    CTL --> ACT["Actuator"]

    classDef zerocopy fill:#c8e6c9,stroke:#2e7d32,stroke-width:2px
    classDef meta fill:#bbdefb,stroke:#1565c0,stroke-width:2px
    class DMABUF,OUT,RT zerocopy
    class META meta
```

Green = zero-copy region. Blue = metadata message. Twelve more diagrams in [`DesignFiles/diagrams.md`](DesignFiles/diagrams.md).

---

## Target Metrics

| Metric | Target | Baseline (ROS2 + rmw_iceoryx_cpp) |
|---|---|---|
| Sensor→command P99 latency | ≤ `T_inf + 5ms` | TBD (week 1-2 baseline) |
| 1kHz RT worst-case jitter | < 100µs | cyclictest 24h |
| Inter-device memory copies | **0** (eBPF-verified) | typically 1-3 |
| Staleness bound | sum of 7 explicit terms | non-deterministic |
| Page faults during RT | **0** | OS / config dependent |

The formula:
```
worst_case_staleness ≤ 
    T_cap + T_fence_p + T_inf + T_pub
  + T_sc + T_rt_seq + T_view
```
[Definition](DesignFiles/detailed_design_doc.md#5-bounded-staleness-formula) | [Visualization](DesignFiles/diagrams.md#9-bounded-staleness--visualized)

---

## 30-Second Demo (work in progress)

```
[ TODO: 30-second video — closed-loop mini demo ]
[ V4L2 camera → NPU inference → 1kHz RT control loop ]
[ Live overlay of the metrics above ]
```

Video drops after the first build (week 9-10).

---

## Quick Build (placeholder)

> The only currently buildable code is the week 1-2 spike PoC under `examples/spike_poc/`. The full library lands in week 4+.

### Prerequisites

- Linux + PREEMPT_RT-patched kernel (only required for RT validation. The spike PoC alone runs on a stock kernel.)
- A V4L2-compatible camera (a USB UVC camera works)
- One accelerator board:
  - **AMD AI Series** (XDNA NPU) — XDNA driver, ROCm 6.x+
  - **NVIDIA Jetson Orin** — JetPack 6.x, CUDA 12.x
- gcc 11+, cmake 3.22+

### Spike PoC Build (week 1-2 validation)

```bash
# Build both producer and consumer
cmake -S examples/spike_poc -B build/spike -DCMAKE_BUILD_TYPE=Release
cmake --build build/spike -j

# Run (one terminal)
./build/spike/dczc_spike_producer /dev/video0

# Other terminal
./build/spike/dczc_spike_consumer
```

What the PoC validates:
- ✅ V4L2 capture → `VIDIOC_EXPBUF` exports a dma-buf FD
- ✅ `SCM_RIGHTS` delivers the dma-buf FD across processes
- ✅ The received FD is mmap'd as a zero-copy host view (eBPF-verified)
- 🟡 Accelerator import (AMD XDNA / NVIDIA Jetson — decided in spike)

[Spike guide](examples/spike_poc/README.md) | [Spike decision tree](DesignFiles/diagrams.md#12-week-1-2-spike-poc-decision-tree)

---

## API Preview (current header skeleton)

```cpp
#include <dczc/publisher.h>
#include <dczc/subscriber.h>
#include <dczc/pool.h>

// Producer side (non-RT)
auto pool = dczc::TensorPool::create({
    .n_buffers   = 32,
    .buffer_size = 4 * 1024 * 1024,
    .backend     = dczc::PoolBackend::V4L2,
});
auto pub = dczc::TensorPublisher::create("camera/inference_out", *pool);
pub->handshake_pool();  // SCM_RIGHTS bulk transfer

while (running) {
    auto desc = pub->acquire_descriptor();
    fill_shape_dtype(desc, /*...*/);
    // ... write inference output directly into the pool buffer ...
    pub->publish(std::move(desc));
}

// Consumer side (RT 1kHz loop)
auto sub = dczc::TensorSubscriber::create("camera/inference_out");
sub->wait_handshake();
sub->set_fallback_policy(dczc::FallbackPolicy::LastKnownGood);

dczc::rt_setup_memory_and_sched();  // mlockall + MAP_POPULATE + SCHED_FIFO

while (rt_tick()) {
    auto view = sub->latest_view(/*max_retry=*/8);
    if (view) {
        process(view->data, view->shape);
        log_staleness(view->staleness_ns);
    }
    // fallback is applied internally by sub
}
```

[Full API](include/dczc/) | [`TensorDescriptor` definition](DesignFiles/detailed_design_doc.md#112-tensordescriptor-definition-iceoryx2-payload)

---

## FAQ

**Q. ROS2 + Iceoryx2 integrations already exist. Why another?**
A. `rmw_iceoryx_cpp` and Iceoryx2's ROS2 integration give zero-copy at the **message middleware level**. dczc adds the **dma-buf FD sidecar + accelerator import integration layer** on top, so zero-copy is unbroken from sensor through accelerator and into the RT control loop. [Differentiation in detail](DesignFiles/data-centric-zero-copy-design-20260510.md#premises-agreed)

**Q. Can dma-buf FDs be sent through Iceoryx2 SHM?**
A. No. Writing an integer FD into shared memory means nothing in another process — FD tables are per-process. Cross-process FD transfer requires `SCM_RIGHTS` or `pidfd_getfd(2)`. [Sidecar handshake sequence](DesignFiles/diagrams.md#3-fd-handshake-sequence)

**Q. Does the RT loop call NPU inference directly?**
A. No. NPU inference latency has a long tail through P99.99 and is affected by thermal throttling — it cannot be bounded deterministically. dczc runs inference in a non-RT worker; the RT loop reads only the **most recent inference result with a measured/guaranteed staleness bound**, as a zero-copy view via seqlock. [RT pattern](DesignFiles/detailed_design_doc.md#33-rt-consumer-pattern-seqlock)

**Q. Which platform is the first build target?**
A. Decided during the week 1-2 spike PoC. Candidates: **AMD AI Series (XDNA)** and **NVIDIA Jetson Orin**. Apple Silicon is **out of scope for the first build** because macOS lacks V4L2 and Asahi Linux lacks an ANE driver. [Decision tree](DesignFiles/diagrams.md#12-week-1-2-spike-poc-decision-tree)

**Q. Multi-host (distributed) support?**
A. The first build is single-host multi-process only. Multi-host is Phase 4 (zenoh integration or Iceoryx2 distributed mode). [Evolution path](DesignFiles/diagrams.md#11-evolution-path--phase-1--phase-4)

**Q. Can ROS2 users adopt this?**
A. Yes — Phase 3 ships a ROS2 RMW backend (Approach C). The Phase 1 core is a minimal library with no ROS2 dependency, so ROS2 users can adopt it via a thin wrapper.

**Q. License?**
A. Apache 2.0 (with patent grant — friendlier for robotics industry adoption).

---

## Reproducible Benchmark Environment

> After the first build, the README's benchmark graphs are measured under:

```
[TODO: filled in after the first build]
- Board: AMD AI Series xxx or NVIDIA Jetson Orin xxx
- Kernel: Linux x.y.z + PREEMPT_RT
- Camera: resolution / fps
- Model: name + input tensor shape + inference rate
- Measurement window: cyclictest 24h
- ROS2 baseline: distro / RMW / QoS
```

---

## Design Document Tree

| Document | Role |
|---|---|
| [`DesignFiles/data-centric-zero-copy-design-20260510.md`](DesignFiles/data-centric-zero-copy-design-20260510.md) | Direction, differentiation, risk/mitigation (APPROVED v2) |
| [`DesignFiles/detailed_design_doc.md`](DesignFiles/detailed_design_doc.md) | Mechanism details (14 sections, ~700 lines) |
| [`DesignFiles/diagrams.md`](DesignFiles/diagrams.md) | 12 Mermaid diagrams |
| `examples/spike_poc/README.md` | Week 1-2 spike PoC guide |

---

## Roadmap

- [x] Office hours → differentiation, audience, scope locked in
- [x] Design doc v2 (APPROVED, reviewer score 5/10 → projected 8/10)
- [x] Mechanism detail document (~700 lines)
- [x] System diagrams (12 Mermaid views)
- [x] API header skeleton
- [ ] Week 1-2 spike PoC validation (← **current**)
- [ ] Week 4: TensorDescriptor + sidecar handshake formal implementation
- [ ] Week 6: accelerator import backend
- [ ] Week 8: RT consumer + closed-loop integration
- [ ] Week 10: cyclictest 24h + eBPF zero-copy verification
- [ ] Week 12: ROS2 baseline comparison benchmark
- [ ] Week 14: public release + video + external demos

---

## Contributing

This is a design-stage project. Issues and discussions are welcome. PRs will start being accepted after the spike PoC validates.

## License

Apache 2.0 — see [LICENSE](LICENSE)
