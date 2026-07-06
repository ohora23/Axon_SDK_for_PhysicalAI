#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""MockSystem over axon: multi-stream producer/consumer, all metrics.

Producer (parent) and consumer (child, forked before any threads) each run one
thread per sensor stream. Every stream is an independent axon service (own pool +
sidecar + metadata slot). Only the fixed-size descriptor crosses the metadata
plane; the payload stays in the shared dma-buf.

Emitted JSON: per-stream latency/delivery + aggregate throughput + CPU.

Run (needs the built module on PYTHONPATH):
    PYTHONPATH=build/python python3 benchmarks/mock/mock_axon.py --scale 1 --seconds 5
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import threading
import time

import numpy as np

sys.path.insert(0, os.path.dirname(__file__))
import mock_common as mc  # noqa: E402

import axon  # noqa: E402


def _service(name: str) -> str:
    return f"mock/{name}"


# A single RT-style control loop reads the latest of every stream each tick —
# the axon usage model (design doc §4.3: "RT consumer seqlock-reads on the next
# tick"). One loop, not one thread per stream: realistic and fair on CPU.
CONTROL_HZ = 1000


def run_consumer(streams, seconds: float, result_path: str) -> int:
    subs = {}
    for s in streams:
        sub = axon.TensorSubscriber.create(_service(s.name))
        sub.set_fallback_policy(axon.FallbackPolicy.LastKnownGood)
        if sub.wait_handshake(8000) != 0:
            print(f"consumer: handshake failed for {s.name}", file=sys.stderr)
            return 11
        subs[s.name] = sub

    results = {s.name: mc.StreamResult(s.name, s.bytes, s.hz) for s in streams}
    last_seq = {s.name: 0 for s in streams}
    tick_ns = 1_000_000_000 // CONTROL_HZ
    stop_at = mc.monotonic_ns() + int(seconds * 1e9)

    while mc.monotonic_ns() < stop_at:
        t0 = mc.monotonic_ns()
        for s in streams:
            v = subs[s.name].latest_view(8)
            if v is not None and v.seqno != last_seq[s.name]:
                last_seq[s.name] = v.seqno
                pub_ts, seqno = mc.read_header(v.data.reshape(-1)[:mc.HEADER_SIZE])
                if seqno == v.seqno:
                    r = results[s.name]
                    r.seen += 1
                    r.lat_ns.append(mc.monotonic_ns() - pub_ts)
        dt = mc.monotonic_ns() - t0
        if dt < tick_ns:
            time.sleep((tick_ns - dt) / 1e9)

    out = {
        "cpu_seconds": mc.cpu_seconds_self_and_children(),
        "per_stream": {name: {"seen": r.seen, "lat_ns": r.lat_ns}
                       for name, r in results.items()},
    }
    with open(result_path, "w") as f:
        json.dump(out, f)
    return 0


def run_producer(streams, seconds: float, child_pid: int, result_path: str) -> dict:
    pubs = {}
    frames = {}
    for s in streams:
        pool = axon.TensorPool.create(
            n_buffers=8, buffer_size=max(s.bytes, 4096),
            backend=axon.PoolBackend.Custom)
        pub = axon.TensorPublisher.create(_service(s.name), pool)
        pubs[s.name] = (pool, pub)
        frames[s.name] = bytearray(s.bytes)

    # Handshake each stream (rendezvous with the child's per-stream wait).
    for s in streams:
        pubs[s.name][1].handshake_pool()

    sent = {s.name: 0 for s in streams}
    t_start = mc.monotonic_ns()
    stop_at = t_start + int(seconds * 1e9)

    def worker(s):
        pub = pubs[s.name][1]
        frame = frames[s.name]
        arr = np.frombuffer(frame, dtype=np.uint8)
        period_ns = 1_000_000_000 // s.hz
        seq = 0
        while True:
            t0 = mc.monotonic_ns()
            if t0 >= stop_at:
                break
            seq += 1
            mc.stamp_header(frame, t0, seq)
            pub.publish(arr, axon.DType.U8)
            sent[s.name] = seq
            dt = mc.monotonic_ns() - t0
            if dt < period_ns:
                time.sleep((period_ns - dt) / 1e9)

    threads = [threading.Thread(target=worker, args=(s,)) for s in streams]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    wall_s = (mc.monotonic_ns() - t_start) / 1e9

    producer_cpu = mc.cpu_seconds_self_and_children()
    os.waitpid(child_pid, 0)
    with open(result_path) as f:
        child = json.load(f)

    results = []
    for s in streams:
        r = mc.StreamResult(s.name, s.bytes, s.hz)
        r.sent = sent[s.name]
        cs = child["per_stream"][s.name]
        r.seen = cs["seen"]
        r.lat_ns = cs["lat_ns"]
        results.append(r)

    total_cpu = producer_cpu + child["cpu_seconds"]
    _ = wall_s  # measured for reference; throughput/CPU normalize to `seconds`
    return mc.aggregate(results, seconds, total_cpu, "axon", 0)


def main() -> int:
    p = argparse.ArgumentParser(description="MockSystem over axon")
    p.add_argument("--scale", type=int, default=1)
    p.add_argument("--seconds", type=float, default=5.0)
    p.add_argument("--json", type=str, default="")
    args = p.parse_args()

    streams = mc.scaled_profile(args.scale)
    child_result = f"/tmp/mock_axon_child_{os.getpid()}.json"

    pid = os.fork()
    if pid == 0:
        os._exit(run_consumer(streams, args.seconds + 0.5, child_result))

    agg = run_producer(streams, args.seconds, pid, child_result)
    agg["scale"] = args.scale
    try:
        os.unlink(child_result)
    except OSError:
        pass

    if args.json:
        with open(args.json, "w") as f:
            json.dump(agg, f)
    print(json.dumps(agg))
    return 0


if __name__ == "__main__":
    sys.exit(main())
