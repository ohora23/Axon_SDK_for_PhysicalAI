# axon vs ROS 2, in C++ (rclcpp), against ROS 2's own zero-copy path

The earlier benchmarks in [`benchmarks/`](../README.md) and [`benchmarks/mock/`](../mock/README.md)
compare axon with **rclpy** (Python) over default Fast DDS. Most of the gap they show is
Python, not the transport. This package redoes the comparison in C++ and adds ROS 2's
documented zero-copy path, so the numbers answer the question a ROS 2 user would ask.

## What is compared

| transport | what it is |
|---|---|
| `ros_dyn` | rclcpp, `std_msgs/UInt8MultiArray` (unbounded, so serialized and copied in transit) |
| `ros_loan` | rclcpp **loaned messages** on a fixed-size type (`msg/Payload*.msg`) + **Fast DDS data-sharing**: ROS 2's zero-copy path |
| `axon` | axon `TensorPublisher` / `TensorSubscriber`: Custom (memfd) pool on the host, Accelerator (CUDA VMM) pool on the GPU |
| `axon_wait` | (multi-stream only) `axon`, with the consumer blocking in `TensorSubscriber::wait_any` instead of a 1 kHz loop |
| `iox2`, zenoh, Cyclone | (host) iceoryx2, rmw_zenoh_cpp and rmw_cyclonedds_cpp + iceoryx; see [Payload size, iceoryx2 and the other RMWs](#payload-size-iceoryx2-and-the-other-rmws-host) |
| `rosidl_buffer`, `cudaipc` | (GPU only) the official ROS 2 `rosidl::Buffer` CUDA backend and ros2_cuda_ipc; see [GPU → GPU against the other GPU zero-copy transports](#gpu--gpu-against-the-other-gpu-zero-copy-transports) |

**Fast DDS profile.** Every ROS 2 cell runs with [`fastdds_zero_copy.xml`](fastdds_zero_copy.xml)
(`RMW_FASTRTPS_USE_QOS_FROM_XML=1`): synchronous publishing, data-sharing where the type allows it,
preallocated history, the same QoS as the code, and **16 MB shared-memory segments**. Without the
last one, `ros_dyn` loses camera frames (see the notes), so the binaries refuse to run `ros_dyn`
when an unbounded message is more than half the profile's `<segment_size>` (512 KB if unset):
they print the fix and exit 2 instead of reporting a silently lossy result
([`src/fastdds_check.h`](src/fastdds_check.h)). The runner records the data-sharing segments in
`/dev/shm` halfway through each `ros_loan` cell; every one had them.

Two scenarios:

- **host** (`gpu=0`): the producer has the payload in host memory; the consumer reads it in host memory.
- **GPU** (`gpu=1`): the producer has the payload on the GPU (an inference output) and the consumer needs it on the GPU.
  ROS 2 moves it D2H on the producer and H2D on the consumer; axon hands the consumer the same device buffer.

Same rules for every transport:

- One C++ binary, producer and consumer in separate processes, the same clock (`CLOCK_MONOTONIC_RAW`).
- The producer writes the **whole payload** every frame, taking `t0` just before the write. Latency is `t0` → the consumer has the frame where it needs it.
- The consumer **busy-polls** (`take` / `rcl_take_loaned_message` / `latest_view`), so no transport pays for a wakeup. `ros_loan` refuses to run if the subscription cannot take loans.
- GPU copies are followed by `cuCtxSynchronize()` before a timestamp or a publish, so no copy is still in flight when the clock stops. The axon GPU consumer reads the 16 B header and 8 B tail back from the device to check the frame; that readback is most of axon's flat ~25 µs.
- Header and tail both carry the seqno; a mismatch counts as `torn`. Every cell below had 0 torn frames.
- QoS keep-last(1), best-effort; axon's ring is 4 buffers deep (`AXON_BENCH_NBUF`, see the notes).
- 500 frames at 100 Hz, the first 20 dropped as warm-up, 3 runs per cell.

## Single stream: latency

Ryzen 7 9800X3D, RTX 5080, Ubuntu 24.04 (non-RT kernel), ROS 2 Jazzy, Fast DDS 2.14.
p50 latency in µs, median of 3 runs (the three runs in brackets); raw data in `results/rep*.jsonl`.

**Host → host**

| payload | `ros_dyn` | `ros_loan` (zero-copy) | `axon` | axon vs `ros_loan` |
|---|---|---|---|---|
| 147 KiB | 42 (40/42/44) | 25 (26/25/24) | **4** (4/4/4) | 6× |
| 1 MiB | 133 (133/130/133) | 36 (36/36/35) | **15** (15/15/15) | 2.4× |
| 4 MiB | 455 (464/455/453) | 80 (81/80/80) | **59** (60/59/59) | 1.4× |

**GPU → GPU**

| payload | `ros_dyn` | `ros_loan` (zero-copy) | `axon` | axon vs `ros_loan` |
|---|---|---|---|---|
| 147 KiB | 67 (66/67/69) | 50 (47/50/50) | **24** (25/24/24) | 2.1× |
| 1 MiB | 288 (288/289/286) | 171 (173/171/170) | **25** (27/24/25) | 6.8× |
| 4 MiB | 842 (842/836/842) | 485 (489/481/485) | **26** (27/26/26) | 19× |

p99 stays close to p50: in every single run, p99 is within 2.5× of p50. Every transport delivered all 500 frames
in every cell.

- **On the host, ROS 2's zero-copy path is close to axon.** Both copy the payload once, into shared memory, on the producer. What is left is a fixed cost per message of about 20 µs in the DDS machinery, so axon's lead shrinks from 6× at 147 KiB to 1.4× at 4 MiB. Against `ros_dyn`, which copies the payload in transit, axon is 7.7-10.5× ahead.
- **On the GPU, the gap is the two copies ROS 2 cannot avoid.** A ROS 2 message lives in host memory, so a GPU payload goes D2H, through the transport, then H2D: 485 µs at 4 MiB. axon passes the device buffer itself, and its latency stays at 24-26 µs whatever the size.
  The two copies here are pageable. Measured separately on this machine, a 4 MiB D2H + H2D pair takes 408 µs pageable and 230 µs pinned, so pinning the ROS 2 buffers would bring the 4 MiB cell to roughly 310 µs, still about 12× axon. Not measured inside the benchmark: the loan lives in the Fast DDS segment, and registering it with CUDA is not something a ROS 2 user would normally do.
- **Not measured here:** NITROS / type adaptation, which keeps GPU buffers inside one process (composable nodes), and `rmw_zenoh` shared memory.

## GPU → GPU against the other GPU zero-copy transports

Two ROS 2 projects now share GPU memory between processes the way axon does (CUDA VMM
allocations whose FDs cross a Unix socket), with only a descriptor on the topic:

| transport | what it is |
|---|---|
| `rosidl_buffer` | the **official** ROS 2 Lyrical `rosidl::Buffer` CUDA backend ([ros2/rosidl_buffer_backends](https://github.com/ros2/rosidl_buffer_backends) `cuda_buffer_backend` 0.1.2): a `sensor_msgs/Image` whose `data` lives in CUDA VMM; the publisher's `WriteHandle` records a CUDA IPC event, and the subscriber's `ReadHandle` waits on it |
| `cudaipc` | [dskkato/ros2_cuda_ipc](https://github.com/dskkato/ros2_cuda_ipc) v0.5 (MIT): the same design as a standalone library over Jazzy (`GpuImage` descriptor, a CUDA IPC event for completion, and a buffer reference held by each reader's `ReadHandle`) |

Same GPU → GPU cell as above: the producer has the payload on the GPU, writes all of it plus
the header and tail into the transport's buffer, the consumer readback stops the clock.
Each transport is used the way its documentation shows. Both completion events and
reader-held buffers are honoured, and nothing is host-synchronized that the library does
not ask for.
`rosidl_buffer` runs in the Lyrical container ([`lyrical/`](lyrical/)), with these settings:

- Ubuntu 26.04 and Fast DDS 3.6.2, against Jazzy's 2.14 on the host.
- The same `fastdds_zero_copy.xml`.
- `acceptable_buffer_backends="cuda"`.

Every frame was checked to have arrived on the `cuda` backend (`get_backend_type()`), with 0
CPU fallbacks. 500 frames at 100 Hz, 3 runs, p50 µs (median). The GPU runs in its default
mode first, where each process has its own GPU context, then with **CUDA MPS** (one shared
context; see [Why the gap shrinks with MPS](#why-the-gap-shrinks-with-mps)):

| payload | ROS 2 loaned + data-sharing | `rosidl_buffer` (official) | `cudaipc` | **axon** |
|---|---|---|---|---|
| 147 KiB | 46 | 123 | 124 | **24** |
| 1 MiB | 162 | 124 | 124 | **24** |
| 4 MiB | 484 | 124 | 127 | **26** |
| *with MPS:* 147 KiB | 41 | 42 | 39 | **24** |
| *with MPS:* 1 MiB | 169 | 42 | 37 | **24** |
| *with MPS:* 4 MiB | 479 | 42 | 35 | **25** |

All cells delivered 500/500 frames with 0 torn. **Without MPS, axon is about 5× faster than the
GPU-sharing ROS 2 transports; with MPS, about 1.5×.**

- **p99:** for the three GPU-sharing transports, the median p99 is 10-17 µs above p50. The
  worst single run was `rosidl_buffer` at 4 MiB, with a p99 of 187 µs.
- **When and how:** measured 2026-10-19 with 10 of 16 CPUs online (see the note on this PC
  below). The `rosidl_buffer` cell ran in its container right after the host session.
- **Raw data:** `results/cudaipc_rep.jsonl` holds cudaipc, axon and ros_loan from one
  session, with per-stage timings, a host-synced cudaipc pass and the probe.
  `results/rosidl_buffer_rep.jsonl` holds the container run, with its own probe and versions.
  The MPS runs are `results/cudaipc_mps_rep.jsonl` and `results/rosidl_buffer_mps_rep.jsonl`
  (`"mps":1` in the meta line), measured 2026-10-19 on the same 10 CPUs. They come from
  `run_cudaipc.sh` and the container run under MPS (commands below), with the meta line tagged.
  `results/sync_cost_mps_rep.jsonl` is `sync_cost ipcwait` under MPS.

**Where the ~100 µs goes.** `rosidl_buffer` and `cudaipc` both share the buffer and stay flat
in size, beating ROS 2's host-memory zero-copy above 1 MiB. Per stage for `cudaipc`, median
over the three sizes, recorded in the jsonl:

- **t0 until the descriptor arrives over DDS:** 24-32 µs. This includes the producer
  queueing its GPU copies.
- **`read()`:** 2 µs. The import is cached.
- **CUDA IPC event wait, 24 B readback and stream sync:** 90-106 µs.

Two checks isolate the event wait:

- **Producer host-synced first:** with the event already complete, the last stage still
  takes 65 µs.
- **[`src/ipc_event_probe.cpp`](src/ipc_event_probe.cpp), no library:** waiting on an
  already-completed CUDA IPC event from another process, plus the stream sync, costs
  **61-72 µs p50** across runs. An empty stream costs 0.3 µs. The value is the same on the
  host and inside the Lyrical container.

### Why the gap shrinks with MPS

Most likely, that 60-70 µs is not the event itself but the GPU switching from the producer's
context to the consumer's. [`src/sync_cost.cu`](src/sync_cost.cu) `ipcwait` measures it on
the GPU clock (`%globaltimer`, which ticks in 256 ns steps on this GPU, hence the repeated
values). The interval runs from the end of a 300 µs producer kernel to the start of the
consumer's next kernel, in another process. 3 runs, p50 µs:

| handoff | default (a context per process) | with MPS |
|---|---|---|
| consumer `cudaStreamWaitEvent` on the producer's IPC event, enqueued while it is still pending | 58.9 | 5.6 |
| no event (axon's pattern, stand-in): producer host-syncs, tells the consumer over a pipe, the consumer launches | 62-72 | 8.5-17.4 |
| the same two streams in **one** process, normal event / host-sync | 1.3 / 4.9 | (same) |

The consumer's `cudaStreamWaitEvent` call returns in 1.2-1.6 µs, so a consumer can wait on the
GPU without blocking its thread. But without MPS the GPU still needs about 60 µs before the
consumer's work runs, with or without an event. MPS also changes how IPC events are handled,
so this does not prove the event costs nothing, but the event-free row pays the same. In the
cudaipc cell with MPS, the event wait, readback and sync stage falls from 90-106 µs to 5 µs. What remains is the 28-33 µs of the
descriptor travelling over DDS, which axon does not pay.

This changes what the headline means:

- **axon's 24-26 µs is the time to the consumer's 24 B readback, a copy.** The copy does not pay
  the context switch, with or without MPS. A consumer that *launches a kernel* on the frame
  pays it with axon too; see [When the consumer runs a kernel](#when-the-consumer-runs-a-kernel).
- **With MPS, the GPU-sharing ROS 2 transports land at 35-56 µs** and axon at 24-29 µs, across
  the #47/#48 sessions and this one (here: 38-56 against 27-29).
  The remaining difference is the DDS descriptor.
- MPS is not the default. It runs one server per user, so containers must run as the same
  user and share its pipe directory. Every client shares one GPU context, so a fault in one
  client hits the others. Use it where that isolation trade-off is acceptable, which is often
  the case on a dedicated robot computer.

```bash
# MPS as a normal user (no sudo); run the benchmarks in the same shell, then `echo quit | nvidia-cuda-mps-control`
export CUDA_MPS_PIPE_DIRECTORY=$HOME/mps-pipe CUDA_MPS_LOG_DIRECTORY=$HOME/mps-log
mkdir -p $CUDA_MPS_PIPE_DIRECTORY $CUDA_MPS_LOG_DIRECTORY && nvidia-cuda-mps-control -d
# the Lyrical container then needs: --user "$(id -u):$(id -g)" -v $CUDA_MPS_PIPE_DIRECTORY:/mps-pipe -e CUDA_MPS_PIPE_DIRECTORY=/mps-pipe
```

### When the consumer runs a kernel

With `BENCH_CONSUMER_KERNEL=1`, the consumer reads the 24 B with one tiny kernel on the consumer's
stream instead of two DtoH copies ([`src/gpu_read_kernel.h`](src/gpu_read_kernel.h), inline
PTX, results written to mapped host memory). For the event-based transports, that kernel is
queued behind the event wait on the same stream. axon, ROS 2 loaned and intra-process launch it on
the legacy default stream and wait with `cuCtxSynchronize`. This is what a consumer that runs inference
on the frame pays first. Every cell was measured in one session, with and without MPS, 3 runs,
500/500 delivered, 0 torn, 0 CPU fallbacks. p50 µs (median of 3) at 1 MiB. At 147 KiB and 4 MiB
every row is within ±7 µs of it, except ROS 2's host-memory path, which grows with size
(45-52 µs at 147 KiB, 481-505 µs at 4 MiB):

| transport | copy, default | **kernel, default** | copy, MPS | **kernel, MPS** |
|---|---|---|---|---|
| **axon** | 24 | **129** | 27 | **23** |
| `cudaipc` | 124 | 122 | 41 | 44 |
| `rosidl_buffer` | 126 | 124 | 56 | 49 |
| ROS 2 loaned + data-sharing (host memory) | 167 | 176 | 167 | 178 |
| ROS 2 intra-process, same process (`ros_intra_poll`) | 29 | 25 | 37 | 32 |

- **Without MPS, a kernel-launching axon consumer is as slow as the event-based transports:**
  129 µs against 122-124 µs. The ~100 µs switch between the producer's and consumer's GPU
  contexts is paid by whoever touches the frame with a kernel first. The event designs paid it
  inside their event wait, so their kernel cells barely change. axon's copy-only cell never
  paid it.
- **With MPS, axon's lead holds for kernel consumers:** 22-26 µs against 41-51 µs, 1.6-2.2×
  depending on size.
- **Inside one process there is no context switch.** The intra-process kernel cell is 25 µs
  without MPS. MPS adds 3-8 µs there, and to axon's copy cell.
- **p99:** axon's kernel cell without MPS is 140-142 µs. With MPS, axon's kernel cell is 34-37 µs,
  against 68-86 µs for the GPU-sharing transports. A short 100-frame smoke run once showed a
  1 ms p99 for axon without MPS; the gated runs did not.

So for GPU consumers that run kernels, axon's GPU advantage over the GPU-sharing ROS 2
transports **needs MPS**. Without MPS, it remains only for consumers that copy results out, or
against ROS 2's host-memory path.

Raw data: `results/consumer_kernel_rep.jsonl` (`"mps"` and `"consumer_kernel"` on every consumer line),
from `run_gated.sh gpu`.

### The producer's host-sync

axon avoids both costs in this cell. Its descriptor goes through a shared-memory seqlock, not
DDS, and its producer **host-synchronizes before publishing**, so the consumer never waits on
an event. The producer thread blocks until its GPU work is done. `sync_cost pipeline`
measures what that costs a producer that runs back to back: CPU work (`C`, e.g. preparing the
next input), a GPU kernel (`K`, the "inference"), then a 1 MiB copy into a 4-deep ring. Three
producers:

- `sync_naive`: enqueue frame *i*, `cudaStreamSynchronize`, publish *i*, then the CPU work for *i+1*.
- `sync_pipelined`: CPU work, enqueue *i*, `cudaEventSynchronize` on *i−1*, publish *i−1*. Still
  axon's rule (only complete frames are published), but the GPU always has the next frame queued.
- `event`: CPU work, enqueue *i*, record an event, publish *i* at once, as the event-based
  transports do; the producer waits only for the ring slot it is about to overwrite.

GPU start and end of every frame are stamped with `%globaltimer`. 2000 frames (400 for
K ≥ 1 ms), 3 runs, median:

| K µs | C µs | `sync_naive` frames/s (GPU busy) | `sync_pipelined` | `event` |
|---|---|---|---|---|
| 50 | 0 | 17,627 (91.5 %) | 18,973 (98.5 %) | 18,836 (97.8 %) |
| 50 | 50 | 9,346 (48.5 %) | 18,277 (94.9 %) | 18,301 (95.1 %) |
| 200 | 0 | 4,837 (97.7 %) | 4,934 (99.6 %) | 4,924 (99.4 %) |
| 200 | 200 | 2,457 (49.6 %) | 4,881 (98.6 %) | 4,883 (98.6 %) |
| 1000 | 1000 | 498 (50.0 %) | 993 (99.7 %) | 993 (99.7 %) |
| 5000 | 0 | 200 (99.9 %) | 200 (100 %) | 200 (100 %) |

- **A naive host-sync idles the GPU about 5 µs per frame** (the wake-up and the next launch). That
  costs 7 % of throughput at a 50 µs kernel and nothing measurable from 1 ms up.
- **It halves throughput when the producer also has CPU work**, because the CPU and GPU stages
  then run one after the other.
- **Pipelining the sync removes the loss**: `sync_pipelined` matches `event` in every row. The
  price is latency. Frame *i−1* is published after the CPU work for frame *i*, so up to
  `max(0, C − K)` later than it completed, plus the wake-up.
- **With axon's API, fill the descriptor right before each `publish()`.** The pipelined
  producer above uses no transport. `acquire_descriptor()` returns one shared staging
  descriptor and clears it on every call. So a producer that acquires frame *i* before
  publishing frame *i−1* must fill frame *i−1*'s fields after that acquire. The buffer index
  stays in each `AcquiredDescriptor`. This pattern was not run through axon here.

So the host-sync is a constraint on how the producer is written, not a throughput ceiling.
The event-based designs still let the producer run ahead without that restructuring. They
also keep a frame alive while a consumer holds it, where axon is latest-value-wins.
ros2_cuda_ipc additionally does not reuse a block for 100 ms after publishing it, so it needs
`rate × 0.1 s` blocks: 16 here, against axon's 4.

```bash
benchmarks/ros2_cpp/run_sync_cost.sh 2000 3    # producer host-sync cost + ipcwait; refuses if another process uses the GPU
benchmarks/ros2_cpp/run_cudaipc.sh 500 100 3   # cudaipc, axon, ros_loan; needs ros2_cuda_ipc built
docker build -t axon-bench-lyrical -f benchmarks/ros2_cpp/lyrical/Dockerfile benchmarks/ros2_cpp
docker run --rm --runtime=nvidia -e NVIDIA_VISIBLE_DEVICES=all -e NVIDIA_DRIVER_CAPABILITIES=compute,utility \
    --ipc=host --pid=host axon-bench-lyrical  # rosidl::Buffer, plus the IPC event probe
```

ros2_cuda_ipc is built separately (`git clone https://github.com/dskkato/ros2_cuda_ipc &&
colcon build`) and sourced before this package; `cudaipc_bench` is only built when it is found.
The Lyrical image satisfies the backend's `libnvidia-compute-595` dependency with an empty
package and uses the host driver mounted by the NVIDIA runtime.

## Payload size, iceoryx2 and the other RMWs (host)

The same single-stream cell (busy-polling consumer, payload written in full, 3 runs, median
p50 µs) from 64 B to 64 MiB. 64 B and 8640 B run at 1 kHz for 2000 frames, 147 KiB-4 MiB at
100 Hz for 500, and 16 and 64 MiB at 30 Hz for 300. The cells add:

- **`iox2`:** Eclipse iceoryx2 v0.10.0, C++ bindings ([`src/iox2_bench.cpp`](src/iox2_bench.cpp)).
  A zero-copy slice loaned from shared memory; subscriber buffer 1 with safe overflow, the same
  latest-value delivery as the others.
- **rmw_zenoh_cpp 0.2.11** with its shared memory switched on ([`zenoh_shm_session.json5`](zenoh_shm_session.json5);
  ROS ships it off). It has no loaned messages (`can_loan_messages=0`), so it runs the
  `UInt8MultiArray` cell, which is serialized into shared memory. One cell at 1 MiB has SHM off,
  for comparison.
- **rmw_cyclonedds_cpp 2.2.4 + iceoryx** ([`cyclonedds_shm.xml`](cyclonedds_shm.xml), RouDi with
  [`iox_roudi_mempools.toml`](iox_roudi_mempools.toml)): loaned messages of the fixed-size types.
  Without iceoryx, Cyclone refuses the loans (`can_loan_messages=0`), so these cells did go
  through it.

| payload | **axon** | `iox2` | Fast DDS loaned + data-sharing | Cyclone + iceoryx, loaned | zenoh SHM, serialized |
|---|---|---|---|---|---|
| 64 B | **0.3** | 0.6 | 6.3 | 4.7 | 17.4 |
| 8640 B | **0.4** | 0.6 | 5.8 | 4.8 | 18.6 |
| 147 KiB | **4.3** | 5.2 | 22.4 | 17.4 | 63.0 |
| 1 MiB | **15.4** | 16.5 | 33.9 | 30.9 | 125.5 (SHM off: 242) |
| 4 MiB | 59.2 | 59.3 | 75.4 | 73.8 | 344.9 |
| 16 MiB | 474 | **238** | 304 | not run¹ | 1495 |
| 64 MiB | 3271 | **2348** | 3050 | not run¹ | not run |

¹ The RouDi mempools in `iox_roudi_mempools.toml` stop at 4 MiB chunks. The axon, `iox2` and
Fast DDS columns come from the `sizes` session, Cyclone and zenoh from the `rmw` session. In the
`rmw` session, Fast DDS loaned (34.4 µs) and axon (15.3 µs) at 1 MiB match the `sizes` session.

Every cell delivered every frame with 0 torn, except zenoh: two runs missed frame 1 (setup), and
one 8640 B run lost one frame at 1 kHz. The serialized Fast DDS cell (`UInt8MultiArray`) is
125 µs at 1 MiB, the same as zenoh with SHM.

- **Up to 4 MiB, iceoryx2 is as fast as axon on the host.** It is within about 1 µs of axon from 64 B to 4 MiB.
  The loaned ROS 2 paths (Fast DDS, Cyclone + iceoryx) add 4-19 µs per message. axon's
  host advantage over a plain shared-memory transport is therefore not latency. It is what sits
  around the transport: dma-buf/V4L2 and CUDA buffer pools, the GPU handoff, and RT
  latest-value semantics.
- **From 16 MiB, axon (4 buffers) is the slowest of the zero-copy paths on the host.** In this
  table, 16 MiB gives axon 474, iceoryx2 238 and Fast DDS loaned 304 µs; the clock note below
  explains part of the spread. With the clock held: axon 373 against iceoryx2's 219. The time
  grows with axon's ring depth, and the counters below show why: at 16 MiB the producer's write
  runs out of cache. A shallower ring
  makes a torn frame more likely for a slow reader, though
  (see [docs/usage.md](../../docs/usage.md) on ring sizing).

**Why ring depth costs at 16 MiB** ([`run_ring_perf.sh`](run_ring_perf.sh)). The counters are taken
on the producer process. It writes the whole payload, and that write is most of the latency.
30 Hz, 300 frames, 3 runs, median per frame, counted from 1.5 s in (after setup). The producer
is pinned to CPU 6 and the consumer to CPU 7, two different physical cores. PM QoS 0 is held
(no deep C-states); the clock section below explains why. A payload is 65 K cache lines at
4 MiB, 262 K at 16 MiB and 1 M at 64 MiB:

| payload | producer | p50 µs (3 runs) | lines filled from DRAM | from L3 | L1 dTLB misses | cycles |
|---|---|---|---|---|---|---|
| 4 MiB | axon, 1 / 2 / 4 buffers | 57 / 57 / 58 | 0.1-0.2 K | 9-13 K | 2.1-2.2 K | 0.33-0.36 M |
| 4 MiB | iceoryx2 | 62 | 0.0 K | 15 K | 2.2 K | 0.35 M |
| 16 MiB | axon, 1 buffer | 220 (218-223) | 1.2 K (0.5 %) | 12 K | 8.3 K | 1.24 M |
| 16 MiB | axon, 2 buffers | 230 (226-231) | 3.5 K (1.3 %) | 8 K | 8.3 K | 1.31 M |
| 16 MiB | axon, 4 buffers | **373** (355-374) | **38.7 K (14.8 %)** | 9 K | 8.3 K | 2.12 M |
| 16 MiB | iceoryx2 | 219 (218-220) | 0.4 K (0.1 %) | 9 K | 8.3 K | 1.23 M |
| 64 MiB | axon, 1 buffer | **2155** (2153-2177) | 276 K (26 %) | 18 K | 32.9 K | 11.5 M |
| 64 MiB | axon, 2 / 4 buffers | 3605 / 3319 | 348 / 441 K (33 / 42 %) | 256 / 205 K | 30-32 K | 18.0-18.6 M |
| 64 MiB | iceoryx2 | **2210** (2182-2281) | 261 K (25 %) | 54 K | 32.9 K | 11.8 M |

- **At 16 MiB, ring depth costs DRAM traffic.** With 4 buffers, 15 % of each frame's lines come
  from DRAM, against 0.5-1.3 % with 1-2. Cycles and latency follow: 1.6-1.7× slower.
  - dTLB misses are the same in every row. ~8.3 K per 16 MiB frame matches 4 KiB pages for
    the source and the destination (inferred, not checked directly).
  - Page faults are about 0 per frame (0.1 for axon, 0.0 for iceoryx2).
  - So the cost is consistent with cache capacity, not paging or the TLB.
  - axon with one buffer matches iceoryx2, whose working set is presumably a few samples (not
    measured). With 2 buffers, axon costs about 5 % more than with 1.
- **At 64 MiB, the payload no longer fits whatever the depth** (25-42 % from DRAM). A 1-buffer
  ring is 2.5 % faster than iceoryx2.
- **An earlier version of this section reported a "7 % unexplained" gap at 64 MiB.** It did
  not reproduce in three later runs. Two unpinned runs had axon and iceoryx2 within 3 %: 2100 vs
  2099 µs (`ring_perf_second_rep.jsonl`) and 2067 vs 2120 µs (`ring_perf_unpinned_rep.jsonl`).
  The pinned run above puts axon 2.5 % ahead. In that first run (`ring_perf_first_rep.jsonl`)
  axon also spent about 7 % more cycles per frame, so it was not the clock effect described
  below. It was variation between sessions, and its cause was not established.
- **4 KiB aliasing between the source and destination made no measurable difference at 64 MiB**
  (within about 5 % noise, 1 buffer, unpinned). axon's destination is
  page-aligned and 16 B from the source's page offset, iceoryx2's is 168 B. Moving the source
  (`BENCH_SRC_OFFSET`) to any of four offsets left the 64 MiB p50 within run-to-run noise, for
  both (`results/src_offset_probe.txt`).
- In practice, a host payload of several MiB or more wants the shallowest ring its readers
  allow; 2 buffers is the cheap middle ground. The benchmark keeps 4 for every size, to keep
  one rule.

**The producer's clock decides low-rate host numbers** ([`run_clock_probe.sh`](run_clock_probe.sh),
`results/clock_probe.txt`, which holds the second of the two sessions below). At 30 Hz, the producer's core idles between frames. Unpinned, the
same cell came out at 55 or 105 µs (4 MiB) and 212 or 416 µs (16 MiB) from run to run. In those
runs, cycles per frame did not change, context switches were 1 per frame (the sleep), and
migrations about 0 (`ring_perf_unpinned_rep.jsonl`, `ring_perf_pinned_noqos_rep.jsonl`). The clock
did change. Average clock (cycles / task-clock) of the 16 MiB, 1-buffer producer, two sessions:

| producer placement | p50 µs | clock |
|---|---|---|
| on the SMT sibling of the busy-polling consumer's core | 222-302 | 5.1 GHz |
| on its own idle core | 416-428 | **2.8-3.3 GHz** |
| on its own core, PM QoS 0 | 219-299 | 5.2 GHz |

A producer that wakes on an idle core runs its write at about half the clock. Unpinned, the
scheduler presumably sometimes put it on a core that was already busy, which would explain why
runs varied; the CPU it ran on was not logged. The clock explains the 2× split, not all of the
spread. At the same 5.1-5.2 GHz, the second probe session ran up to ~35 % slower than the first
(250-302 vs 219-270 µs).

**That residual tracks how much of the payload the producer refills from DRAM**
([`run_spread_probe.sh`](run_spread_probe.sh), `results/spread_probe.jsonl`, `results/spread_dose.jsonl`).
At 1 buffer, the 16 MiB source and destination (32 MiB) fit in the 96 MB L3, but at 30 Hz they sit
idle for 33 ms between frames, and whatever else runs on the machine can evict them. Measured on a
quiet machine right after a reboot, producer pinned to CPU 6 with `CpuLatencyHold` 1 µs, consumer
on CPU 7, conditions interleaved per round:

| condition | p50 µs | producer cycles / frame | DRAM fills / frame |
|---|---|---|---|
| 300 frames, nothing else running (15 of 16 runs) | 211-217 | 1.13-1.22M | 76-5,572 |
| 150 frames (6 runs) | 211-218 | 1.18-1.27M | 198-6,452 |
| + another process copying 16 MiB every 33 ms (2) | 295-301 | 1.43-1.44M | 12-15k |
| + the same with 32 MiB (2) | 432-435 | 2.0-2.1M | 55-61k |
| + the same with 48 MiB (2) | 515-528 | 2.3M | 83-86k |
| + 96 MiB copied every 5 ms (6) | 754-782 | 3.9-4.3M | 258-308k |

The frame count does not matter. Across all 38 runs, p50 follows DRAM fills per frame (r = 0.98;
below 100k fills, about 209 µs + 3.8 µs per 1,000 fills, r = 0.99). One baseline run, with no
thrasher, came out at 332 µs with 34k fills: something else on the machine did it. A cache
neighbour of a few tens of MiB, such as another session's workload or a browser, is enough to
produce the earlier sessions' 250-300 µs. Those sessions did not log DRAM fills, so this is the
mechanism they are consistent with, not a measurement of them. For a host producer at a low
rate, the payload's cache residency is part of the latency, and a shared machine moves it. The
probe uses 150 frames, the ring runs 300. The size
table above was measured unpinned with default C-states, so its 16 and 64 MiB host cells carry
this noise. The 16 MiB ranking holds in the pinned run: axon (4 buffers) 373 µs against
iceoryx2's 219. For stable low-rate numbers, pin the producer and hold PM QoS 0, as an RT
robot computer would.

The SDK now provides this as `axon::CpuLatencyHold` (`RtSetupConfig::cpu_latency_us`,
Python `axon.cpu_latency_hold`; see [docs/usage.md](../../docs/usage.md)). bench's producer
takes it as `BENCH_CPU_LATENCY_US` / `BENCH_CPU_LATENCY_CPU`. Through it, with the producer
pinned to CPU 6 (`results/cpu_latency_hold_probe.txt`, 2 runs each):

| limit | p50 | clock |
|---|---|---|
| none | 397-424 µs | 3.1-3.5 GHz |
| 20 µs (C2 allowed), system-wide | 418-428 µs | 3.0-3.6 GHz |
| **1 µs (C1), CPU 6 only** | **241-242 µs** | **4.9-5.0 GHz** |
| 1 µs, system-wide | 254-271 µs | 4.6-4.9 GHz |
| 0 µs (polling), CPU 6 only / system-wide | 234-247 / 235-260 µs | 5.2 GHz |

C2 (18 µs exit latency) is already deep enough to cost the clock. A 1 µs limit on the
producer's core alone keeps it, and the other cores keep their deep idle states. This is one
machine with 2 runs per row. The per-CPU rows come from the second session only, and the
system-wide rows from both, so treat small differences between them as noise.

Raw data: `results/ring_perf_rep.jsonl` (pinned, PM QoS 0; `PIN=6,7 PMQOS=1 run_ring_perf.sh`),
`results/ring_perf_unpinned_rep.jsonl`, `results/ring_perf_pinned_noqos_rep.jsonl`,
`results/ring_perf_second_rep.jsonl`, `results/ring_perf_first_rep.jsonl`. All need `kernel.perf_event_paranoid` ≤ 1.
- **zenoh's shared memory halves its latency** (242 → 125 µs at 1 MiB), but without loans it
  stays a serialized path.

**GPU → GPU at 16 and 64 MiB** (axon vs Fast DDS loaned, 30 Hz): axon 36 / 184 µs, ROS 2
1769 / 6959 µs. axon's time now includes the producer's own 16-64 MiB device copy.

Raw data: `results/sizes_rep.jsonl` (`run_gated.sh sizes`) and `results/rmw_rep.jsonl`
(`run_gated.sh rmw`, tagged with `rmw`). Both were measured on 2026-10-20 with 10 CPUs online.

## Multi-stream: CPU, latency, delivery and cache misses

`mstream` runs the [`benchmarks/mock`](../mock/README.md) robot workload in C++: four 640×480×2 camera/depth streams at
30 Hz (×`scale`), a 2D LiDAR, IMU, odometry and `cmd_vel`; scale 1 offers 74 MB/s, scale 4 297 MB/s. One producer
process writes every payload in full (one thread, earliest deadline first). Consumers:

- `ros_dyn`, `ros_loan`: an event-driven `SingleThreadedExecutor` (`spin_once` blocks until work arrives).
- `axon`: axon's RT model, a 1 kHz loop that reads the latest of every stream.
- `axon_wait`: blocks in `TensorSubscriber::wait_any` over all streams, then takes every stream that changed.

How it is measured:

- **Window:** the consumer measures 5 s from the first frame it sees; the producer sends for 6.5 s. Setup, discovery
  and idle waits are outside both windows.
- **CPU:** `getrusage` over each window, producer plus consumer (all threads, including Fast DDS's).
- **Cache misses and instructions:** `perf_event_open` on every thread of each process, enabled only inside the same
  windows (`PERF=1`), reported per second of window.
- **Delivery:** distinct frames seen / the seqno span inside the window, per stream; the table shows the worst stream.
- Consumers read the 24 bytes they check (header and tail), not the whole frame. CPU frequency and core placement
  were not pinned.

3 runs, median:

| scale (MB/s) | cores: `ros_dyn` / `ros_loan` / `axon` / `axon_wait` | camera p50 µs (same order) | camera p99 µs (same order) |
|---|---|---|---|
| 1 (74) | 0.038 / 0.031 / **0.007** / 0.012 | 148 / 76 / 650 / **30** | 514 / 296 / 1010 / **135** |
| 2 (148) | 0.055 / 0.033 / **0.009** / 0.013 | 138 / 54 / 618 / **29** | 484 / 303 / 1007 / **124** |
| 3 (222) | 0.071 / 0.043 / **0.011** / 0.018 | 132 / 52 / 586 / **29** | 392 / 258 / 1004 / **118** |
| 4 (297) | 0.077 / 0.038 / **0.015** / 0.019 | 103 / 44 / 557 / **29** | 334 / 235 / 1017 / **124** |

Cache misses / instructions per second of window, producer + consumer:

| scale (MB/s) | `ros_dyn` | `ros_loan` | `axon` | `axon_wait` |
|---|---|---|---|---|
| 1 (74) | 4.98 M / 0.068 B | 2.77 M / 0.047 B | **0.36 M / 0.015 B** | 0.84 M / 0.012 B |
| 4 (297) | 15.57 M / 0.219 B | 3.66 M / 0.127 B | **0.87 M / 0.024 B** | 1.31 M / 0.040 B |

Every transport delivered every frame of every camera stream in every run, with 0 torn frames. The only losses were
in `axon_wait` at scales 2 and 3: the 200 Hz IMU stream lost 0.1-0.2 % of its frames (worst stream 0.998-0.999),
because when it published twice while the consumer was busy with a burst of camera frames, the consumer took only the
newer one. That is axon's latest-value-wins delivery, by design.

What this says:

- **In C++ every transport is cheap.** At 297 MB/s, ROS 2's zero-copy path costs 0.038 cores and axon 0.015-0.019.
  The rclpy figures in [`benchmarks/mock`](../mock/README.md) (0.98 vs 0.30 cores) are almost all Python.
- **With `wait_any`, axon has the lowest latency and still about half the CPU of ROS 2's zero-copy path.** Camera
  p50 is 29 µs against 44 µs and p99 124 µs against 235 µs at 297 MB/s, for 0.019 against 0.038 cores and 2.8× fewer
  cache misses. Waking up costs CPU: `axon_wait` uses 27-71 % more than axon's 1 kHz loop.
- **The 1 kHz loop is the cheapest model but waits for its tick.** A frame waits on average half a tick, so camera
  p50 is 550-650 µs. That is the RT-loop model, not the transport: an RT control loop runs at a fixed rate anyway.
- **None of the transports is flat in bandwidth:** from scale 1 to 4, axon goes from 0.007 to 0.015 cores (the
  producer's payload writes), `ros_loan` from 0.031 to 0.038, `ros_dyn` from 0.038 to 0.077. Run-to-run spread for the
  ROS 2 cells is up to ±30 %.
- **Not covered:** consumers that read the whole frame, the EventsExecutor. Intra-process ROS 2 and 1 → N fan-out are in the sections below.

```bash
benchmarks/ros2_cpp/run_mstream.sh                 # 5 s per cell, scales 1-4
PERF=1 benchmarks/ros2_cpp/run_mstream.sh          # + cache misses and instructions (perf_event_paranoid <= 1)
```

`MSTREAM_DEBUG=1` prints per-stream counts from the consumer; `MSTREAM_STAGGER=1` spreads the first deadlines.
Raw data: `results/ms_rep{1,2,3}.jsonl`.

## Same process: ROS 2 intra-process against axon across processes

Many ROS 2 systems avoid copies by putting the nodes in one process: a component container
with intra-process communication. [`src/intra.cpp`](src/intra.cpp) measures that path. Two
nodes run in one process with `use_intra_process_comms(true)`, and the consumer runs in a
`SingleThreadedExecutor` on its own thread. A component container uses the same intra-process
manager.

- **Host:** every frame the producer allocates a `std::unique_ptr<Payload*>` with
  `MessageInitialization::SKIP`, writes the payload once and publishes it. `t0` is taken
  before the allocation, because a `unique_ptr` publish needs a fresh message every frame. The
  single subscriber takes a `unique_ptr`, so ownership moves. Every frame was checked to arrive
  at the address it was published from (`copied` = 0).
- **GPU:** a REP-2007 type adapter (`GpuFrame{CUdeviceptr, bytes}` adapted to
  `UInt8MultiArray`) carries only the device pointer. The producer host-syncs like axon's, and
  the consumer reads the same 24 B back. The ROS conversion never ran (`converted` = 0).
- `ros_intra_poll` loops on `spin_some(0)`; `ros_intra_wait` blocks in `spin()`. Otherwise the
  same rules as above apply: 500 frames at 100 Hz, 3 runs, 500/500 delivered, 0 torn.

p50 µs, median of 3. "PM QoS 0" is explained below. axon runs across two processes. Its
busy-polling consumer (the default of `bench.cpp`) never idles, so it has one column. Its
blocking consumer is the N = 1 cell of the fan-out tables, measured in the same session as
the PM QoS columns:

| payload | axon, spinning | `ros_intra_poll` | `ros_intra_wait` | `ros_intra_poll`, PM QoS 0 | `ros_intra_wait`, PM QoS 0 | axon blocking, PM QoS 0 |
|---|---|---|---|---|---|---|
| host 147 KiB | **3.1** | 8.6 | 79 | 4.8 | 5.8 | **4.0** |
| host 1 MiB | **15.4** | 19.9 | 117 | 19.3 | 19.9 | **18.1** |
| host 4 MiB | **58.8** | 63.2 | 243 | 70.7 | 70.9 | **69.6** |
| GPU 147 KiB | **23.2** | 27.2 | 163 | 25.7 | 26.4 | |
| GPU 1 MiB | **23.6** | 27.6 | 153 | 26.1 | 26.9 | **24.9** |
| GPU 4 MiB | **25.4** | 29.6 | 155 | 28.1 | 28.5 | |

- **axon across processes is not slower than ROS 2 inside one process.**
  - Spinning axon vs the `spin_some(0)` loop: 4-5.5 µs faster in every cell.
  - Both consumers blocking, with PM QoS 0: 1.3-2.0 µs faster.

  So moving nodes into one process gives a ROS 2 system no latency advantage over axon, and
  axon keeps the processes separate. A crash stays in its process, and each process can use its
  own language, build and framework. Both paths copy the payload once on the producer (host),
  or share one device buffer (GPU). Where the remaining µs go (executor dispatch, the
  allocation) was not traced.
- **On an idle machine, a blocking consumer pays 70-180 µs to wake.** The default
  `ros_intra_wait` column is 70-180 µs above polling. With deep C-states held off (PM QoS 0) the
  difference is 0.2-1.0 µs. So the cost is the core coming back from a deep C-state, not the
  executor. This PC's deepest state (C3) advertises a 350 µs exit latency.
- **The poll loop does not spin a full core.** The `spin_some(0)` process used 0.08 cores, and
  PM QoS speeds it up at most sizes, so it idles part of the time. The 4 MiB host cells run about
  7 µs higher in the PM QoS session for every transport, axon included. Why was not traced. The blocking
  variant used 0.001-0.02 cores.

Raw data: `results/intra_rep.jsonl`, `results/intra_qos_rep.jsonl` (`"pm_qos_us":0` in the meta
line) and `results/intra_axon_ref.jsonl` (the spinning axon cells, same session as the default
columns).

**PM QoS 0** means that while the runner held `/dev/cpu_dma_latency` open with the value 0, the
kernel kept every core in polling idle (exit latency 0, so not even C1). Real-time robot
computers often do this, or disable deep C-states in the BIOS. A 1 µs limit (C1 allowed) keeps
most of the effect at far less power; see `axon::CpuLatencyHold` below. On this PC the device
was writable without sudo only because of a misconfigured third-party udev rule. The device is
root-only by default; [`tools/udev/99-axon-cpu-latency.rules`](../../tools/udev/99-axon-cpu-latency.rules)
grants a dedicated group. The setting ends when the file is closed.

## 1 → N fan-out

One producer and N subscriber processes, for N = 1, 2, 4 and 8. Here every consumer **blocks**
instead of busy-polling, because 8 spinning consumers on this PC's 10 online CPUs would end up
measuring the scheduler. `ros_loan` waits in an `rclcpp::WaitSet`, then makes a loaned take.
axon waits in `TensorSubscriber::wait_for_update`. Both use the Fast DDS profile and the cell
rules above. Both producers wait for N subscribers, then 500 ms, before the first frame. CPU
is `getrusage` from each process's first frame to its last (5 s). Every consumer, including
the busy-polling default of the earlier tables, now calls `getrusage` once per frame after
taking its timestamp, for that window. The producer's CPU and that
of all its subscribers are added together. 3 runs, median; latency is the worst subscriber's
p50/p99 in a run. Deep C-states held off (PM QoS 0):

| cell | N | `ros_loan` p50 / p99 µs | **axon** p50 / p99 µs | `ros_loan` CPU (cores) | **axon** CPU |
|---|---|---|---|---|---|
| host 147 KiB | 1 | 8.6 / 28 | **4.0 / 8** | 0.0017 | **0.0007** |
| | 8 | 19.1 / 45 | **4.9 / 11** | 0.0141 | **0.0026** |
| host 1 MiB | 1 | 23.1 / 46 | **18.1 / 28** | 0.0033 | **0.0022** |
| | 8 | 33.6 / 62 | **19.0 / 27** | 0.0155 | **0.0041** |
| host 4 MiB | 1 | 73.3 / 99 | **69.6 / 91** | 0.0084 | **0.0076** |
| | 8 | 85.1 / 115 | **69.9 / 92** | 0.0211 | **0.0095** |
| GPU 1 MiB | 1 | 154.3 / 192 | **24.9 / 46** | 0.0167 | **0.0031** |
| | 2 | 204.8 / 238 | **26.7 / 45** | 0.0340 | **0.0047** |
| | 4 | 296.2 / 347 | **29.0 / 47** | 0.0959 | **0.0083** |
| | 8 | 449.7 / 582 | **37.9 / 60** | 0.2564 | **0.0208** |

- **On the host, axon stays flat in N and ROS 2 grows slowly.** From 1 to 8 subscribers at
  147 KiB, ROS 2 goes from 8.6 to 19 µs and axon from 4.0 to 4.9 µs. From 1 MiB up, the
  producer's payload copy dominates both. At N = 8 ROS 2 uses 2-5.5× the CPU of axon, mostly
  in the subscribers.
- **On the GPU, ROS 2's per-subscriber copy adds up.** Each ROS 2 subscriber copies the frame
  H2D itself. Latency grows from 154 to 450 µs, and CPU reaches a quarter core at N = 8. axon's
  subscribers import the same device buffer and grow from 25 to 38 µs, at 12× less CPU.
- **Delivery:** every subscriber of both transports received 500/500 with `first_seq` = 1, 0 torn.

With the default C-states, every blocking consumer pays the wake-up from a deep C-state. All
latencies rise and the ranking stays the same (p50 µs, N = 1 → 8; CPU at N = 8):

| cell | `ros_loan` | axon | CPU at N = 8, `ros_loan` / axon |
|---|---|---|---|
| host 147 KiB | 138 → 136 | 43 → 49 | 0.066 / 0.020 |
| host 1 MiB | 171 → 172 | 81 → 86 | 0.067 / 0.023 |
| host 4 MiB | 296 → 292 | 203 → 208 | 0.076 / 0.032 |
| GPU 1 MiB | 329 → 549 | 134 → 145 | 0.320 / 0.053 |

The ROS 2 path probably pays the wake-up more than once per frame: a Fast DDS thread is woken,
and then the wait set. axon wakes the subscriber directly, with a futex in the shared slot.
This was not traced.

Raw data: `results/fanout_qos_rep.jsonl` and `results/fanout_rep.jsonl`. Each line is tagged
with `fanout_n`; producer lines carry `n_subs`.

```bash
benchmarks/ros2_cpp/run_intra.sh 500 100 3     # same-process ROS 2 (intra.cpp), poll and wait
benchmarks/ros2_cpp/run_fanout.sh 500 100 3    # 1 -> N; FANOUT_CELL=1:1048576 runs one cell
benchmarks/ros2_cpp/run_gated.sh               # how the committed files were made: one unit at a time,
benchmarks/ros2_cpp/run_gated.sh pmqos         #   redone when another process used the GPU or a core
```

## Notes from setting it up

- **Other work on the GPU or CPU invalidates a run.** During this work another process used
  the GPU, and every kernel time-sliced against it: a 200 µs kernel took 840 µs from launch to
  sync. `run_sync_cost.sh` refuses to start while another process uses the GPU's SMs (an idle
  resident model is fine). The intra, fan-out and sync-cost runs above were made in units of
  one rep (one cell for fan-out). A sampler watched `nvidia-smi pmon` and `top` while each
  unit ran, and any unit during which another process used the SMs or more than 30 % of a
  core was discarded and run again. The committed runs still used a filter that also ignored
  `python3` and `node` processes on the GPU (fixed since in `run_gated.sh`). Their GPU cells
  match the earlier gated sessions within run-to-run spread.

- **This PC was unstable during part of the work.** Under heavy mixed load, physical cores 2-4 of the Ryzen 7 9800X3D crashed the kernel (13 panics since 2026-09-30). A ros2_cuda_ipc run made shortly before a panic measured 0.6-1 ms and was discarded. Everything in the GPU-transport comparison above was measured after offlining those cores (`0-1,5-9,13-15` online, recorded in the jsonl `meta` line). The earlier single-stream and multi-stream tables were measured with all 16 CPUs, in sessions that did not panic.

- **Default Fast DDS does not use shared memory for these sizes.** Without the profile, `ros_loan` reports `can_loan_messages=1`, but `/dev/shm` holds no data-sharing segment during a 4 MiB run, and it measured 102 µs (1 MiB) and 332 µs (4 MiB). With the profile the segments appear and latency drops to about 35 µs and 80 µs. `can_loan_messages()` only says the type is plain.
- **The default 512 KB shared-memory segment drops camera frames.** A 614 KB unbounded message does not fit, so Fast DDS sends it as UDP fragments, and under best-effort one lost fragment loses the frame. Before the profile raised the segment to 16 MB, `ros_dyn` delivered 40-92 % of the first camera stream (the other cameras were near 100 %; spreading the first deadlines with `MSTREAM_STAGGER=1` helped but did not fix it). With 16 MB segments, 3 of 3 runs at scale 1 delivered 100 %, and so did every cell above. That is why the binaries now check the segment size before running `ros_dyn`.
- **Forcing data-sharing `ON` breaks node creation**: ROS 2's own topics (`rosout`, parameter events) are not plain types. `AUTOMATIC` enables it where it can.
- **axon's ring depth matters at 4 MiB.** With 16 buffers (64 MB of the 96 MB L3, shared with the producer's source buffer and everything else), host 4 MiB p50 was 104-133 µs; with 4 buffers, 59 µs. ROS 2 keep-last(1) reuses one or two buffers, so the runner uses 4 (`AXON_BENCH_NBUF`).

## Run

Needs ROS 2 Jazzy and a CUDA GPU (both scenarios are in one matrix).

```bash
benchmarks/ros2_cpp/run.sh            # 500 frames at 100 Hz, every cell; prints a table
benchmarks/ros2_cpp/run.sh 200 100    # shorter
```

The runner builds axon with `-DAXON_WITH_CUDA=ON` into `build-cuda/` if needed, builds this package with colcon,
and writes `results/<date>.jsonl` (one producer and one consumer line per cell).
