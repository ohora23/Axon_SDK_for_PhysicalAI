#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Run the axon and ROS2 latency benchmarks with identical parameters and print
a side-by-side comparison (design doc §8.2).

    python3 benchmarks/compare.py --frames 500 --rate-hz 200 --bytes 150528

- axon runs against the built module (auto-located under build/python).
- ROS2 runs in a subshell with the ROS2 environment sourced.

Either side is skipped gracefully if its runtime is unavailable, so the script
is useful even on a machine with only one of the two installed.
"""

from __future__ import annotations

import argparse
import glob
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)


def find_module_dir() -> str | None:
    for pat in ("build/python", "build*/python", "**/axon*.so"):
        hits = glob.glob(os.path.join(REPO, pat), recursive=True)
        for h in hits:
            d = h if os.path.isdir(h) else os.path.dirname(h)
            if glob.glob(os.path.join(d, "axon*.so")):
                return d
    return None


def find_ros_setup() -> str | None:
    for p in sorted(glob.glob("/opt/ros/*/setup.bash"), reverse=True):
        return p
    return None


def run_json(cmd: list[str], env: dict, label: str) -> dict | None:
    try:
        out = subprocess.run(cmd, env=env, cwd=REPO, capture_output=True,
                             text=True, timeout=300)
    except Exception as e:  # noqa: BLE001
        print(f"[{label}] failed to launch: {e}", file=sys.stderr)
        return None
    if out.returncode != 0:
        print(f"[{label}] exit {out.returncode}\n{out.stderr[-2000:]}", file=sys.stderr)
        return None
    line = out.stdout.strip().splitlines()[-1] if out.stdout.strip() else ""
    try:
        return json.loads(line)
    except json.JSONDecodeError:
        print(f"[{label}] no JSON on stdout:\n{out.stdout[-1000:]}", file=sys.stderr)
        return None


def bench_args(a) -> list[str]:
    return ["--frames", str(a.frames), "--rate-hz", str(a.rate_hz),
            "--bytes", str(a.bytes)]


def run_axon(a) -> dict | None:
    mod = find_module_dir()
    if not mod:
        print("[axon] module not found — build with -DAXON_BUILD_PYTHON=ON",
              file=sys.stderr)
        return None
    env = dict(os.environ)
    env["PYTHONPATH"] = mod + os.pathsep + env.get("PYTHONPATH", "")
    return run_json([sys.executable, os.path.join(HERE, "bench_axon.py"),
                     *bench_args(a), "--json", "/tmp/axon_bench.json"], env, "axon")


def run_ros2(a) -> dict | None:
    setup = find_ros_setup()
    if not setup:
        print("[ros2] no /opt/ros/*/setup.bash — skipping", file=sys.stderr)
        return None
    inner = (f"source {setup} && exec python3 "
             f"{os.path.join(HERE, 'bench_ros2.py')} "
             f"{' '.join(bench_args(a))} --json /tmp/ros2_bench.json")
    return run_json(["bash", "-lc", inner], dict(os.environ), "ros2")


def fmt_row(label: str, s: dict | None) -> str:
    if not s or s.get("n", 0) == 0:
        return f"  {label:<24} (no data)"
    return (f"  {label:<24} n={s['n']:<5} "
            f"p50={s['p50_us']:>8.1f}  p90={s['p90_us']:>8.1f}  "
            f"p99={s['p99_us']:>9.1f}  max={s['max_us']:>9.1f}  (µs)")


def main() -> int:
    p = argparse.ArgumentParser(description="axon vs ROS2 latency comparison")
    p.add_argument("--frames", type=int, default=500)
    p.add_argument("--rate-hz", type=int, default=200)
    p.add_argument("--bytes", type=int, default=224 * 224 * 3)
    a = p.parse_args()

    print(f"\nWorkload: {a.frames} frames, {a.bytes} B payload "
          f"({a.bytes/1024:.0f} KiB), {a.rate_hz} Hz\n")

    axon_s = run_axon(a)
    ros2_s = run_ros2(a)

    print("─────────── one-way publish→observe latency ───────────")
    print(fmt_row("axon (zero-copy)", axon_s))
    print(fmt_row("ROS2 (Fast-RTPS DDS)", ros2_s))
    print("────────────────────────────────────────────────────────")

    if axon_s and ros2_s and axon_s.get("n") and ros2_s.get("n"):
        for k in ("p50_us", "p99_us"):
            d, r = axon_s[k], ros2_s[k]
            if d > 0:
                print(f"  {k:<8} speedup (ROS2/axon): {r/d:5.1f}x")
    print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
