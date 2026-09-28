#!/usr/bin/env python3
"""Plot the lossy-RoCE grid sweep in the SAME form as dcqcn_params.png
(the lossless figure): 3x3 with two heatmap rows + a response-curve row.

Row 1: allreduce completion (µs) heatmaps — GInit (rows) x ECN threshold
(cols), one column per TFast. In the lossless figure all 45 cells were
11702.3 (uniform); here the grid actually varies — the whole point.

Row 2: LLR retransmissions heatmaps on the same grid — the lossy
counterpart of the lossless figure's delivered-Gbps row (the metric the
regime makes move: each tail-drop costs a GBN window retransmission).

Bottom: the responding axes — completion and retransmissions vs marking
threshold (mean over the grid, min-max band), plus the key-numbers panel.

Reads results_lossy_grid.csv; writes lossy_roce_grid.{pdf,png}."""
import csv
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

HERE = Path(__file__).resolve().parent

ECN_THRESHOLDS = [1, 2, 4, 6, 8]
GINITS = [0.125, 0.25, 0.5]
TFASTS = [10000, 50000, 100000]


def load(field):
    grid = {}
    dcqcn0 = lossless = None
    with (HERE / "results_lossy_grid.csv").open() as f:
        for r in csv.DictReader(f):
            if r["status"] != "complete":
                continue
            v = float(r[field])
            if r["runType"] == "dcqcn0":
                dcqcn0 = v
            elif r["runType"] == "lossless":
                lossless = v
            else:
                grid[(int(r["ecnThreshold"]), float(r["dcqcnGInit"]),
                      int(r["dcqcnTFastNs"]))] = v
    return grid, dcqcn0, lossless


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
    ax.set_xlabel("ECN threshold (pkts; drop=8)")
    ax.set_ylabel("GInit")
    ax.set_title(title, fontsize=10)
    return im


def band(ax, grid, ylabel, label):
    per_e = {}
    for (e, g, tf), v in grid.items():
        per_e.setdefault(e, []).append(v)
    es = sorted(per_e)
    means = [np.mean(per_e[e]) for e in es]
    lo = [np.min(per_e[e]) for e in es]
    hi = [np.max(per_e[e]) for e in es]
    ax.fill_between(es, lo, hi, alpha=0.2, color="#1f77b4")
    ax.plot(es, means, "o-", lw=2, ms=6, color="#1f77b4", label=label)
    ax.set_xlabel("ECN threshold (pkts; drop=8)")
    ax.set_ylabel(ylabel)
    ax.grid(True, alpha=0.3)
    ax.legend(fontsize=8)


def main():
    grid_t, dcqcn0_t, lossless_t = load("timeUs")
    grid_r, _, _ = load("retransmittedPackets")

    t_vals = list(grid_t.values())
    t_min, t_max = min(t_vals), max(t_vals)
    r_vals = list(grid_r.values())
    r_min, r_max = min(r_vals), max(r_vals)

    fig, axes = plt.subplots(3, 3, figsize=(12, 9),
                             gridspec_kw={"height_ratios": [1, 1, 0.9]},
                             constrained_layout=True)
    for c, tfast in enumerate(TFASTS):
        im_t = heat(axes[0][c], grid_t, tfast, f"TFast = {tfast // 1000} µs",
                    lambda v: f"{v:.0f}", "viridis", t_min, t_max)
        im_r = heat(axes[1][c], grid_r, tfast, f"TFast = {tfast // 1000} µs",
                    lambda v: f"{v:.0f}", "viridis", r_min, r_max)
    fig.colorbar(im_t, ax=axes[0], shrink=0.85, pad=0.02,
                 label="allreduce completion (µs)")
    fig.colorbar(im_r, ax=axes[1], shrink=0.85, pad=0.02,
                 label="LLR retransmissions (pkts)")

    axes[2][0].set_title("Completion vs marking threshold (grid mean, min–max band)",
                         fontsize=10)
    band(axes[2][0], grid_t, "completion (µs)", "grid mean ± range")
    axes[2][0].axhline(dcqcn0_t, color="#d62728", linestyle="--", lw=1.5)
    axes[2][0].text(8, dcqcn0_t, f"no CC {dcqcn0_t:.0f}", fontsize=8,
                    color="#d62728", ha="right", va="bottom")
    axes[2][0].axhline(lossless_t, color="gray", linestyle=":", lw=1.5)
    axes[2][0].text(8, lossless_t, f"lossless {lossless_t:.0f}", fontsize=8,
                    color="gray", ha="right", va="top")

    axes[2][1].set_title("Retransmissions vs marking threshold", fontsize=10)
    band(axes[2][1], grid_r, "LLR retransmissions (pkts)", "grid mean ± range")
    axes[2][1].axhline(892, color="#d62728", linestyle="--", lw=1.5)
    axes[2][1].text(8, 892, "no CC 892", fontsize=8, color="#d62728",
                    ha="right", va="bottom")

    best = min(t_vals)
    worst = max(t_vals)
    best_key = min(grid_t, key=grid_t.get)
    # TFast swing: max within-(ecn, GInit) spread across the 3 TFast columns.
    per_eg = {}
    for (e, g, tf), v in grid_t.items():
        per_eg.setdefault((e, g), []).append(v)
    tfast_swing = max((max(v) - min(v)) for v in per_eg.values())
    recovery = 1 - (best - lossless_t) / (dcqcn0_t - lossless_t)
    axes[2][2].axis("off")
    axes[2][2].text(0.02, 0.95,
                    "Key numbers\n"
                    f"  lossless: {lossless_t:.0f} µs\n"
                    f"  lossy no CC: {dcqcn0_t:.0f} µs (+32 %)\n"
                    f"  grid: {best:.0f}–{worst:.0f} µs\n"
                    f"    (best recovers {recovery:.0%} of the drop tax)\n"
                    f"  best cell: ecn={best_key[0]}, GInit={best_key[1]},\n"
                    f"    TFast={best_key[2] // 1000} µs → {best:.0f} µs\n"
                    f"  late marks (thr 6) WORSE than no marks\n"
                    f"    (thr 8): 19.3 vs 18.9 ms mean\n"
                    f"  TFast: {tfast_swing / dcqcn0_t * 100:.0f} µs max swing\n"
                    f"    within a (ecn, GInit) column —\n"
                    f"    live only at early thresholds, flat at thr ≥ 4",
                    va="top", fontsize=9)

    fig.suptitle("DCQCN parameter sensitivity in the lossy-RoCE (no-PFC) regime "
                 "(drop=8, mark-before-drop):\n"
                 "the grid the lossless figure could not show — early marks + "
                 "GInit move completion; late marks (thr 6) backfire; "
                 "TFast live only at early thresholds")
    for ext in ("pdf", "png"):
        fig.savefig(HERE / f"lossy_roce_grid.{ext}", dpi=150)
    print(f"wrote lossy_roce_grid.pdf/.png")


if __name__ == "__main__":
    main()
