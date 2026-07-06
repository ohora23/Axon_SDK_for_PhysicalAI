#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""ROS2 latency baseline — the DDS path (design doc §8.2).

Publisher and subscriber run as separate *spawned* processes (subprocess, not
fork — rclpy is multi-threaded, so fork-after-import would risk deadlock),
exchanging a UInt8MultiArray of the same payload size as the axon benchmark.
Every frame carries a monotonic publish timestamp in its first bytes; the
subscriber computes publish->observe latency. Unlike axon, the full payload is
serialized and copied through the DDS transport on every frame — exactly the
cost this project exists to remove.

QoS is sensor-data (best-effort, keep-last depth 1) to mirror axon's latest-value
seqlock semantics as closely as ROS2 allows.

Run (ROS2 must be sourced):
    source /opt/ros/jazzy/setup.bash
    python3 benchmarks/bench_ros2.py --json /tmp/ros2.json
"""

from __future__ import annotations

import array
import json
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(__file__))
import bench_common as bc  # noqa: E402

TOPIC = "bench_tensor_stream"


def _role_subscriber(args) -> int:
    import rclpy
    from rclpy.node import Node
    from rclpy.qos import qos_profile_sensor_data
    from std_msgs.msg import UInt8MultiArray

    rclpy.init()
    node = Node("axon_bench_sub")
    samples: list[int] = []
    state = {"done": False, "last": 0}

    def on_msg(msg) -> None:
        now = bc.monotonic_ns()
        raw = bytes(msg.data[:bc.HEADER_SIZE])
        publish_ts, seqno = bc.read_header(raw)
        if seqno != state["last"]:
            state["last"] = seqno
            samples.append(now - publish_ts)
            if seqno >= args.frames:
                state["done"] = True

    node.create_subscription(UInt8MultiArray, TOPIC, on_msg, qos_profile_sensor_data)
    deadline = bc.monotonic_ns() + int(15e9) + args.frames * int(1e7)
    while rclpy.ok() and not state["done"] and bc.monotonic_ns() < deadline:
        rclpy.spin_once(node, timeout_sec=0.05)

    stats = bc.summarize(samples, {
        "transport": "ROS2 (Fast-RTPS DDS)",
        "payload_bytes": args.bytes,
        "rate_hz": args.rate_hz,
        "frames_sent": args.frames,
    })
    with open(args.json or "/tmp/ros2_bench.json", "w") as f:
        json.dump(stats.as_dict(), f)
    node.destroy_node()
    rclpy.shutdown()
    return 0


def _role_publisher(args) -> int:
    import rclpy
    from rclpy.node import Node
    from rclpy.qos import qos_profile_sensor_data
    from std_msgs.msg import UInt8MultiArray

    rclpy.init()
    node = Node("axon_bench_pub")
    pub = node.create_publisher(UInt8MultiArray, TOPIC, qos_profile_sensor_data)

    # Wait for DDS discovery so early frames are not dropped.
    t_end = time.time() + 8.0
    while pub.get_subscription_count() < 1 and time.time() < t_end:
        time.sleep(0.02)

    frame = bytearray(args.bytes)
    msg = UInt8MultiArray()
    for s in range(1, args.frames + 1):
        t0 = bc.monotonic_ns()
        bc.stamp_header(frame, t0, s)
        msg.data = array.array("B", frame)
        pub.publish(msg)
        bc.sleep_for_rate(args.rate_hz, t0)

    node.destroy_node()
    rclpy.shutdown()
    return 0


def _spawn(role: str, args) -> subprocess.Popen:
    cmd = [sys.executable, os.path.abspath(__file__), "--role", role,
           "--frames", str(args.frames), "--rate-hz", str(args.rate_hz),
           "--bytes", str(args.bytes), "--json", args.json or "/tmp/ros2_bench.json"]
    return subprocess.Popen(cmd)


def main() -> int:
    parser = bc.common_args("ROS2 latency baseline")
    parser.add_argument("--role", choices=["pub", "sub"], default="",
                        help="internal: run a single role (spawned by the orchestrator)")
    args = parser.parse_args()

    if args.role == "sub":
        return _role_subscriber(args)
    if args.role == "pub":
        return _role_publisher(args)

    # Orchestrator: spawn subscriber first (so it is discovered), then publisher.
    result_path = args.json or "/tmp/ros2_bench.json"
    sub = _spawn("sub", args)
    time.sleep(0.5)
    pub = _spawn("pub", args)
    pub_rc = pub.wait()
    sub_rc = sub.wait()
    if sub_rc != 0:
        print(f"bench_ros2: subscriber rc={sub_rc}", file=sys.stderr)
        return sub_rc
    if pub_rc != 0:
        print(f"bench_ros2: publisher rc={pub_rc}", file=sys.stderr)
        return pub_rc

    with open(result_path) as f:
        print(json.dumps(json.load(f)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
