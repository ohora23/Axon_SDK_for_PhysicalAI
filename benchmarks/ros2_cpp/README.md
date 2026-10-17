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
- **Not measured here:** NITROS / type adaptation, which keeps GPU buffers inside one process (composable nodes), `rmw_zenoh` shared memory, and multi-stream CPU cost in C++. The CPU and cache-miss figures elsewhere in this repo are still against rclpy.

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
