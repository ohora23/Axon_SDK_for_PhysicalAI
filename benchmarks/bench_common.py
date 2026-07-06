# SPDX-License-Identifier: Apache-2.0
"""Shared helpers for the axon vs ROS2 latency benchmarks (design doc §8.2).

All benchmarks measure one-way, publish->observe latency using CLOCK_MONOTONIC_RAW
stamped into the first bytes of the payload, so the numbers are directly
comparable across transports on the same host.
"""

from __future__ import annotations

import argparse
import struct
import time
from dataclasses import dataclass, asdict


def monotonic_ns() -> int:
    return time.clock_gettime_ns(time.CLOCK_MONOTONIC_RAW)


# The payload's first 16 bytes carry: [0:8] publish timestamp, [8:16] seqno.
HEADER_FMT = "<QQ"
HEADER_SIZE = struct.calcsize(HEADER_FMT)


def stamp_header(buf: bytearray, publish_ts_ns: int, seqno: int) -> None:
    struct.pack_into(HEADER_FMT, buf, 0, publish_ts_ns, seqno)


def read_header(buf) -> tuple[int, int]:
    return struct.unpack_from(HEADER_FMT, buf, 0)


@dataclass
class LatencyStats:
    transport: str
    payload_bytes: int
    rate_hz: int
    frames_sent: int
    frames_seen: int
    n: int = 0
    p50_us: float = 0.0
    p90_us: float = 0.0
    p99_us: float = 0.0
    max_us: float = 0.0
    mean_us: float = 0.0
    min_us: float = 0.0

    def as_dict(self) -> dict:
        return asdict(self)


def summarize(samples_ns, meta: dict) -> LatencyStats:
    s = LatencyStats(
        transport=meta["transport"],
        payload_bytes=meta["payload_bytes"],
        rate_hz=meta["rate_hz"],
        frames_sent=meta["frames_sent"],
        frames_seen=len(samples_ns),
    )
    if not samples_ns:
        return s
    v = sorted(samples_ns)
    n = len(v)

    def pct(p: float) -> float:
        idx = min(n - 1, int(p * (n - 1) + 0.5))
        return v[idx] / 1e3

    s.n = n
    s.min_us = v[0] / 1e3
    s.max_us = v[-1] / 1e3
    s.mean_us = (sum(v) / n) / 1e3
    s.p50_us = pct(0.50)
    s.p90_us = pct(0.90)
    s.p99_us = pct(0.99)
    return s


def common_args(description: str) -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description=description)
    p.add_argument("--frames", type=int, default=500)
    p.add_argument("--rate-hz", type=int, default=200,
                   help="publish rate; 0 = as fast as possible")
    p.add_argument("--bytes", type=int, default=224 * 224 * 3,
                   help="payload size in bytes (default 150528 = 224x224x3)")
    p.add_argument("--json", type=str, default="",
                   help="write LatencyStats as JSON to this path")
    return p


def sleep_for_rate(rate_hz: int, t0_ns: int) -> None:
    if rate_hz <= 0:
        return
    period_ns = 1_000_000_000 // rate_hz
    elapsed = monotonic_ns() - t0_ns
    if elapsed < period_ns:
        time.sleep((period_ns - elapsed) / 1e9)
