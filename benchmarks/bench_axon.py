#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""axon latency benchmark — the zero-copy path.

Producer and consumer run as separate processes (fork) over the real sidecar +
seqlock + dma-buf pool. Each frame carries a monotonic publish timestamp in its
first bytes; the consumer computes publish->observe latency. Because the payload
lives in a shared dma-buf, only the fixed-size descriptor crosses the metadata
queue — there is no per-frame serialization or byte copy on the transport.

Run (needs the built module on PYTHONPATH):
    PYTHONPATH=build/python python3 benchmarks/bench_axon.py --json /tmp/axon.json
"""

from __future__ import annotations

import json
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(__file__))
import bench_common as bc  # noqa: E402

import axon  # noqa: E402

SERVICE = "bench/axon_stream"


def run_consumer(args, result_path: str) -> int:
    sub = axon.TensorSubscriber.create(SERVICE)
    sub.set_fallback_policy(axon.FallbackPolicy.LastKnownGood)
    if sub.wait_handshake(5000) != 0:
        return 11

    samples = []
    last_seqno = 0
    # Busy-poll so the measurement reflects transport + seqlock cost, not a
    # polling artifact (ROS2's callback path is event-driven; this keeps the
    # comparison about the transport, not the wakeup model).
    deadline = bc.monotonic_ns() + int(6e9) + args.frames * int(1e7)
    while True:
        v = sub.latest_view(8)
        if v is not None and v.seqno != last_seqno:
            now = bc.monotonic_ns()
            last_seqno = v.seqno
            publish_ts, seqno = bc.read_header(v.data.reshape(-1)[:bc.HEADER_SIZE])
            if seqno == v.seqno:
                samples.append(now - publish_ts)
            if v.seqno >= args.frames:
                break
        if bc.monotonic_ns() > deadline:
            break

    stats = bc.summarize(samples, {
        "transport": "axon (zero-copy)",
        "payload_bytes": args.bytes,
        "rate_hz": args.rate_hz,
        "frames_sent": args.frames,
    })
    with open(result_path, "w") as f:
        json.dump(stats.as_dict(), f)
    return 0


def run_producer(args, child_pid: int) -> int:
    n_buffers = 16
    pool = axon.TensorPool.create(
        n_buffers=n_buffers, buffer_size=max(args.bytes, 4096),
        backend=axon.PoolBackend.Custom)
    pub = axon.TensorPublisher.create(SERVICE, pool)
    pub.handshake_pool()

    frame = bytearray(args.bytes)
    for s in range(1, args.frames + 1):
        t0 = bc.monotonic_ns()
        bc.stamp_header(frame, t0, s)
        pub.publish(np.frombuffer(frame, dtype=np.uint8), axon.DType.U8)
        bc.sleep_for_rate(args.rate_hz, t0)

    _, status = os.waitpid(child_pid, 0)
    return os.waitstatus_to_exitcode(status)


def main() -> int:
    args = bc.common_args("axon latency benchmark").parse_args()
    result_path = args.json or "/tmp/axon_bench.json"

    pid = os.fork()
    if pid == 0:
        os._exit(run_consumer(args, result_path))
    rc = run_producer(args, pid)
    if rc != 0:
        print(f"bench_axon: consumer failed rc={rc}", file=sys.stderr)
        return rc

    with open(result_path) as f:
        stats = json.load(f)
    print(json.dumps(stats))
    return 0


if __name__ == "__main__":
    sys.exit(main())
