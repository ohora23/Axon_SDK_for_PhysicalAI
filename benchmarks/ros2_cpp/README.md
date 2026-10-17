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
- **Not covered:** consumers that read the whole frame, the EventsExecutor, intra-process composition, `rmw_zenoh`.

```bash
benchmarks/ros2_cpp/run_mstream.sh                 # 5 s per cell, scales 1-4
PERF=1 benchmarks/ros2_cpp/run_mstream.sh          # + cache misses and instructions (perf_event_paranoid <= 1)
```

`MSTREAM_DEBUG=1` prints per-stream counts from the consumer; `MSTREAM_STAGGER=1` spreads the first deadlines.
Raw data: `results/ms_rep{1,2,3}.jsonl`.

## Notes from setting it up

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
