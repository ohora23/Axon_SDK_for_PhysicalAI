#!/usr/bin/env python3
"""Figures and the numbers table for the Jetson camera -> inference white paper.

Reads the per-frame CSVs in instrumentation/jetson/e2e/results/ and writes figures/*.svg and
numbers.json next to this file. Every number in whitepaper.html comes from numbers.json.
"""
import csv, glob, json, os
import matplotlib
matplotlib.rcParams["svg.hashsalt"] = "axon"
matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = os.path.dirname(os.path.abspath(__file__))
RES = os.path.join(HERE, "..", "..", "..", "instrumentation", "jetson", "e2e", "results")
CELLS = ["default_warm", "jetson_clocks_warm", "default_nowarm", "jetson_clocks_nowarm"]
MODES = ["zerocopy", "copy"]
STAGES = {"capture_to_publish": ("t_cap", "t_pub"), "publish_to_received": ("t_pub", "t_view"),
          "h2d": ("t_view", "t_gpu"), "kernel": ("t_gpu", "t_pre"), "inference": ("t_pre", "t_inf"),
          "received_to_inferred": ("t_view", "t_inf"), "capture_to_inferred": ("t_cap", "t_inf")}

def rows(cell, mode):
    out = []
    for f in sorted(glob.glob(os.path.join(RES, cell, f"e2e_{mode}_r*.csv"))):
        out += list(csv.DictReader(open(f)))
    return out

def us(r, a, b):
    return (int(r[b]) - int(r[a])) / 1e3

def pct(v, p):
    v = sorted(v)
    return v[min(len(v) - 1, int(len(v) * p))]

data, num = {}, {}
for c in CELLS:
    for m in MODES:
        rs = rows(c, m)
        data[(c, m)] = {k: [us(r, a, b) for r in rs] for k, (a, b) in STAGES.items()}
        num[f"{c}/{m}"] = {"n": len(rs), "runs": len(glob.glob(os.path.join(RES, c, f"e2e_{m}_r*.csv"))),
                           "seqlock_retries": sum(int(r["seqlock_retries"]) for r in rs),
                           **{k: {q: round(pct(v, p), 1) for q, p in (("p50", .5), ("p90", .9), ("p99", .99))} | {"max": round(max(v), 1)}
                              for k, v in data[(c, m)].items()}}
json.dump(num, open(os.path.join(HERE, "numbers.json"), "w"), indent=1)

plt.rcParams.update({"font.size": 9, "font.family": "DejaVu Sans", "axes.spines.top": False, "axes.spines.right": False})
C_ZC, C_CP = "#1f6fb2", "#c4651c"

# Fig 2: median stage breakdown, receive -> inferred, warm cells
fig, axes = plt.subplots(1, 2, figsize=(7.2, 2.7))
for ax, c, title in zip(axes, ["default_warm", "jetson_clocks_warm"], ["default clocks (30 W)", "jetson_clocks"]):
    parts = [("h2d", "H2D copy", "#c4651c"), ("kernel", "pre-processing", "#7f9db9"), ("inference", "inference", "#3b5b7a")]
    for i, m in enumerate(MODES):
        left = 0
        for k, label, col in parts:
            w = pct(data[(c, m)][k], .5) / 1e3
            ax.barh(i, w, left=left, color=col, label=label if i == 0 else None, height=0.55)
            left += w
        ax.text(left, i, f"  {left:.2f} ms", va="center", fontsize=8)
    ax.set_yticks([0, 1], ["zero-copy", "copy"])
    ax.invert_yaxis()
    ax.set_title(title, fontsize=9)
    ax.set_xlabel("median, ms (received → inferred)")
    ax.set_xlim(0, ax.get_xlim()[1] * 1.18)
fig.legend(*axes[0].get_legend_handles_labels(), frameon=False, fontsize=7.5, ncol=3, loc="upper center")
fig.tight_layout(rect=(0, 0, 1, 0.9))
fig.savefig(os.path.join(HERE, "figures", "fig2_breakdown.svg"), metadata={"Date": None})

# Fig 3: CDF of received -> inferred, warm cells
fig, axes = plt.subplots(1, 2, figsize=(7.2, 2.5))
for ax, c, title in zip(axes, ["default_warm", "jetson_clocks_warm"], ["default clocks (30 W)", "jetson_clocks"]):
    for m, col, label in (("zerocopy", C_ZC, "zero-copy"), ("copy", C_CP, "copy")):
        v = sorted(x / 1e3 for x in data[(c, m)]["received_to_inferred"])
        ax.plot(v, [(i + 1) / len(v) for i in range(len(v))], color=col, label=label, lw=1.4)
    ax.set_title(title, fontsize=9)
    ax.set_xlabel("received → inferred, ms")
    ax.set_ylabel("fraction of frames")
axes[0].legend(frameon=False, fontsize=8)
fig.tight_layout()
fig.savefig(os.path.join(HERE, "figures", "fig3_cdf.svg"), metadata={"Date": None})

# Fig 4: where the GPU wake-up cost lands (medians, us), no warm-up vs warm-up
fig, ax = plt.subplots(figsize=(7.2, 2.9))
labels, groups = [], []
for c, tag in (("default", "default clocks"), ("jetson_clocks", "jetson_clocks")):
    for m, ml in (("zerocopy", "zero-copy"), ("copy", "copy")):
        for w in ("nowarm", "warm"):
            labels.append(f"{ml}\n{tag}\n{'no warm-up' if w == 'nowarm' else 'warm-up'}")
            d = data[(f"{c}_{w}", m)]
            groups.append([pct(d["publish_to_received"], .5), pct(d["h2d"], .5), pct(d["kernel"], .5)])
x = range(len(labels))
bottom = [0] * len(labels)
for j, (lab, col) in enumerate((("publish → received", "#c9ced4"), ("H2D copy", "#c4651c"), ("pre-processing kernel", "#1f6fb2"))):
    vals = [g[j] for g in groups]
    ax.bar(x, vals, bottom=bottom, color=col, label=lab, width=0.6)
    bottom = [b + v for b, v in zip(bottom, vals)]
ax.set_xticks(list(x), labels, fontsize=6.5)
ax.set_ylabel("median, µs")
fig.legend(*ax.get_legend_handles_labels(), frameon=False, fontsize=7.5, ncol=3, loc="upper center")
fig.tight_layout(rect=(0, 0, 1, 0.9))
fig.savefig(os.path.join(HERE, "figures", "fig4_wake.svg"), metadata={"Date": None})
print(json.dumps({k: {s: v[s]["p50"] for s in ("h2d", "kernel", "inference", "received_to_inferred", "capture_to_inferred", "publish_to_received", "capture_to_publish")} for k, v in num.items()}, indent=0))
