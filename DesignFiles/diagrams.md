# Data-Centric Physical AI Architecture — Diagrams

> **References**: `data-centric-zero-copy-design-20260510.md` (Status: APPROVED v2), `detailed_design_doc.md` (mechanism details).
>
> A collection of Mermaid diagrams: structural, sequence, and state. These are higher-resolution counterparts of the ASCII diagrams inside the detail document and can be embedded directly into the README, slides, and PR descriptions.
>
> **Rendering**: GitHub renders Mermaid natively. Locally, use VS Code's *Markdown Preview Mermaid Support* extension or `mermaid-cli` (`mmdc -i diagrams.md -o diagrams.svg`) to extract SVGs.

---

## Table of Contents

1. [System Architecture — 5 Planes](#1-system-architecture--5-planes) (detail doc §0)
2. [End-to-End Data Flow](#2-end-to-end-data-flow) (detail doc §0.2, §4.3)
3. [FD Handshake Sequence](#3-fd-handshake-sequence) (detail doc §1.3.3)
4. [End-to-End Sequence — T+0 → T+10.6ms](#4-end-to-end-sequence--t0--t106ms) (detail doc §4.3)
5. [FD Lifecycle State Machine](#5-fd-lifecycle-state-machine) (detail doc §4.2)
6. [Pool-Generation Change Sequence](#6-pool-generation-change-sequence) (detail doc §1.3.4)
7. [RT/non-RT Region Separation and Priority-Inversion Avoidance](#7-rtnon-rt-region-separation-and-priority-inversion-avoidance) (detail doc §3.1, §3.4)
8. [RT Consumer Seqlock Pattern](#8-rt-consumer-seqlock-pattern) (detail doc §3.3)
9. [Bounded Staleness — Visualized](#9-bounded-staleness--visualized) (detail doc §5)
10. [Risks → Mitigations](#10-risks--mitigations) (detail doc §9)
11. [Evolution Path — Phase 1 → Phase 4](#11-evolution-path--phase-1--phase-4) (detail doc §10)
12. [Week 1-2 Spike PoC Decision Tree](#12-week-1-2-spike-poc-decision-tree) (detail doc §6.4)

---

## 1. System Architecture — 5 Planes

How the five planes (metadata · FD · memory · time · sync) combine to form the path from sensor to control loop.

```mermaid
flowchart TB
    subgraph Sensor["Sensor Layer"]
        CAM["Camera / LiDAR"]
        V4L2["V4L2 Driver"]
    end

    subgraph Memory["Memory Plane (zero-copy payload)"]
        POOL[("dma-buf Pool<br/>(N buffers)")]
    end

    subgraph Compute["Compute Plane — non-RT"]
        IMPORT["Accelerator<br/>Import"]
        INF["Inference Worker<br/>(NPU / GPU)"]
    end

    subgraph Meta["Meta Plane"]
        ICE["Iceoryx2 SHM Queue<br/>(TensorDescriptor only)"]
    end

    subgraph FDChan["FD Plane"]
        SC["SCM_RIGHTS /<br/>pidfd_getfd sidecar"]
    end

    subgraph Sync["Sync Plane"]
        FENCE["dma_resv implicit /<br/>sync_file explicit"]
    end

    subgraph RT["Time Plane — RT (1kHz)"]
        SUB["RT Subscriber<br/>(seqlock + mlockall)"]
        CTL["Control Loop"]
    end

    CAM --> V4L2 --> POOL
    POOL -.->|"FD (one-time)"| SC
    SC --> IMPORT
    POOL --> IMPORT --> INF
    INF --> POOL
    INF -->|"TensorDescriptor<br/>(meta only)"| ICE
    INF -->|"sync_file FD"| SC
    ICE -->|"seqlock read"| SUB
    SC -.->|"cached FDs"| SUB
    POOL -.->|"zero-copy view"| SUB
    FENCE -.-> SUB
    SUB --> CTL

    style Memory fill:#e8f5e9,stroke:#2e7d32
    style Compute fill:#fff3e0,stroke:#e65100
    style Meta fill:#e3f2fd,stroke:#1565c0
    style FDChan fill:#f3e5f5,stroke:#6a1b9a
    style RT fill:#ffebee,stroke:#c62828
    style Sync fill:#fffde7,stroke:#f57f17
```

**Key observations**:
- The `dma-buf Pool` is a single block of memory. Capture, import, inference, and view all point at the same physical memory.
- Only ≤ 144 B of metadata flows over `Iceoryx2`. The payload does not flow.
- `SCM_RIGHTS` is dotted = used only at handshake time; it does not appear on the steady-state publish path.

---

## 2. End-to-End Data Flow

A simpler view of the same flow without the plane partitioning.

```mermaid
flowchart LR
    SENSOR["📷 Sensor"] --> CAPTURE["V4L2 Capture<br/>VIDIOC_EXPBUF"]
    CAPTURE --> DMABUF["dma-buf"]
    DMABUF --> NPU["NPU/GPU Import<br/>+ Inference"]
    NPU --> OUT_DMABUF["Output dma-buf"]
    OUT_DMABUF --> META["Iceoryx2:<br/>TensorDescriptor"]
    META --> RT_READ["RT seqlock read"]
    OUT_DMABUF -.->|"zero-copy view"| RT_READ
    RT_READ --> CTL["1kHz Control Loop"]
    CTL --> ACT["Actuator"]

    classDef zerocopy fill:#c8e6c9,stroke:#2e7d32,stroke-width:2px
    classDef meta fill:#bbdefb,stroke:#1565c0,stroke-width:2px
    class DMABUF,OUT_DMABUF,RT_READ zerocopy
    class META meta
```

Green = zero-copy region (no host memory copy). Blue = metadata message.

---

## 3. FD Handshake Sequence

The producer delivers every dma-buf FD in the buffer pool to the consumer in a single shot, so the steady-state publish path has no sidecar calls.

```mermaid
sequenceDiagram
    autonumber
    participant P as Producer<br/>(non-RT)
    participant ICE as Iceoryx2 SHM
    participant SC as Sidecar Socket<br/>(SCM_RIGHTS)
    participant C as Consumer<br/>(RT or non-RT side)

    Note over P,C: ── Boot / subscribe start ──

    P->>ICE: Iceoryx2 service register
    C->>ICE: Iceoryx2 service discover
    C->>SC: connect (Unix domain socket)
    P->>P: Allocate buffer pool<br/>(N dma-bufs)

    Note over P,SC: Bulk-deliver every FD at once

    P->>SC: SCM_RIGHTS:<br/>FDs[N] + pool_generation=1
    SC->>C: deliver FDs
    C->>C: Register FDs in cache,<br/>accelerator-import or mmap each

    Note over P,C: ── Steady-state publish loop (no sidecar use) ──

    loop Every frame
        P->>P: capture → inference
        P->>ICE: publish(TensorDescriptor:<br/>bo_handle, seqno, pool_gen=1)
        ICE-->>C: lock-free read
        C->>C: cached_pool_gen == desc.pool_gen?<br/>YES → use cached handle
    end

    Note over P,C: ── On pool reallocation (rare) ──

    P->>P: Decide to retire pool<br/>pool_generation = 2
    P->>SC: SCM_RIGHTS: new FDs[M] + pool_gen=2
    SC->>C: deliver new FDs
    C->>C: Refresh cache

    P->>ICE: publish(... pool_gen=2)
    C->>C: cached_pool_gen=2 == desc.pool_gen=2 ✓
```

---

## 4. End-to-End Sequence — T+0 → T+10.6ms

Time axis from §4.3 of the detail document. Every step from capture to command inside one 1kHz cycle.

```mermaid
sequenceDiagram
    autonumber
    participant CAM as Camera<br/>(V4L2)
    participant POOL as dma-buf Pool
    participant W as Inference Worker<br/>(non-RT)
    participant ICE as Iceoryx2
    participant SC as Sidecar
    participant RT as RT Consumer<br/>(1kHz)
    participant ACT as Actuator

    Note over CAM,ACT: T+0ms — capture-complete<br/>(hardware timestamp)

    CAM->>POOL: capture into buf[i]<br/>(implicit dma_resv fence)

    Note over W: T+0.1ms — acquire buffer

    POOL->>W: buf[i] (FD already shared via SC at boot)
    W->>W: lookup cached import handle (cache hit)

    Note over W: T+0.3ms — inference starts

    W->>W: NPU inference<br/>(non-RT, ~10ms)

    Note over W,ICE: T+10ms — inference done

    W->>W: Create sync_file FD (output fence)
    W->>SC: Deliver sync_file FD<br/>(sidecar)
    W->>ICE: publish(TensorDescriptor:<br/>bo_handle, seqno=k, sync_fence_token)

    Note over RT: T+10.3ms — next 1kHz tick

    RT->>ICE: seqlock read latest
    ICE-->>RT: TensorDescriptor (seqno=k)
    RT->>SC: sync_file FD lookup (cached)
    RT->>RT: poll_non_blocking(fence) → ready

    Note over RT: T+10.5ms — view access

    POOL-->>RT: zero-copy view (mlocked, no page fault)
    RT->>RT: inference result → control command

    Note over RT,ACT: T+10.6ms — command sent

    RT->>ACT: motor command

    Note over CAM,ACT: Worst-case staleness ≤ 11ms<br/>(detail doc §5 formula)
```

---

## 5. FD Lifecycle State Machine

Visualization of the state machine in detail doc §4.2.

```mermaid
stateDiagram-v2
    [*] --> ALLOCATED: Producer allocates<br/>buffer pool

    ALLOCATED --> SHARED: SCM_RIGHTS<br/>bulk delivery

    SHARED --> ATTACHED_BY_N: N consumers<br/>attach (accelerator import / mmap)

    ATTACHED_BY_N --> ATTACHED_BY_N: New consumer attach<br/>(refcount++)

    ATTACHED_BY_N --> RETIRING: Producer decides<br/>to retire pool<br/>pool_generation++

    RETIRING --> RETIRING: Consumers detach<br/>(refcount--)

    RETIRING --> CLOSED: refcount = 0<br/>non-RT lifetime owner<br/>calls close(2)

    CLOSED --> [*]

    note right of ATTACHED_BY_N
        RT consumer is forbidden
        from calling close(2)
        (syscall = non-deterministic)
    end note

    note right of RETIRING
        Publishes during this window
        get a new pool_generation
        and trigger a sidecar
        re-handshake
    end note
```

---

## 6. Pool-Generation Change Sequence

The rare path where the pool is reallocated. The consumer detects the stale generation and re-requests through the sidecar.

```mermaid
sequenceDiagram
    autonumber
    participant P as Producer
    participant ICE as Iceoryx2
    participant SC as Sidecar
    participant C as Consumer

    Note over P,C: Steady-state operation (pool_gen=5)
    P->>ICE: publish(pool_gen=5, seqno=100)
    ICE-->>C: ok

    Note over P: ── Pool reallocation triggered ──
    Note right of P: e.g. resolution change,<br/>buffer shortage, memory pressure
    P->>P: Allocate fresh pool<br/>pool_generation = 6

    Note over P,C: Hazard window — consumer doesn't know yet

    P->>ICE: publish(pool_gen=6, seqno=101)
    ICE-->>C: deliver
    C->>C: cached_pool_gen=5 vs desc=6<br/>**MISMATCH** → classify as stale
    C->>SC: Request re-handshake
    P->>SC: SCM_RIGHTS: new FDs + pool_gen=6
    SC->>C: deliver new FDs
    C->>C: Refresh cache, re-import on accelerator

    Note over P,C: Steady-state resumes (pool_gen=6)
    P->>ICE: publish(pool_gen=6, seqno=102)
    ICE-->>C: ok (cache hit)

    Note over C: During the hazard window the RT loop<br/>applies the fallback policy<br/>(last_known_good, etc.)
```

---

## 7. RT/non-RT Region Separation and Priority-Inversion Avoidance

Core principle: **dma-buf attach/detach is performed only by the producer / non-RT worker.** The RT consumer only reads views of attached handles.

```mermaid
flowchart TB
    subgraph NonRT["non-RT Domain (SCHED_OTHER)"]
        direction TB
        PROD["Producer Process<br/>(capture + publish)"]
        WORKER["Inference Worker"]
        SUBHELPER["Subscriber attach helper<br/>(performs accelerator import)"]
    end

    subgraph RTDomain["RT Domain (SCHED_FIFO prio 80-95)"]
        direction TB
        RTSUB["RT Subscriber<br/>(seqlock read only)"]
        RTLOOP["1kHz Control Loop"]
    end

    subgraph DMABUF["dma_resv lock region (kernel)"]
        ATTACH["dma-buf attach/detach"]
        FENCE_OP["fence wait/signal"]
    end

    PROD -->|owns| ATTACH
    WORKER -->|owns| ATTACH
    SUBHELPER -->|owns| ATTACH
    SUBHELPER -.->|"hands over only<br/>attached handles"| RTSUB
    RTSUB -.->|"❌ no direct dma_resv access"| ATTACH
    RTSUB -->|"✓ non-blocking polling only"| FENCE_OP
    RTSUB --> RTLOOP

    style RTDomain fill:#ffebee,stroke:#c62828,stroke-width:2px
    style NonRT fill:#e3f2fd,stroke:#1565c0,stroke-width:2px
    style DMABUF fill:#fff9c4,stroke:#f57f17
```

**Rules**:
- The red region must **never** acquire the locks of the yellow region directly.
- Locks in the yellow region are taken only from the blue region; the result (handle) is then passed to the red region.
- Fence access from the red region uses **non-blocking polling** with a bounded retry.

---

## 8. RT Consumer Seqlock Pattern

```mermaid
flowchart TD
    START([1kHz tick start])
    INIT["retry = 0"]
    READ_SEQ_BEFORE["seq_before = atomic_load_acquire(slot.seqno)"]
    ODD_CHECK{"seq_before<br/>odd?<br/>(writer in progress)"}
    READ_DESC["TensorDescriptor desc = slot.desc<br/>(fixed-size POD copy)"]
    LOOKUP_HANDLE["handle = lookup_attached_handle(<br/>desc.bo_handle, desc.pool_gen)"]
    READ_SEQ_AFTER["seq_after = atomic_load_acquire(slot.seqno)"]
    SEQ_MATCH{"seq_before<br/>== seq_after?"}
    RETURN_VIEW["return view<br/>(staleness = now - desc.publish_ts)"]
    INC_RETRY["retry++"]
    MAX_CHECK{"retry<br/>>= max_retry?"}
    FALLBACK["Apply fallback policy:<br/>last_known_good /<br/>zero_command /<br/>user_callback"]
    END([Continue control loop])

    START --> INIT --> READ_SEQ_BEFORE --> ODD_CHECK
    ODD_CHECK -->|YES| INC_RETRY
    ODD_CHECK -->|NO| READ_DESC --> LOOKUP_HANDLE --> READ_SEQ_AFTER --> SEQ_MATCH
    SEQ_MATCH -->|YES| RETURN_VIEW --> END
    SEQ_MATCH -->|NO writer interleaved| INC_RETRY
    INC_RETRY --> MAX_CHECK
    MAX_CHECK -->|NO| READ_SEQ_BEFORE
    MAX_CHECK -->|YES| FALLBACK --> END

    style FALLBACK fill:#ffccbc,stroke:#bf360c
    style RETURN_VIEW fill:#c8e6c9,stroke:#2e7d32
```

---

## 9. Bounded Staleness — Visualized

How the seven terms add up. Normal values assume Jetson Orin.

```mermaid
flowchart LR
    T_CAP["T_cap<br/>sensor capture<br/>→ dma-buf export<br/><b>100-500 µs</b>"]
    T_FENCE_P["T_fence_p<br/>producer fence wait<br/>(DMA_BUF_IOCTL_SYNC)<br/><b>10-100 µs</b>"]
    T_INF["T_inf<br/>inference worst-case<br/>(P99.99 + thermal)<br/><b>5-15 ms</b>"]
    T_PUB["T_pub<br/>Iceoryx2 publish<br/><b>1-10 µs</b>"]
    T_SC["T_sc<br/>sidecar handshake<br/>(amortized)<br/><b>≈0 steady</b>"]
    T_RT_SEQ["T_rt_seq<br/>seqlock retry<br/><b>0-hundreds of ns</b>"]
    T_VIEW["T_view<br/>dma-buf view access<br/>(mlock guarantees)<br/><b>≈0</b>"]
    SUM["worst_case_staleness<br/><b>≤ sum</b>"]

    T_CAP --> T_FENCE_P --> T_INF --> T_PUB --> T_SC --> T_RT_SEQ --> T_VIEW --> SUM

    style T_INF fill:#ffccbc,stroke:#bf360c
    style T_CAP fill:#fff9c4,stroke:#f57f17
    style T_FENCE_P fill:#fff9c4,stroke:#f57f17
    style T_PUB fill:#c8e6c9,stroke:#2e7d32
    style T_SC fill:#c8e6c9,stroke:#2e7d32
    style T_RT_SEQ fill:#c8e6c9,stroke:#2e7d32
    style T_VIEW fill:#c8e6c9,stroke:#2e7d32
    style SUM fill:#bbdefb,stroke:#1565c0,stroke-width:3px
```

**Color meaning**:
- 🔴 Red (`T_inf`) — the largest term. Model worst-case + thermal margin dominates the staleness bound.
- 🟡 Yellow (`T_cap`, `T_fence_p`) — non-trivial µs-level terms. Need careful measurement and edge-case validation.
- 🟢 Green (the rest) — guaranteed ≈0 by mlockall + Iceoryx2 lock-free + amortized handshake.

→ **The optimization priority is clearly `T_inf`, which is the model selection / quantization / thermal-solution problem.** What systems infrastructure has to do is keep the yellow and green terms ≈0.

---

## 10. Risks → Mitigations

Mapping the eleven risks (R1-R11) to where they are mitigated.

```mermaid
flowchart LR
    subgraph Risks["Risks (design doc v2 §Reviewer)"]
        R1["R1: Iceoryx2 SHM is not<br/>an FD-transport channel"]
        R2["R2: AMD XDNA dma-buf<br/>import path unclear"]
        R3["R3: Apple Silicon<br/>unrealistic"]
        R4["R4: dma-buf fence/sync<br/>semantics under-specified"]
        R5["R5: Page faults break<br/>1kHz determinism"]
        R6["R6: dma_resv lock<br/>priority inversion"]
        R7["R7: FD leaks / lifecycle"]
        R8["R8: Inference long tail"]
        R9["R9: Stale infinite retry"]
        R10["R10: Frame drop /<br/>NPU timeout"]
        R11["R11: Benchmark not<br/>reproducible"]
    end

    subgraph Mitigations["Mitigations (detail doc location)"]
        M1["§1.3 sidecar"]
        M2["§6.4 spike PoC"]
        M3["§6.3 excluded from first build"]
        M4["§1.4 + sync_fence_kind field"]
        M5["§3.2 mlockall +<br/>MAP_POPULATE"]
        M6["§3.4 producer-only attach"]
        M7["§4.2 state machine +<br/>RT close ban"]
        M8["§5.1 inference_worst_case<br/>+ thermal margin"]
        M9["§3.5 retry cap +<br/>fallback policy"]
        M10["§3.5 fallback +<br/>per-subsystem timeouts"]
        M11["§8.2 environment spec README"]
    end

    R1 --> M1
    R2 --> M2
    R3 --> M3
    R4 --> M4
    R5 --> M5
    R6 --> M6
    R7 --> M7
    R8 --> M8
    R9 --> M9
    R10 --> M10
    R11 --> M11

    style Risks fill:#ffebee,stroke:#c62828
    style Mitigations fill:#e8f5e9,stroke:#2e7d32
```

---

## 11. Evolution Path — Phase 1 → Phase 4

```mermaid
flowchart LR
    P1["<b>Phase 1</b><br/>(12-14 weeks)<br/>• Single host multi-process<br/>• Single accelerator backend<br/>• C++ + Python<br/>• Closed-loop mini demo"]
    P2["<b>Phase 2</b><br/>• First-class Rust crate<br/>• Second accelerator<br/>• LeRobot export adapter"]
    P3["<b>Phase 3</b><br/>• Discrete NUMA<br/>  (PCIe P2P, GPUDirect)<br/>• Approach B<br/>  (graph runtime)<br/>• Approach C<br/>  (ROS2 RMW)"]
    P4["<b>Phase 4</b><br/>• Multi-host distributed<br/>  (zenoh integration)<br/>• PTP time synchronization<br/>• SLAM/VLA<br/>  reference pipelines"]

    P1 -->|after validation| P2
    P2 -->|after adoption signal| P3
    P3 -->|after community formed| P4

    style P1 fill:#bbdefb,stroke:#1565c0,stroke-width:3px
    style P2 fill:#c8e6c9,stroke:#2e7d32
    style P3 fill:#fff9c4,stroke:#f57f17
    style P4 fill:#f8bbd0,stroke:#c2185b
```

The transitions between phases are explicit — not "12 weeks elapsed → Phase 2" but **validation signal + adoption signal + community signal** as the trigger.

---

## 12. Week 1-2 Spike PoC Decision Tree

The spike checklist of detail doc §6.4 expressed as a decision tree.

```mermaid
flowchart TD
    START([Week 1 starts])
    HW["Acquire a board:<br/>AMD AI Series<br/>or Jetson Orin<br/>or both"]
    V4L2_TEST["V4L2 capture<br/>+ VIDIOC_EXPBUF<br/>smoke test"]
    V4L2_OK{"Success?"}
    V4L2_FAIL["Investigate camera<br/>compatibility, or<br/>baseline with USB UVC"]
    SCM_TEST["SCM_RIGHTS:<br/>deliver FDs to another process<br/>+ verify via mmap"]
    SCM_OK{"Success?"}
    SCM_FAIL["Verify the<br/>pidfd_getfd fallback path"]

    AMD_BRANCH{"AMD board?"}
    AMD_IMPORT["Try AMDXDNA external<br/>dma-buf import"]
    AMD_OK{"Production path<br/>in mainline?"}
    AMD_PATCH["Evaluate custom-patch cost<br/>vs. switching to Jetson"]
    PATCH_OK{"Patch cost<br/>acceptable?"}

    JETSON_BRANCH{"Jetson board?"}
    JETSON_IMPORT["NVMM →<br/>cuMemImportFromFd<br/>or EGLStream"]
    JETSON_OK{"Success?"}

    ZEROCOPY_VERIFY["eBPF verification:<br/>copy_to_user/copy_from_user<br/>= 0"]
    ZC_OK{"Verified?"}
    CYCLICTEST["1kHz cyclictest<br/>baseline"]

    DECIDE["Lock target platform<br/>+ enter formal design"]
    REVISIT["Revisit design doc<br/>(switch target<br/>or shrink scope)"]

    START --> HW --> V4L2_TEST --> V4L2_OK
    V4L2_OK -->|YES| SCM_TEST
    V4L2_OK -->|NO| V4L2_FAIL --> V4L2_TEST
    SCM_TEST --> SCM_OK
    SCM_OK -->|YES| AMD_BRANCH
    SCM_OK -->|NO| SCM_FAIL --> SCM_TEST

    AMD_BRANCH -->|YES| AMD_IMPORT --> AMD_OK
    AMD_BRANCH -->|NO| JETSON_BRANCH

    AMD_OK -->|YES| ZEROCOPY_VERIFY
    AMD_OK -->|NO| AMD_PATCH --> PATCH_OK
    PATCH_OK -->|YES| ZEROCOPY_VERIFY
    PATCH_OK -->|NO| JETSON_BRANCH

    JETSON_BRANCH -->|YES| JETSON_IMPORT --> JETSON_OK
    JETSON_BRANCH -->|NO| REVISIT

    JETSON_OK -->|YES| ZEROCOPY_VERIFY
    JETSON_OK -->|NO| REVISIT

    ZEROCOPY_VERIFY --> ZC_OK
    ZC_OK -->|YES| CYCLICTEST --> DECIDE
    ZC_OK -->|NO| REVISIT

    style DECIDE fill:#c8e6c9,stroke:#2e7d32,stroke-width:3px
    style REVISIT fill:#ffccbc,stroke:#bf360c,stroke-width:2px
```

**Priority of decisions**:
1. **If both can be tried in week 1, smoke-test both** — fast plate validation.
2. If AMD has a production path, prefer AMD (open ecosystem).
3. If AMD's patch cost is high and Jetson succeeds → switch to Jetson.
4. Both fail → revisit design doc (Intel iGPU? Mali? or shrink scope).

---

## Appendix. Mermaid Rendering Tips

### GitHub
- Embed as-is in READMEs, issues, and PRs. Native support.
- Narrow viewports may clip nodes — prefer `flowchart LR` (horizontal) when possible.

### Local SVG Extraction
```bash
# Install mermaid-cli (npm or bun)
npm i -g @mermaid-js/mermaid-cli
mmdc -i diagrams.md -o diagrams.svg -t dark
```

### VS Code
- Extension: *Markdown Preview Mermaid Support*
- Zoom and scroll work in the preview pane.

### PNG Extraction (for slides)
```bash
mmdc -i diagrams.md -o diagrams.png -w 2400 -H 1600 --backgroundColor white
```
