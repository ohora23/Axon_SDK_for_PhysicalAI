# SPDX-License-Identifier: Apache-2.0
"""MockSystem: a representative multi-stream robot data workload.

Instead of one synthetic stream, this models the concurrent sensor traffic of a
serving/mobile robot (Bear Robotics Servi/Penny class): several RGBD cameras, a
2D LiDAR, plus high-rate proprioception (IMU, odometry, control). The point is to
generate realistic *aggregate bandwidth* and watch how each transport (dczc vs
ROS2/DDS) behaves as that bandwidth scales — where DDS saturates on
serialization+copy while dczc stays flat because only descriptors cross the
metadata plane.

Profile numbers are a representative default (not any proprietary spec) and are
easy to edit. The `scale` knob replicates the heavy camera streams to sweep
toward the saturation point.
"""

from __future__ import annotations

import resource
import struct
import time
from dataclasses import dataclass, field


def monotonic_ns() -> int:
    return time.clock_gettime_ns(time.CLOCK_MONOTONIC_RAW)


# Payload header stamped into the first bytes of every frame:
#   [0:8]  publish timestamp (monotonic ns)
#   [8:16] per-stream sequence number
HEADER_FMT = "<QQ"
HEADER_SIZE = struct.calcsize(HEADER_FMT)


def stamp_header(buf, publish_ts_ns: int, seqno: int) -> None:
    struct.pack_into(HEADER_FMT, buf, 0, publish_ts_ns, seqno)


def read_header(buf) -> tuple[int, int]:
    return struct.unpack_from(HEADER_FMT, buf, 0)


@dataclass(frozen=True)
class Stream:
    name: str
    bytes: int
    hz: int
    heavy: bool = False  # replicated by the scale knob (cameras/depth)


# Representative serving-robot sensor set. Cameras dominate the bandwidth; the
# proprioception streams dominate the message *rate*.
def base_profile() -> list[Stream]:
    return [
        Stream("cam_front_rgb", 640 * 480 * 2, 30, heavy=True),   # YUYV 640x480
        Stream("cam_left_rgb",  640 * 480 * 2, 30, heavy=True),
        Stream("cam_right_rgb", 640 * 480 * 2, 30, heavy=True),
        Stream("depth_front",   640 * 480 * 2, 30, heavy=True),   # 16-bit depth
        Stream("lidar_2d",      1080 * 8,      15),               # ranges+intensities
        Stream("imu",           64,            200),
        Stream("wheel_odom",    48,            100),
        Stream("cmd_vel",       48,            50),
    ]


def scaled_profile(scale: int) -> list[Stream]:
    """scale>=1 replicates the heavy (camera/depth) streams to push bandwidth."""
    streams = base_profile()
    if scale <= 1:
        return streams
    out: list[Stream] = []
    for s in streams:
        if s.heavy:
            for k in range(scale):
                out.append(Stream(f"{s.name}_{k}", s.bytes, s.hz, heavy=True))
        else:
            out.append(s)
    return out


def offered_mbps(streams: list[Stream]) -> float:
    return sum(s.bytes * s.hz for s in streams) / 1e6


# ---- per-stream latency + delivery accounting ----
@dataclass
class StreamResult:
    name: str
    bytes: int
    hz: int
    sent: int = 0
    seen: int = 0            # distinct frames the consumer observed
    lat_ns: list[int] = field(default_factory=list)

    def delivery_ratio(self) -> float:
        return self.seen / self.sent if self.sent else 0.0

    def pct(self, p: float) -> float:
        if not self.lat_ns:
            return 0.0
        v = sorted(self.lat_ns)
        idx = min(len(v) - 1, int(p * (len(v) - 1) + 0.5))
        return v[idx] / 1e3  # µs

    def summary(self) -> dict:
        return {
            "name": self.name, "bytes": self.bytes, "hz": self.hz,
            "sent": self.sent, "seen": self.seen,
            "delivery": round(self.delivery_ratio(), 4),
            "p50_us": round(self.pct(0.50), 1),
            "p99_us": round(self.pct(0.99), 1),
            "max_us": round(self.pct(1.0), 1),
        }


def cpu_seconds_self_and_children() -> float:
    """Total CPU seconds (user+sys) for this process and its reaped children."""
    s = resource.getrusage(resource.RUSAGE_SELF)
    c = resource.getrusage(resource.RUSAGE_CHILDREN)
    return (s.ru_utime + s.ru_stime) + (c.ru_utime + c.ru_stime)


def aggregate(results: list[StreamResult], window_s: float, cpu_s: float,
              backend: str, scale: int) -> dict:
    # window_s is the streaming duration used to normalize throughput and CPU for
    # both backends (so ROS2's discovery/drain margins don't skew the numbers).
    sent = sum(r.sent for r in results)
    seen = sum(r.seen for r in results)
    achieved_mbps = sum(r.bytes * r.seen for r in results) / 1e6 / window_s if window_s else 0.0
    offered = offered_mbps([Stream(r.name, r.bytes, r.hz) for r in results])
    worst = min((r.delivery_ratio() for r in results), default=0.0)
    return {
        "backend": backend,
        "scale": scale,
        "streams": len(results),
        "offered_mbps": round(offered, 1),
        "achieved_mbps": round(achieved_mbps, 1),
        "frames_sent": sent,
        "frames_seen": seen,
        "delivery_overall": round(seen / sent, 4) if sent else 0.0,
        "delivery_worst_stream": round(worst, 4),
        "cpu_seconds": round(cpu_s, 2),
        "window_seconds": round(window_s, 2),
        "cpu_util_cores": round(cpu_s / window_s, 2) if window_s else 0.0,
        "per_stream": [r.summary() for r in results],
    }
