#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Regenerate the axon performance charts (docs/assets/*.png) from the measured
# numbers in docs/hardware-verification.md and the benchmark report. One clean,
# consistent style: Liberation Sans, muted grid, direct labels, axon green vs a
# warm "cost" color, redundant encoding (marker/dash) for color-vision safety.
#
#   python3 docs/assets/make_charts.py [out_dir]
import sys, pathlib
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib import font_manager, ticker

# ---- style tokens ----
AXON, COST = "#159e5e", "#e0663a"
INK, MUTED, GRID, SPINE = "#1f2933", "#6b7684", "#e8ecf0", "#cbd2d9"
for fam in ("Liberation Sans", "DejaVu Sans"):
    if any(fam in f.name for f in font_manager.fontManager.ttflist):
        plt.rcParams["font.family"] = fam
        break
plt.rcParams.update({
    "figure.dpi": 200, "savefig.dpi": 200, "figure.facecolor": "white",
    "axes.facecolor": "white", "text.color": INK, "axes.labelcolor": INK,
    "xtick.color": MUTED, "ytick.color": MUTED, "axes.edgecolor": SPINE,
    "font.size": 11, "axes.labelsize": 11, "xtick.labelsize": 10, "ytick.labelsize": 10,
})
OUT = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else pathlib.Path(__file__).parent)
OUT.mkdir(parents=True, exist_ok=True)


def frame(title, subtitle, size=(6.8, 4.2)):
    fig = plt.figure(figsize=size)
    fig.text(0.035, 0.945, title, size=15, weight="bold", color=INK, va="top")
    fig.text(0.035, 0.875, subtitle, size=10.5, color=MUTED, va="top")
    ax = fig.add_axes([0.115, 0.13, 0.83, 0.62])
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)
    ax.grid(axis="y", color=GRID, lw=1, zorder=0)
    ax.set_axisbelow(True)
    ax.tick_params(length=0)
    return fig, ax


def save(fig, name):
    p = OUT / name
    fig.savefig(p, facecolor="white")
    plt.close(fig)
    print("wrote", p)


def barlabel(ax, bars, fmt, dy=1.02, **kw):
    for b in bars:
        h = b.get_height()
        ax.text(b.get_x() + b.get_width() / 2, h * dy, fmt(h), ha="center",
                va="bottom", size=9.5, color=INK, **kw)


# 1. single-stream latency ---------------------------------------------------
fig, ax = frame("Single-stream latency", "One-way publish→observe. axon barely grows with payload; ROS2 pays serialization.")
groups, ax_v, ro_v, mult = ["147 KiB\n@200 Hz", "1 MiB\n@120 Hz"], [18, 46], [165, 971], ["9.0×", "20.9×"]
x = range(len(groups)); w = 0.34
b1 = ax.bar([i - w/2 for i in x], ax_v, w, color=AXON, label="axon", zorder=3)
b2 = ax.bar([i + w/2 for i in x], ro_v, w, color=COST, label="ROS2 (Fast-RTPS)", zorder=3)
ax.set_yscale("log"); ax.set_ylabel("p50 latency (µs, log)")
ax.set_xticks(list(x)); ax.set_xticklabels(groups)
barlabel(ax, b1, lambda h: f"{h:.0f}"); barlabel(ax, b2, lambda h: f"{h:.0f}")
for i, m in enumerate(mult):
    ax.text(i, ro_v[i]*1.55, m, ha="center", size=11, weight="bold", color=AXON)
ax.set_ylim(10, 2000)
ax.legend(frameon=False, loc="upper left", fontsize=9.5)
save(fig, "01_latency_single.png")

# 2. CPU vs bandwidth --------------------------------------------------------
fig, ax = frame("CPU cost vs bandwidth", "Multi-stream serving-robot load. axon stays flat as sensor bandwidth grows.")
bw = [74, 148, 222, 295]
ax_c, ro_c = [0.26, 0.27, 0.285, 0.30], [0.77, 0.84, 0.91, 0.98]
ax.plot(bw, ax_c, "-o", color=AXON, lw=2.4, ms=6, label="axon", zorder=3)
ax.plot(bw, ro_c, "--s", color=COST, lw=2.4, ms=6, label="ROS2 (Fast-RTPS)", zorder=3)
ax.set_xlabel("aggregate bandwidth (MB/s)"); ax.set_ylabel("CPU (cores)")
ax.set_ylim(0, 1.15); ax.set_xlim(60, 305)
ax.text(295, 0.30-0.07, "0.30", ha="right", size=10, color=AXON, weight="bold")
ax.text(295, 0.98+0.03, "0.98", ha="right", size=10, color=COST, weight="bold")
ax.annotate("3.3× less CPU", xy=(295, 0.64), xytext=(210, 0.55), size=10.5, weight="bold",
            color=INK, arrowprops=dict(arrowstyle="-", color=MUTED, lw=1))
ax.legend(frameon=False, loc="upper left", fontsize=9.5)
save(fig, "02_cpu_sweep.png")

# 3. frame delivery ----------------------------------------------------------
fig, ax = frame("Frame delivery under load", "axon drops zero frames; ROS2's worst stream falls as load rises.")
dv_ax, dv_ro = [100, 100, 100, 100], [100, 99.4, 98.2, 96.7]
ax.plot(bw, dv_ax, "-o", color=AXON, lw=2.4, ms=6, label="axon", zorder=3)
ax.plot(bw, dv_ro, "--s", color=COST, lw=2.4, ms=6, label="ROS2 worst stream", zorder=3)
ax.set_xlabel("aggregate bandwidth (MB/s)"); ax.set_ylabel("frames delivered (%)")
ax.set_ylim(95.5, 100.6); ax.set_xlim(60, 305)
ax.text(295, 100.15, "100%", ha="right", size=10, color=AXON, weight="bold")
ax.text(295, 96.7-0.45, "96.7%", ha="right", size=10, color=COST, weight="bold")
ax.legend(frameon=False, loc="lower left", fontsize=9.5)
save(fig, "03_delivery.png")

# 4. memory subsystem --------------------------------------------------------
fig, ax = frame("Memory-subsystem cost (same bytes)", "Moving identical data, ROS2 copies it; axon references it in place.")
counters = ["cache-\nreferences", "cache-\nmisses", "instructions"]
ax_v = [131e6, 15.0e6, 4.9e9]; ro_v = [925e6, 128.2e6, 24.8e9]; mult = ["7.0×", "8.6×", "5.1×"]
y = range(len(counters)); h = 0.34
def human(v): return f"{v/1e9:.1f} B" if v >= 1e9 else f"{v/1e6:.0f} M"
ax.barh([i + h/2 for i in y], ax_v, h, color=AXON, label="axon", zorder=3)
ax.barh([i - h/2 for i in y], ro_v, h, color=COST, label="ROS2", zorder=3)
ax.set_xscale("log"); ax.set_xlim(5e6, 6e10)
ax.set_yticks(list(y)); ax.set_yticklabels(counters)
ax.grid(axis="y", lw=0); ax.grid(axis="x", color=GRID, lw=1)
ax.set_xlabel("count (log)")
for i in y:
    ax.text(ax_v[i]*1.15, i + h/2, human(ax_v[i]), va="center", size=9, color=INK)
    ax.text(ro_v[i]*1.15, i - h/2, f"{human(ro_v[i])}  ·  {mult[i]}", va="center", size=9, color=INK)
ax.legend(frameon=False, loc="lower right", fontsize=9.5)
save(fig, "04_memory.png")

# 5. syscalls ----------------------------------------------------------------
fig, ax = frame("Data-path syscalls", "axon's only transport syscalls are the one-time FD handshake; each frame is a shared-memory store.")
bars = ax.bar(["axon", "ROS2 (Fast-RTPS)"], [404, 10187], 0.5, color=[AXON, COST], zorder=3)
ax.set_ylabel("total syscalls (≈964 frames)")
barlabel(ax, bars, lambda h: f"{int(h):,}")
ax.set_ylim(0, 11800)
ax.text(1, 10187*0.5, "25×", ha="center", size=13, weight="bold", color="white")
ax.text(0.03, 0.90, "per-frame transport syscalls\naxon ~0   vs   ROS2 ~0.5",
        transform=ax.transAxes, ha="left", va="top", size=10, color=MUTED)
save(fig, "05_syscalls.png")

# 6. RT page faults ----------------------------------------------------------
fig, ax = frame("RT page faults stay flat", "Run 20× the frames and minor faults are identical: the RT loop adds 0 faults/frame.")
bars = ax.bar(["100 frames\n(1×)", "2000 frames\n(20×)"], [2191, 2191], 0.5, color=AXON, zorder=3)
ax.set_ylabel("minor page faults")
barlabel(ax, bars, lambda h: f"{int(h):,}")
ax.set_ylim(0, 2700)
ax.annotate("identical → 0 added / frame\n(major faults: 0)", xy=(1, 2191), xytext=(0.5, 1500),
            ha="center", size=10.5, weight="bold", color=INK,
            arrowprops=dict(arrowstyle="-", color=MUTED, lw=1))
save(fig, "06_pagefault.png")

# 7. VLM handoff (hero) ------------------------------------------------------
fig, ax = frame("VLM encoder→LLM handoff", "Cross-process embedding handoff. axon is O(1); a host round-trip is O(size).")
mb = [0.5, 1.7, 8, 34]
ax_l, na_l, mult = [78, 106, 96, 102], [97, 274, 975, 3710], ["1.2×", "2.6×", "10.1×", "36.3×"]
ax.plot(mb, ax_l, "-o", color=AXON, lw=2.6, ms=7, label="axon (dma-buf share)", zorder=3)
ax.plot(mb, na_l, "--s", color=COST, lw=2.6, ms=7, label="naive host round-trip", zorder=3)
ax.set_xscale("log"); ax.set_yscale("log")
ax.set_xlabel("embedding size (MB, log)"); ax.set_ylabel("p50 latency (µs, log)")
ax.set_xlim(0.35, 55); ax.set_ylim(50, 6000)
ax.xaxis.set_major_formatter(ticker.FuncFormatter(lambda v, _: f"{v:g}"))
ax.set_xticks(mb)
for xx, yy, m in zip(mb, na_l, mult):
    ax.text(xx, yy*1.25, m, ha="center", size=10, weight="bold", color=INK)
ax.text(34, 102*0.6, "~0.1 ms flat", ha="right", size=9.5, color=AXON, weight="bold")
ax.legend(frameon=False, loc="upper left", fontsize=9.5)
save(fig, "07_vlm_handoff.png")

print("done ->", OUT)
