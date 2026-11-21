#!/usr/bin/env python3
"""Figures for "What axon's numbers support" (docs/whitepapers/axon-performance).

Every value below is copied from a reviewed table in the repository; the source is named next to
each block. The Jetson camera numbers are read from the camera white paper's numbers.json, which
make_figures.py there computes from the raw per-frame CSVs.
"""
import json, os
import matplotlib
matplotlib.rcParams["svg.hashsalt"] = "axon"
matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = os.path.dirname(os.path.abspath(__file__))
FIG = os.path.join(HERE, "figures")
CAM = json.load(open(os.path.join(HERE, "..", "jetson-camera-e2e", "numbers.json")))
plt.rcParams.update({"font.size": 9, "font.family": "DejaVu Sans", "axes.spines.top": False, "axes.spines.right": False})
AX, ROS, IOX, RB, CI, CY, ZN = "#1f6fb2", "#c4651c", "#2e8b57", "#8e5ea2", "#b8860b", "#7f8c8d", "#a0a0a0"
WIN, MOD, PAR, LOSS = "#1f6fb2", "#6fa8dc", "#b0b7bf", "#c4651c"

def save(fig, name):
    fig.savefig(os.path.join(FIG, name), metadata={"Date": None})
    plt.close(fig)

def cam(cell, mode, stage="capture_to_inferred"):
    return CAM[f"{cell}/{mode}"][stage]["p50"] / 1e3

# ---- benchmarks/ros2_cpp/README.md "GPU → GPU against the other GPU zero-copy transports" (p50 µs) ----
SIZES = ["147 KiB", "1 MiB", "4 MiB"]
GPU = {"default": {"ROS 2 loaned + data-sharing": [46, 162, 484], "rosidl::Buffer (official)": [123, 124, 124],
                   "ros2_cuda_ipc": [124, 124, 127], "axon": [24, 24, 26]},
       "MPS": {"ROS 2 loaned + data-sharing": [41, 169, 479], "rosidl::Buffer (official)": [42, 42, 42],
               "ros2_cuda_ipc": [39, 37, 35], "axon": [24, 24, 25]}}
# same file, "When the consumer runs a kernel" (1 MiB, p50 µs): copy/kernel x default/MPS
CONSUMER = {"axon": [24, 129, 27, 23], "ros2_cuda_ipc": [124, 122, 41, 44], "rosidl::Buffer": [126, 124, 56, 49]}
# same file, "1 → N fan-out", GPU 1 MiB
FAN_N = [1, 2, 4, 8]
FAN = {"ROS 2 loaned": ([154.3, 204.8, 296.2, 449.7], [0.0167, 0.0340, 0.0959, 0.2564]),
       "axon": ([24.9, 26.7, 29.0, 37.9], [0.0031, 0.0047, 0.0083, 0.0208])}
# same file, "Payload size, iceoryx2 and the other RMWs (host)" (p50 µs, clock not held)
PAY = ["64 B", "8.6 KB", "147 KiB", "1 MiB", "4 MiB", "16 MiB", "64 MiB"]
PAYB = [64, 8640, 150528, 1048576, 4194304, 16777216, 67108864]
HOST = {"axon (4 buffers)": [0.3, 0.4, 4.3, 15.4, 59.2, 474, 3271], "iceoryx2 0.10": [0.6, 0.6, 5.2, 16.5, 59.3, 238, 2348],
        "Fast DDS loaned + data-sharing": [6.3, 5.8, 22.4, 33.9, 75.4, 304, 3050],
        "Cyclone + iceoryx, loaned": [4.7, 4.8, 17.4, 30.9, 73.8, None, None],
        "zenoh SHM (serialized)": [17.4, 18.6, 63.0, 125.5, 344.9, 1495, None]}
# README headline / #51: 16 MiB with the producer clock held: axon 1 / 2 / 4 buffers vs iceoryx2
HELD16 = {"axon, 1 buffer": 220, "axon, 2 buffers": 230, "axon, 4 buffers": 373, "iceoryx2": 219}
# README "Measured Results", multi-stream at 297 MB/s (median of 3)
MS = ["rclcpp UInt8MultiArray", "ROS 2 zero-copy", "axon, 1 kHz loop", "axon, wait_any"]
MS_CPU = [0.077, 0.038, 0.015, 0.019]
MS_P50 = [103, 44, 557, 29]
MS_MISS = [15.6, 3.66, 0.87, 1.31]
# benchmarks/ros2_cpp/README.md "Tail latency under load (host)", 1 MiB p50, the two runs (low, high)
TAIL = {"idle": {"axon": (23, 26), "iceoryx2": (20, 24), "ROS 2 loaned": (35, 40)},
        "CPU load (pinned)": {"axon": (32, 33), "iceoryx2": (26, 29), "ROS 2 loaned": (548, 584)},
        "memory load (pinned)": {"axon": (173, 179), "iceoryx2": (196, 198), "ROS 2 loaned": (284, 312)},
        "CPU load, unpinned": {"axon": (321, 495), "iceoryx2": (510, 706), "ROS 2 loaned": (1545, 1837)}}
# same file, frame-clock probe (16 MiB, 30 Hz host producer), p50 / p90 µs (upper end of the two runs)
CLOCK = [("no limit", 430, 549), ("CpuLatencyHold 1 µs", 213, 419), ("polling (0 µs)", 212, 221), ("2 ms pre-wake spin", 211, 216)]
# docs/hardware-verification.md §7, Jetson AGX Xavier 1 kHz cyclictest worst case (µs): idle, load
RT = [("stock kernel, 30 W", 1138, 1263), ("PREEMPT_RT, 30 W", 1145, 1229), ("RT + jetson_clocks", 1191, 1160),
      ("RT + CPUs 4-7 isolated, 30 W", 133, 226), ("RT + isolated, MAXN + jetson_clocks", 74, 148)]

# ---------------- Figure 1: the claim map ----------------
claims = [  # (label, factor = other / axon, colour)
    ("GPU→GPU 4 MiB vs ROS 2 zero-copy", 484 / 26),
    ("GPU 1 MiB, 1→8 subscribers vs ROS 2 zero-copy", 449.7 / 37.9),
    ("GPU→GPU 1 MiB vs ROS 2 zero-copy", 162 / 24),
    ("GPU vs rosidl::Buffer, copy-out consumer, no MPS", 126 / 24),
    ("GPU vs rosidl::Buffer, kernel consumer, with MPS", 49 / 23),
    ("multi-stream CPU vs ROS 2 zero-copy", 0.038 / 0.019),
    ("host 1 MiB vs ROS 2 zero-copy", 33.9 / 15.4),
    ("multi-stream camera p50 vs ROS 2 zero-copy", 44 / 29),
    ("host 4 MiB vs ROS 2 zero-copy", 75.4 / 59.2),
    ("Jetson camera→inference vs pageable copy (OAK-D, locked)", cam("oak_jetson_clocks_warm", "copy") / cam("oak_jetson_clocks_warm", "zerocopy")),
    ("host 1 MiB vs iceoryx2", 16.5 / 15.4),
    ("Jetson camera→inference vs mapped copy (OAK-D, locked)", cam("oak_jetson_clocks_warm_mapped", "copy") / cam("oak_jetson_clocks_warm_mapped", "zerocopy")),
    ("GPU vs rosidl::Buffer, kernel consumer, no MPS", 124 / 129),
    ("host 16 MiB vs iceoryx2 (4-buffer ring, clock held)", 219 / 373),
]
claims.sort(key=lambda c: -c[1])
def colour(f):
    return WIN if f >= 3 else MOD if f >= 1.2 else PAR if f >= 0.9 else LOSS
fig, ax = plt.subplots(figsize=(7.4, 4.6))
for i, (lab, f) in enumerate(claims):
    ax.barh(i, f, color=colour(f), height=0.62, left=0)
    ax.text(f * 1.04, i, f"{f:.2f}×", va="center", fontsize=7.5)
ax.set_xscale("log")
ax.axvline(1, color="#555", lw=0.8)
ax.set_yticks(range(len(claims)), [c[0] for c in claims], fontsize=7.5)
ax.invert_yaxis()
ax.set_xlim(0.4, 40)
ax.set_xticks([0.5, 1, 2, 5, 10, 20], ["0.5×", "1×", "2×", "5×", "10×", "20×"])
ax.set_xlabel("other / axon (median latency or CPU; > 1 = axon better, log scale)")
from matplotlib.patches import Patch
ax.legend(handles=[Patch(color=WIN, label="≥ 3×"), Patch(color=MOD, label="1.2-3×"), Patch(color=PAR, label="parity (0.9-1.2×)"),
                   Patch(color=LOSS, label="axon slower")], frameon=False, fontsize=7, loc="lower right")
fig.tight_layout()
save(fig, "f1_claim_map.svg")

# ---------------- Figure 2: GPU→GPU vs payload, default and MPS ----------------
fig, axes = plt.subplots(1, 2, figsize=(7.2, 2.8), sharey=True)
cols = {"ROS 2 loaned + data-sharing": ROS, "rosidl::Buffer (official)": RB, "ros2_cuda_ipc": CI, "axon": AX}
for ax, key, title in zip(axes, ["default", "MPS"], ["one CUDA context per process (default)", "with CUDA MPS"]):
    for name, v in GPU[key].items():
        ax.plot(SIZES, v, marker="o", color=cols[name], label=name, lw=1.6 if name == "axon" else 1.2)
    ax.set_title(title, fontsize=9)
    ax.set_xlabel("payload")
    ax.set_yscale("log")
axes[0].set_ylabel("p50 latency, µs (log)")
fig.legend(*axes[0].get_legend_handles_labels(), frameon=False, fontsize=7.5, ncol=4, loc="upper center")
fig.tight_layout(rect=(0, 0, 1, 0.88))
save(fig, "f2_gpu_payload.svg")

# ---------------- Figure 3: consumer type x MPS ----------------
fig, ax = plt.subplots(figsize=(7.2, 2.7))
groups = ["copy-out consumer\ndefault", "kernel consumer\ndefault", "copy-out consumer\nMPS", "kernel consumer\nMPS"]
w = 0.26
for j, (name, col) in enumerate((("axon", AX), ("ros2_cuda_ipc", CI), ("rosidl::Buffer", RB))):
    xs = [i + (j - 1) * w for i in range(4)]
    ax.bar(xs, CONSUMER[name], width=w, color=col, label=name)
    for x, v in zip(xs, CONSUMER[name]):
        ax.text(x, v + 2, str(v), ha="center", fontsize=6.5)
ax.set_xticks(range(4), groups, fontsize=7.5)
ax.set_ylabel("p50 latency, µs (GPU 1 MiB)")
ax.legend(frameon=False, fontsize=7.5, ncol=3, loc="upper right")
fig.tight_layout()
save(fig, "f3_consumer_mps.svg")

# ---------------- Figure 4: host payload sweep ----------------
fig, ax = plt.subplots(figsize=(7.2, 3.0))
hcols = {"axon (4 buffers)": AX, "iceoryx2 0.10": IOX, "Fast DDS loaned + data-sharing": ROS, "Cyclone + iceoryx, loaned": CY,
         "zenoh SHM (serialized)": ZN}
for name, v in HOST.items():
    pts = [(b, y) for b, y in zip(PAYB, v) if y is not None]
    ax.plot([p[0] for p in pts], [p[1] for p in pts], marker="o", ms=3.5, color=hcols[name], label=name, lw=1.6 if name.startswith("axon") else 1.1)
ax.scatter([16777216] * 2, [HELD16["axon, 1 buffer"], HELD16["axon, 4 buffers"]], marker="x", color=AX, zorder=5)
ax.annotate("clock held: 1 buffer 220,\n4 buffers 373 (iceoryx2 219)", (16777216, 220), xytext=(1.3e5, 1300), fontsize=7,
            arrowprops=dict(arrowstyle="-", color="#888", lw=0.7))
ax.set_xscale("log"); ax.set_yscale("log")
ax.set_xticks(PAYB, PAY, fontsize=7.5)
ax.set_ylabel("p50 latency, µs (log)")
ax.set_xlabel("payload (host → host)")
ax.legend(frameon=False, fontsize=7, loc="upper left")
fig.tight_layout()
save(fig, "f4_host_payload.svg")

# ---------------- Figure 5: fan-out ----------------
fig, (a1, a2) = plt.subplots(1, 2, figsize=(7.2, 2.6))
for name, col in (("ROS 2 loaned", ROS), ("axon", AX)):
    a1.plot(FAN_N, FAN[name][0], marker="o", color=col, label=name)
    a2.plot(FAN_N, FAN[name][1], marker="o", color=col, label=name)
a1.set_title("p50 latency, µs", fontsize=9); a2.set_title("CPU, producer + all subscribers (cores)", fontsize=9)
for a in (a1, a2):
    a.set_xticks(FAN_N); a.set_xlabel("subscribers (GPU 1 MiB)")
a1.legend(frameon=False, fontsize=7.5)
fig.tight_layout()
save(fig, "f5_fanout.svg")

# ---------------- Figure 6: multi-stream ----------------
fig, axes = plt.subplots(1, 3, figsize=(7.4, 2.6))
mcols = [ZN, ROS, MOD, AX]
for ax, vals, title in zip(axes, (MS_CPU, MS_P50, MS_MISS), ("CPU (cores)", "camera p50 latency (µs)", "cache misses (M/s)")):
    ax.bar(range(4), vals, color=mcols)
    ax.set_xticks(range(4), ["UInt8\nMultiArray", "ROS 2\nzero-copy", "axon\n1 kHz", "axon\nwait_any"], fontsize=6.8)
    ax.set_title(title, fontsize=8.5)
    for i, v in enumerate(vals):
        ax.text(i, v, f"{v:g}", ha="center", va="bottom", fontsize=6.5)
fig.tight_layout()
save(fig, "f6_multistream.svg")

# ---------------- Figure 7: tail under load (1 MiB p50, two runs as a range) ----------------
fig, ax = plt.subplots(figsize=(7.2, 2.8))
conds = list(TAIL)
w = 0.26
for j, (name, col) in enumerate((("axon", AX), ("iceoryx2", IOX), ("ROS 2 loaned", ROS))):
    xs = [i + (j - 1) * w for i in range(len(conds))]
    lo = [TAIL[c][name][0] for c in conds]; hi = [TAIL[c][name][1] for c in conds]
    ax.bar(xs, hi, width=w, color=col, alpha=0.35)
    ax.bar(xs, lo, width=w, color=col, label=name)
ax.set_yscale("log")
ax.set_xticks(range(len(conds)), conds, fontsize=7.5)
ax.set_ylabel("p50, µs (log); dark = lower run, light = upper")
ax.legend(frameon=False, fontsize=7.5, ncol=3, loc="upper left")
fig.tight_layout()
save(fig, "f7_tail_load.svg")

# ---------------- Figure 8: low-rate producer clock ----------------
fig, ax = plt.subplots(figsize=(7.2, 2.5))
labels = [c[0] for c in CLOCK]
ax.bar([i - 0.18 for i in range(4)], [c[1] for c in CLOCK], width=0.36, color=AX, label="p50")
ax.bar([i + 0.18 for i in range(4)], [c[2] for c in CLOCK], width=0.36, color=MOD, label="p90")
ax.set_xticks(range(4), labels, fontsize=7.5)
ax.set_ylabel("µs (16 MiB, 30 Hz, host)")
ax.legend(frameon=False, fontsize=7.5)
fig.tight_layout()
save(fig, "f8_lowrate_clock.svg")

# ---------------- Figure 9: Jetson camera -> inference ----------------
fig, axes = plt.subplots(1, 2, figsize=(7.4, 2.9), sharey=True)
rows = [("webcam480", "zerocopy", "", "webcam, zero-copy"), ("webcam480", "copy", "", "webcam, pageable copy"),
        ("oak", "zerocopy", "_mapped", "OAK-D, zero-copy"), ("oak", "copy", "_mapped", "OAK-D, mapped copy"),
        ("oak", "copy", "_pinned", "OAK-D, pinned copy"), ("oak", "copy", "", "OAK-D, pageable copy")]
for ax, cl, title in zip(axes, ("default", "jetson_clocks"), ("default clocks (30 W)", "jetson_clocks")):
    for i, (c, m, suf, lab) in enumerate(rows):
        cell = f"{c}_{cl}_warm{suf}"
        v = cam(cell, m)
        ax.barh(i, v, color=AX if m == "zerocopy" else ROS if suf == "" else MOD, height=0.6)
        ax.text(v, i, f" {v:.2f}", va="center", fontsize=7)
    ax.set_yticks(range(len(rows)), [r[3] for r in rows], fontsize=7.5)
    ax.set_title(title, fontsize=9)
    ax.set_xlabel("capture → inferred, median ms")
    ax.set_xlim(0, ax.get_xlim()[1] * 1.15)
axes[0].invert_yaxis()
fig.tight_layout()
save(fig, "f9_camera.svg")

# ---------------- Figure 10: Jetson RT jitter ----------------
fig, ax = plt.subplots(figsize=(7.2, 2.6))
ax.barh([i - 0.18 for i in range(5)], [r[1] for r in RT], height=0.36, color=AX, label="idle")
ax.barh([i + 0.18 for i in range(5)], [r[2] for r in RT], height=0.36, color=ROS, label="CPU load")
ax.axvline(100, color="#555", lw=0.8, ls="--")
ax.text(105, 4.45, "100 µs target", fontsize=7)
ax.set_yticks(range(5), [r[0] for r in RT], fontsize=7.5)
ax.invert_yaxis()
ax.set_xlabel("worst-case wake-up latency, µs (1 kHz cyclictest, AGX Xavier)")
ax.legend(frameon=False, fontsize=7.5, loc="lower right")
fig.tight_layout()
save(fig, "f10_rt_jitter.svg")

print("claims:", [(c, round(f, 2)) for c, f in claims])
