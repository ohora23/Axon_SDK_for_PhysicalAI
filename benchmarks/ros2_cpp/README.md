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

Both ROS 2 cells run with the same Fast DDS profile, [`fastdds_zero_copy.xml`](fastdds_zero_copy.xml) (`RMW_FASTRTPS_USE_QOS_FROM_XML=1`): synchronous publishing, data-sharing where the type allows it, preallocated history, and the same QoS as the code. The runner records the data-sharing segments in `/dev/shm` halfway through each cell; every `ros_loan` cell had them.

Two scenarios:

- **host** (`gpu=0`): the producer has the payload in host memory; the consumer reads it in host memory.
- **GPU** (`gpu=1`): the producer has the payload on the GPU (an inference output) and the consumer needs it on the GPU.
  ROS 2 moves it D2H on the producer and H2D on the consumer; axon hands the consumer the same device buffer.

Same rules for every transport:

- One C++ binary, producer and consumer in separate processes, the same clock (`CLOCK_MONOTONIC_RAW`).
- The producer writes the **whole payload** every frame, taking `t0` just before the write. Latency is `t0` → the consumer has the frame where it needs it.
- The consumer **busy-polls** (`take` / `rcl_take_loaned_message` / `latest_view`), so no transport pays for a wakeup. `ros_loan` refuses to run if the subscription cannot take loans.
- GPU copies are followed by `cuCtxSynchronize()` before a timestamp or a publish, so no copy is still in flight when the clock stops. The axon GPU consumer reads the 16 B header and 8 B tail back from the device to check the frame; that readback is most of axon's flat ~23 µs.
- Header and tail both carry the seqno; a mismatch counts as `torn`. Every cell below had 0 torn frames.
- QoS keep-last(1), best-effort; axon's ring is 4 buffers deep (`AXON_BENCH_NBUF`, see below).
- 500 frames at 100 Hz, the first 20 dropped as warm-up, 3 runs per cell.

## Results

Ryzen 7 9800X3D, RTX 5080, Ubuntu 24.04 (non-RT kernel), ROS 2 Jazzy, Fast DDS 2.14.
p50 latency in µs, median of 3 runs (the three runs in brackets); raw data in `results/rep*.jsonl`.

**Host → host**

| payload | `ros_dyn` | `ros_loan` (zero-copy) | `axon` | axon vs `ros_loan` |
|---|---|---|---|---|
| 147 KiB | 40 (41/40/40) | 24 (15/24/24) | **4** (4/4/4) | 6× |
| 1 MiB | 130 (130/130/130) | 34 (34/34/34) | **15** (15/15/15) | 2.3× |
| 4 MiB | 454 (455/454/448) | 78 (78/78/78) | **59** (59/59/59) | 1.3× |

**GPU → GPU**

| payload | `ros_dyn` | `ros_loan` (zero-copy) | `axon` | axon vs `ros_loan` |
|---|---|---|---|---|
| 147 KiB | 65 (66/65/64) | 48 (48/47/48) | **23** (23/23/23) | 2.1× |
| 1 MiB | 282 (282/282/284) | 168 (168/169/166) | **23** (23/23/23) | 7.3× |
| 4 MiB | 835 (835/845/835) | 489 (485/490/489) | **25** (25/25/25) | 20× |

p99 stays close to p50: in every single run, p99 is within 2.4× of p50.
`ros_dyn` dropped up to 10 of 500 frames at 1 and 4 MiB; `ros_loan` and `axon` dropped none.

## What this says

- **On the host, ROS 2's zero-copy path is close to axon.** Both copy the payload once, into shared memory, on the producer. What is left is a fixed cost per message of about 20 µs in the DDS machinery, so axon's lead shrinks from 6× at 147 KiB to 1.3× at 4 MiB. Against `ros_dyn`, which copies the payload in transit, axon is 7.7-10× ahead.
- **On the GPU, the gap is the two copies ROS 2 cannot avoid.** A ROS 2 message lives in host memory, so a GPU payload goes D2H, through the transport, then H2D: 489 µs at 4 MiB. axon passes the device buffer itself, and its latency stays at 23-25 µs whatever the size.
  The two copies here are pageable. Measured separately on this machine, a 4 MiB D2H + H2D pair takes 408 µs pageable and 230 µs pinned, so pinning the ROS 2 buffers would bring the 4 MiB cell to roughly 310 µs, still about 12× axon. Not measured inside the benchmark: the loan lives in the Fast DDS segment, and registering it with CUDA is not something a ROS 2 user would normally do.
- **Not measured here:** NITROS / type adaptation, which keeps GPU buffers inside one process (composable nodes), and `rmw_zenoh` shared memory.

## Multi-stream: CPU, delivery and cache misses

`mstream` runs the [`benchmarks/mock`](../mock/README.md) robot workload in C++: four 640×480×2 camera/depth streams at
30 Hz (×`scale`), a 2D LiDAR, IMU, odometry and `cmd_vel`; scale 1 offers 74 MB/s, scale 4 297 MB/s. One producer
process writes every payload in full (one thread, earliest deadline first). Each consumer runs in its **natural usage
model**: ROS 2 an event-driven `SingleThreadedExecutor` (`spin_once` blocks until work arrives), axon a 1 kHz loop that
reads the latest of every stream.

- **Window:** the consumer measures 5 s from the first frame it sees; the producer sends for 6.5 s. Setup, discovery
  and idle waits are outside both windows.
- **CPU:** `getrusage` over each window, producer plus consumer (all threads, including Fast DDS's).
- **Cache misses and instructions:** `perf_event_open` on every thread of each process, enabled only inside the same
  windows (`PERF=1`), reported per second of window.
- **Delivery:** distinct frames seen / the seqno span inside the window, per stream; the table shows the worst stream.
- Both ROS 2 cells use the same Fast DDS profile as above (so `ros_dyn` is rclcpp with that profile, not a stock
  install). Consumers read the 24 bytes they check (header and tail), not the whole frame. CPU frequency and core
  placement were not pinned.

3 runs, median (all three in brackets for CPU):

| scale (MB/s) | `ros_dyn` cores | `ros_loan` cores | `axon` cores | `ros_dyn` worst-stream delivery (range) | camera p50 µs (`ros_dyn` / `ros_loan` / `axon`) |
|---|---|---|---|---|---|
| 1 (74) | 0.038 (0.030/0.051/0.038) | 0.032 (0.024/0.041/0.032) | **0.007** (0.007/0.007/0.007) | 0.73 (0.40-0.89) | 132 / 73 / 643 |
| 2 (148) | 0.049 (0.049/0.062/0.045) | 0.043 (0.044/0.043/0.028) | **0.009** (0.009/0.009/0.009) | 0.82 (0.42-0.89) | 122 / 58 / 616 |
| 3 (222) | 0.077 (0.076/0.080/0.067) | 0.046 (0.046/0.048/0.036) | **0.011** (0.012/0.011/0.011) | 0.51 (0.46-0.79) | 143 / 59 / 588 |
| 4 (297) | 0.093 (0.099/0.093/0.078) | 0.053 (0.062/0.053/0.043) | **0.014** (0.013/0.014/0.014) | 0.51 (0.50-0.92) | 142 / 50 / 554 |

`ros_loan` and `axon` delivered every frame of every stream in every run (worst stream 1.000), with 0 torn frames.

Cache misses and instructions per second of window, producer + consumer, median of the same 3 runs:

| scale (MB/s) | `ros_dyn` | `ros_loan` | `axon` |
|---|---|---|---|
| 1 (74) | 5.17 M / 0.066 B | 2.85 M / 0.046 B | **0.36 M / 0.015 B** |
| 4 (297) | 15.19 M / 0.208 B | 4.38 M / 0.123 B | **0.88 M / 0.023 B** |

What this says:

- **In C++ every transport is cheap.** At 297 MB/s, ROS 2's zero-copy path costs 0.053 cores and axon 0.014. The
  rclpy figures in [`benchmarks/mock`](../mock/README.md) (0.98 vs 0.30 cores) are almost all Python.
- **axon uses about 4× less CPU, 5× fewer cache misses and 5× fewer instructions than ROS 2's zero-copy path** at
  297 MB/s. None of the three is flat in bandwidth: from scale 1 to 4, axon goes from 0.007 to 0.014 cores (the
  producer's payload writes), `ros_loan` from 0.032 to 0.053, `ros_dyn` from 0.038 to 0.093. Run-to-run spread for the
  ROS 2 cells is up to ±30 %.
- **axon's 1 kHz loop costs latency.** A frame waits on average half a tick for the loop, so axon's camera p50 is
  550-650 µs against 50-75 µs for the event-driven `ros_loan`. That is the price of the RT-loop model (a fixed rate, no
  wakeups), not of the transport: the single-stream numbers above, with busy polling, are 4-59 µs.
- **rclcpp with `UInt8MultiArray` drops camera frames, and only on the first camera.** In every `ros_dyn` run the
  worst stream was the first camera stream (40-92 % delivered); the other cameras were near 100 %. All cameras are due
  at the same instant, and spreading their first deadlines over one period (`MSTREAM_STAGGER=1`) raised the first
  camera to 95-99 % at scale 1 (3 runs) but did not remove the loss. We have not pinned the cause down. Our guess is
  that a 614 KB unbounded message does not fit Fast DDS's default 512 KB shared-memory segment, goes out as UDP
  fragments, and loses the frame when a fragment is dropped under best-effort.
- **Not covered:** consumers that read the whole frame, the EventsExecutor, intra-process composition, `rmw_zenoh`.

```bash
benchmarks/ros2_cpp/run_mstream.sh                 # 5 s per cell, scales 1-4
PERF=1 benchmarks/ros2_cpp/run_mstream.sh          # + cache misses and instructions (perf_event_paranoid <= 1)
```

`MSTREAM_DEBUG=1` prints per-stream counts from the consumer; `MSTREAM_STAGGER=1` spreads the first deadlines.
Raw data: `results/ms_rep{1,2,3}.jsonl`.

## Notes from setting it up

- **Default Fast DDS does not use shared memory for these sizes.** Without the profile, `ros_loan` reports `can_loan_messages=1`, but `/dev/shm` holds no data-sharing segment during a 4 MiB run, and it measured 102 µs (1 MiB) and 332 µs (4 MiB). With [`fastdds_zero_copy.xml`](fastdds_zero_copy.xml) the segments appear and latency drops to 34 µs and 78 µs. `can_loan_messages()` only says the type is plain.
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
