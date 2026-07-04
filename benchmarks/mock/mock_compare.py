#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""MockSystem comparison + saturation sweep: dczc vs ROS2.

Runs the same multi-stream serving-robot workload through both transports at
increasing scale (more camera streams = more aggregate bandwidth) and tabulates
the four metrics: saturation (achieved vs offered bandwidth), per-stream latency
under load, delivery ratio (drops), and CPU cores used.

    python3 benchmarks/mock/mock_compare.py --scales 1,2,3,4 --seconds 5
"""

from __future__ import annotations

import argparse
import glob
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))


def find_module_dir() -> str | None:
    for pat in ("build/python", "build*/python"):
        for d in glob.glob(os.path.join(REPO, pat)):
            if glob.glob(os.path.join(d, "dczc*.so")):
                return d
    return None


def find_ros_setup() -> str | None:
    hits = sorted(glob.glob("/opt/ros/*/setup.bash"), reverse=True)
    return hits[0] if hits else None


def run_json(cmd, env, label: str) -> dict | None:
    try:
        out = subprocess.run(cmd, env=env, cwd=REPO, capture_output=True,
                             text=True, timeout=180)
    except Exception as e:  # noqa: BLE001
        print(f"[{label}] launch failed: {e}", file=sys.stderr)
        return None
    if out.returncode != 0:
        print(f"[{label}] exit {out.returncode}\n{out.stderr[-1500:]}", file=sys.stderr)
        return None
    lines = [l for l in out.stdout.strip().splitlines() if l.startswith("{")]
    if not lines:
        print(f"[{label}] no JSON:\n{out.stdout[-800:]}", file=sys.stderr)
        return None
    return json.loads(lines[-1])


def run_dczc(scale: int, seconds: float, mod: str) -> dict | None:
    env = dict(os.environ)
    env["PYTHONPATH"] = mod + os.pathsep + env.get("PYTHONPATH", "")
    return run_json([sys.executable, os.path.join(HERE, "mock_dczc.py"),
                     "--scale", str(scale), "--seconds", str(seconds)],
                    env, f"dczc s{scale}")


def run_ros2(scale: int, seconds: float, setup: str) -> dict | None:
    inner = (f"source {setup} && exec python3 {os.path.join(HERE, 'mock_ros2.py')} "
             f"--scale {scale} --seconds {seconds}")
    return run_json(["bash", "-lc", inner], dict(os.environ), f"ros2 s{scale}")


def heavy_latency(agg: dict) -> tuple[float, float]:
    """p50/p99 (µs) of the first heavy (camera) stream."""
    for r in agg["per_stream"]:
        if r["bytes"] >= 100_000:
            return r["p50_us"], r["p99_us"]
    return 0.0, 0.0


def main() -> int:
    p = argparse.ArgumentParser(description="MockSystem dczc vs ROS2 sweep")
    p.add_argument("--scales", type=str, default="1,2,3,4")
    p.add_argument("--seconds", type=float, default=5.0)
    args = p.parse_args()
    scales = [int(x) for x in args.scales.split(",") if x.strip()]

    mod = find_module_dir()
    setup = find_ros_setup()
    if not mod:
        print("dczc module not found — build -DDCZC_BUILD_PYTHON=ON", file=sys.stderr)
    if not setup:
        print("ROS2 not found — dczc-only run", file=sys.stderr)

    print(f"\nMockSystem sweep — serving-robot profile, {args.seconds:.0f}s per point")
    print("cam streams scale with the knob; proprioception (imu/odom/cmd) fixed.\n")

    hdr = (f"{'scale':>5} {'offered':>9} | {'backend':<16} "
           f"{'achieved':>9} {'deliv':>6} {'worst':>6} {'cpu(cores)':>10} "
           f"{'cam p50':>9} {'cam p99':>9}")
    print(hdr)
    print("-" * len(hdr))

    rows = []
    for scale in scales:
        results = {}
        if mod:
            results["dczc"] = run_dczc(scale, args.seconds, mod)
        if setup:
            results["ros2"] = run_ros2(scale, args.seconds, setup)

        for key in ("dczc", "ros2"):
            a = results.get(key)
            if not a:
                continue
            p50, p99 = heavy_latency(a)
            print(f"{scale:>5} {a['offered_mbps']:>7.0f}MB | {a['backend']:<16} "
                  f"{a['achieved_mbps']:>7.0f}MB {a['delivery_overall']:>6.3f} "
                  f"{a['delivery_worst_stream']:>6.3f} {a['cpu_util_cores']:>10.2f} "
                  f"{p50:>8.0f}u {p99:>8.0f}u")
            rows.append((scale, key, a))
        print()

    # Saturation summary: first scale where ROS2's worst-stream delivery < 0.98.
    ros2_rows = [(s, a) for s, k, a in rows if k == "ros2"]
    sat = next((s for s, a in ros2_rows if a["delivery_worst_stream"] < 0.98), None)
    if sat is not None:
        print(f"ROS2 starts dropping frames (worst-stream delivery < 0.98) at scale {sat}.")
    dczc_rows = [(s, a) for s, k, a in rows if k == "dczc"]
    if dczc_rows and ros2_rows:
        smax = max(s for s, _ in dczc_rows)
        d = next(a for s, a in dczc_rows if s == smax)
        r = next((a for s, a in ros2_rows if s == smax), None)
        if r and d["cpu_util_cores"] > 0:
            print(f"At scale {smax}: ROS2 uses {r['cpu_util_cores']/d['cpu_util_cores']:.1f}x "
                  f"the CPU of dczc for the same workload.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
