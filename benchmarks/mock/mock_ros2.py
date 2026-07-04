#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""MockSystem over ROS2: multi-stream pub/sub, all metrics.

Same sensor ensemble as mock_dczc, one ROS2 topic per stream (UInt8MultiArray,
sensor-data QoS). The publisher and subscriber are separate spawned processes
(subprocess, not fork — rclpy is multi-threaded). The subscriber is event-driven
(a callback per message), the natural ROS2 consumption model.

Emitted JSON matches mock_dczc's schema so mock_compare can tabulate both.

Run (ROS2 must be sourced):
    source /opt/ros/jazzy/setup.bash
    python3 benchmarks/mock/mock_ros2.py --scale 1 --seconds 5
"""

from __future__ import annotations

import argparse
import array
import json
import os
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(__file__))
import mock_common as mc  # noqa: E402


def _topic(name: str) -> str:
    return f"mock_{name}"


def _role_publisher(streams, seconds: float, result_path: str) -> int:
    import rclpy
    from rclpy.node import Node
    from rclpy.qos import qos_profile_sensor_data
    from std_msgs.msg import UInt8MultiArray

    rclpy.init()
    node = Node("mock_pub")
    pubs = {s.name: node.create_publisher(UInt8MultiArray, _topic(s.name),
                                          qos_profile_sensor_data)
            for s in streams}

    # Wait for discovery on every topic.
    t_end = time.time() + 10.0
    while time.time() < t_end and any(pubs[s.name].get_subscription_count() < 1
                                      for s in streams):
        time.sleep(0.02)

    sent = {s.name: 0 for s in streams}
    stop_at = mc.monotonic_ns() + int(seconds * 1e9)

    def worker(s):
        frame = bytearray(s.bytes)
        msg = UInt8MultiArray()
        period_ns = 1_000_000_000 // s.hz
        seq = 0
        while True:
            t0 = mc.monotonic_ns()
            if t0 >= stop_at:
                break
            seq += 1
            mc.stamp_header(frame, t0, seq)
            msg.data = array.array("B", frame)
            pubs[s.name].publish(msg)
            sent[s.name] = seq
            dt = mc.monotonic_ns() - t0
            if dt < period_ns:
                time.sleep((period_ns - dt) / 1e9)

    threads = [threading.Thread(target=worker, args=(s,)) for s in streams]
    for t in threads:
        t.start()
    for t in threads:
        t.join()

    with open(result_path, "w") as f:
        json.dump({"sent": sent, "cpu_seconds": mc.cpu_seconds_self_and_children()}, f)
    node.destroy_node()
    rclpy.shutdown()
    return 0


def _role_subscriber(streams, seconds: float, result_path: str) -> int:
    import rclpy
    from rclpy.node import Node
    from rclpy.qos import qos_profile_sensor_data
    from std_msgs.msg import UInt8MultiArray

    rclpy.init()
    node = Node("mock_sub")
    results = {s.name: mc.StreamResult(s.name, s.bytes, s.hz) for s in streams}
    last_seq = {s.name: 0 for s in streams}

    def make_cb(name):
        res = results[name]
        def cb(msg):
            now = mc.monotonic_ns()
            pub_ts, seqno = mc.read_header(bytes(msg.data[:mc.HEADER_SIZE]))
            if seqno != last_seq[name]:
                last_seq[name] = seqno
                res.seen += 1
                res.lat_ns.append(now - pub_ts)
        return cb

    for s in streams:
        node.create_subscription(UInt8MultiArray, _topic(s.name),
                                 make_cb(s.name), qos_profile_sensor_data)

    deadline = mc.monotonic_ns() + int((seconds + 2.0) * 1e9)
    while rclpy.ok() and mc.monotonic_ns() < deadline:
        rclpy.spin_once(node, timeout_sec=0.05)

    out = {
        "cpu_seconds": mc.cpu_seconds_self_and_children(),
        "per_stream": {name: {"seen": r.seen, "lat_ns": r.lat_ns}
                       for name, r in results.items()},
    }
    with open(result_path, "w") as f:
        json.dump(out, f)
    node.destroy_node()
    rclpy.shutdown()
    return 0


def _spawn(role: str, scale: int, seconds: float, result_path: str):
    cmd = [sys.executable, os.path.abspath(__file__), "--role", role,
           "--scale", str(scale), "--seconds", str(seconds),
           "--role-json", result_path]
    return subprocess.Popen(cmd)


def main() -> int:
    p = argparse.ArgumentParser(description="MockSystem over ROS2")
    p.add_argument("--scale", type=int, default=1)
    p.add_argument("--seconds", type=float, default=5.0)
    p.add_argument("--json", type=str, default="")
    p.add_argument("--role", choices=["pub", "sub"], default="")
    p.add_argument("--role-json", type=str, default="")
    args = p.parse_args()

    streams = mc.scaled_profile(args.scale)

    if args.role == "pub":
        return _role_publisher(streams, args.seconds, args.role_json)
    if args.role == "sub":
        return _role_subscriber(streams, args.seconds, args.role_json)

    # Orchestrator: subscriber first (so it is discovered), then publisher.
    sub_json = f"/tmp/mock_ros2_sub_{os.getpid()}.json"
    pub_json = f"/tmp/mock_ros2_pub_{os.getpid()}.json"
    sub = _spawn("sub", args.scale, args.seconds, sub_json)
    time.sleep(1.0)
    t_start = mc.monotonic_ns()
    pub = _spawn("pub", args.scale, args.seconds, pub_json)
    pub.wait()
    sub.wait()
    _ = (mc.monotonic_ns() - t_start) / 1e9  # measured wall (incl. margins)

    with open(pub_json) as f:
        pj = json.load(f)
    with open(sub_json) as f:
        sj = json.load(f)

    results = []
    for s in streams:
        r = mc.StreamResult(s.name, s.bytes, s.hz)
        r.sent = pj["sent"][s.name]
        cs = sj["per_stream"][s.name]
        r.seen = cs["seen"]
        r.lat_ns = cs["lat_ns"]
        results.append(r)

    total_cpu = pj["cpu_seconds"] + sj["cpu_seconds"]
    agg = mc.aggregate(results, args.seconds, total_cpu, "ROS2 (Fast-RTPS)", args.scale)
    for f in (sub_json, pub_json):
        try:
            os.unlink(f)
        except OSError:
            pass

    if args.json:
        with open(args.json, "w") as fh:
            json.dump(agg, fh)
    print(json.dumps(agg))
    return 0


if __name__ == "__main__":
    sys.exit(main())
