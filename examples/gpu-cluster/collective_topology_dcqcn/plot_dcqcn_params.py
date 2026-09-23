#!/usr/bin/env python3
"""Plot DCQCN parameter-sensitivity results.

Row 1: collective completion time heatmaps (results_dcqcn_params.csv — host
ncclring allreduce, 16n×8g, 256 MiB, the only collective scenario where ECN
fires). Row 2: delivered Gbps heatmaps in the congested sustained-flow
regime (results_dcqcn_rand.csv — randTraffic on 4:1 leafspine). Bottom
panel: the one axis that responds at all — ECN-marked packets vs marking
threshold (both regimes). Writes dcqcn_params.{pdf,png}."""
import csv
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

HERE = Path(__file__).resolve().parent

ECN_THRESHOLDS = [1, 4, 8, 16, 32]
GINITS = [0.25, 0.5, 0.75]
TFASTS = [10000, 50000, 100000]


def load(name, value_field):
    grid = {}
    base = None
    with (HERE / name).open() as f:
        for r in csv.DictReader(f):
            if r["status"] != "complete":
                continue
            if r["runType"] == "dcqcn0":
                base = float(r[value_field])
                continue
            grid[(int(r["ecnThreshold"]), float(r["dcqcnGInit"]),
                  int(r["dcqcnTFastNs"]))] = float(r[value_field])
    return grid, base


def load_ecn(name):
    per_threshold = {}
    with (HERE / name).open() as f:
        for r in csv.DictReader(f):
            if r["status"] != "complete" or r["runType"] == "dcqcn0":
                continue
            e = int(r["ecnThreshold"])
            per_threshold.setdefault(e, []).append(int(r["ecnMarkedPackets"] or 0))
    return {e: sum(v) / len(v) for e, v in per_threshold.items()}


def heat(ax, grid, tfast, title, fmt, cmap, vmin, vmax):
    m = np.full((len(GINITS), len(ECN_THRESHOLDS)), np.nan)
    for (e, g, tf), v in grid.items():
        if tf == tfast:
            m[GINITS.index(g), ECN_THRESHOLDS.index(e)] = v
    im = ax.imshow(m, cmap=cmap, aspect="auto", origin="lower",
                   vmin=vmin, vmax=vmax)
    for i in range(len(GINITS)):
        for j in range(len(ECN_THRESHOLDS)):
            v = m[i, j]
            if not np.isnan(v):
                frac = 0.0 if vmax == vmin else (v - vmin) / (vmax - vmin)
                color = "white" if frac > 0.55 else "black"
                ax.text(j, i, fmt(v), ha="center", va="center",
                        fontsize=8, color=color)
    ax.set_xticks(range(len(ECN_THRESHOLDS)), [str(e) for e in ECN_THRESHOLDS])
    ax.set_yticks(range(len(GINITS)), [str(g) for g in GINITS])
    ax.set_xlabel("ECN threshold (pkts)")
    ax.set_ylabel("GInit")
    ax.set_title(title, fontsize=10)
    return im


def main():
    grid_t, base_t = load("results_dcqcn_params.csv", "timeUs")
    grid_d, base_d = load("results_dcqcn_rand.csv", "deliveredGbps")
    ecn_coll = load_ecn("results_dcqcn_params.csv")
    ecn_rand = load_ecn("results_dcqcn_rand.csv")

    fig, axes = plt.subplots(3, 3, figsize=(12, 9),
                             gridspec_kw={"height_ratios": [1, 1, 0.9]},
                             constrained_layout=True)
    for c, tfast in enumerate(TFASTS):
        im_t = heat(axes[0][c], grid_t, tfast, f"TFast = {tfast // 1000} µs",
                    lambda v: f"{v:.0f}", "viridis", 11700, 11710)
        im_d = heat(axes[1][c], grid_d, tfast, f"TFast = {tfast // 1000} µs",
                    lambda v: f"{v:.0f}", "viridis", 5130, 5180)
    fig.colorbar(im_t, ax=axes[0], shrink=0.85, pad=0.02,
                 label="allreduce completion (µs)")
    fig.colorbar(im_d, ax=axes[1], shrink=0.85, pad=0.02,
                 label="delivered Gbps (4:1 leafspine)")

    # Bottom: the one responding axis — ECN-marked packets vs threshold.
    ax = axes[2][0]
    ax.plot(ECN_THRESHOLDS, [ecn_coll[e] for e in ECN_THRESHOLDS],
            "o-", lw=2, ms=6, color="#1f77b4", label="collective (host ncclring)")
    ax.set_xlabel("ECN threshold (pkts)")
    ax.set_ylabel("ECN-marked packets")
    ax.set_title("ECN marks vs marking threshold", fontsize=10)
    ax.grid(True, alpha=0.3)
    ax.legend(fontsize=8)
    ax = axes[2][1]
    ax.plot(ECN_THRESHOLDS, [ecn_rand[e] for e in ECN_THRESHOLDS],
            "s-", lw=2, ms=6, color="#d62728", label="randTraffic (4:1 leafspine)")
    ax.set_xlabel("ECN threshold (pkts)")
    ax.set_ylabel("ECN-marked packets")
    ax.set_title("ECN marks vs marking threshold", fontsize=10)
    ax.grid(True, alpha=0.3)
    ax.legend(fontsize=8)
    axes[2][2].axis("off")
    axes[2][2].text(0.02, 0.95,
                    "Key numbers\n"
                    f"  collective: all 46 runs = {base_t:.0f} µs\n"
                    f"    (identical to dcqcn=0)\n"
                    f"  4:1 delivered: {min(grid_d.values()):.0f}"
                    f"–{max(grid_d.values()):.0f} Gbps\n"
                    f"    (dcqcn=0: {base_d:.0f}, ±0.4 %)\n"
                    f"  ECN marks: −2 % (collective) / −11 %\n"
                    "    (randTraffic) from thr 1→32\n"
                    "  GInit / TFast: zero effect",
                    va="top", fontsize=9)

    fig.suptitle("DCQCN parameter sensitivity: GInit / TFast are outcome-neutral in the lossless model;\n"
                 "only the marking threshold moves the ECN counters "
                 f"(dcqcn=0 baselines: {base_t:.0f} µs, {base_d:.0f} Gbps)")
    for ext in ("pdf", "png"):
        fig.savefig(HERE / f"dcqcn_params.{ext}", dpi=150)
    print("wrote dcqcn_params.pdf/.png")


if __name__ == "__main__":
    main()
