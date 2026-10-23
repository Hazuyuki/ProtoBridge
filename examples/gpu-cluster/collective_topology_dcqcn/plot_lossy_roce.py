#!/usr/bin/env python3
"""Plot the lossy-RoCE DCQCN sweep results.

Panel 1 (top-left): loss severity — hierarchical-allreduce completion time
vs voqDropThreshold for the three arms (no CC / default DCQCN / aggressive
GInit 0.125), with the lossless reference line. The DCQCN benefit exists
only at the severe threshold (8): at 16 the 344 drops cost 0.4% and at 32
nothing drops.

Panel 2 (top-right): one-at-a-time parameter sensitivity — each knob's
completion-time swing (max-min over its 5 values) as % of the default
run. The multiplicative-step knobs (GInit/GDecay/AlphaFloor) move ~8%;
the recovery timers (TFast/FastCycles) are flat.

Panel 3 (bottom-left): the two live knobs in detail — completion time vs
GInit and vs GDecay value (U-shaped: too small a step under-backs-off,
too large over-throttles and drags retransmissions).

Panel 4 (bottom-right): the saturation contrast — randTraffic delivered
Gbps under the same lossy regime (drops are ~free under a saturated
bottleneck; CC arms identical), vs the completion-time regime above.

Reads results_lossy_roce.csv + results_lossy_rand.csv; writes
lossy_roce_dcqcn.{pdf,png}."""
import csv
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = Path(__file__).resolve().parent

OAT_AXES = ["dcqcnGInit", "dcqcnGDecay", "dcqcnTFastNs", "dcqcnFastCycles",
            "dcqcnTAiNs", "dcqcnRai", "dcqcnAlphaFloor"]
AXIS_LABELS = {
    "dcqcnGInit": "GInit",
    "dcqcnGDecay": "GDecay",
    "dcqcnTFastNs": "TFast",
    "dcqcnFastCycles": "FastCycles",
    "dcqcnTAiNs": "TAi",
    "dcqcnRai": "RAI",
    "dcqcnAlphaFloor": "AlphaFloor",
}


def load_lossy():
    lossless = None
    dcqcn0 = {}
    default = None
    oat = {a: {} for a in OAT_AXES}
    dropthr = {}  # (drop, param) -> time
    with (HERE / "results_lossy_roce.csv").open() as f:
        for r in csv.DictReader(f):
            if r["status"] != "complete":
                continue
            t = float(r["timeUs"])
            drop = int(r["voqDropThreshold"])
            if r["arm"] == "lossless":
                lossless = t
            elif r["arm"] == "dcqcn0":
                dcqcn0[drop] = t
            elif r["arm"] == "default":
                default = t
            elif r["arm"] == "oat":
                oat[r["param"]][float(r["value"])] = t
            elif r["arm"] == "dropthr":
                dropthr[(drop, r["param"])] = (t, int(r["switchVoqDrops"] or 0),
                                               int(r["retransmittedPackets"] or 0))
    return lossless, dcqcn0, default, oat, dropthr


def load_rand():
    rows = {}
    with (HERE / "results_lossy_rand.csv").open() as f:
        for r in csv.DictReader(f):
            if r["status"] != "complete":
                continue
            rows[(r["arm"], int(r["voqDropThreshold"]))] = (
                float(r["randDeliveredGbps"] or 0),
                int(r["switchVoqDrops"] or 0))
    return rows


def main():
    lossless, dcqcn0, default, oat, dropthr = load_lossy()
    rand = load_rand()

    fig, axes = plt.subplots(2, 2, figsize=(12, 8), constrained_layout=True)

    # --- Panel 1: loss severity -------------------------------------------------
    ax = axes[0][0]
    drops = [8, 16, 32]
    series = [
        ("no CC", [(d, dropthr[(d, "dcqcn0")][0]) for d in drops], "C3", "o"),
        ("DCQCN default", [(d, dropthr[(d, "default")][0]) for d in drops], "C0", "s"),
        ("DCQCN GInit=0.125", [(d, dropthr[(d, "dcqcnGInit")][0]) for d in drops], "C2", "^"),
    ]
    for label, pts, color, marker in series:
        xs, ys = zip(*pts)
        ax.plot(xs, ys, color=color, marker=marker, linewidth=2, label=label)
        for x, y in pts:
            ax.annotate(f"{y / 1000:.2f}ms", (x, y), textcoords="offset points",
                        xytext=(0, 7), ha="center", fontsize=7, color=color)
    ax.axhline(lossless, color="gray", linestyle=":", linewidth=1.5)
    ax.text(31, lossless, f"lossless {lossless / 1000:.2f}ms", ha="right",
            va="bottom", fontsize=7, color="gray")
    ax.set_xlabel("voqDropThreshold (pkts; ecnThreshold=4)")
    ax.set_ylabel("completion time (µs)")
    ax.set_title("Loss severity: 2n×8g hierarchical allreduce 256 MiB\n"
                 "lossy RoCE (no PFC), mark-before-drop", fontsize=10)
    ax.legend(fontsize=8)
    ax.grid(alpha=0.3)

    # --- Panel 2: OAT swing bars ------------------------------------------------
    ax = axes[0][1]
    labels, swings = [], []
    for a in OAT_AXES:
        vals = list(oat[a].values())
        swing = (max(vals) - min(vals)) / default * 100.0
        labels.append(AXIS_LABELS[a])
        swings.append(swing)
    order = sorted(range(len(labels)), key=lambda i: -swings[i])
    labels = [labels[i] for i in order]
    swings = [swings[i] for i in order]
    bars = ax.bar(labels, swings, color="C0", width=0.6)
    for b, s in zip(bars, swings):
        ax.annotate(f"{s:.1f}%", (b.get_x() + b.get_width() / 2, s),
                    textcoords="offset points", xytext=(0, 3), ha="center",
                    fontsize=8)
    ax.set_ylabel("completion-time swing (max−min, % of default)")
    ax.set_title("One-at-a-time parameter sensitivity (drop=8)\n"
                 "step-size knobs bite, recovery timers do not", fontsize=10)
    ax.set_ylim(0, max(swings) * 1.2)
    ax.grid(axis="y", alpha=0.3)

    # --- Panel 3: the two live knobs --------------------------------------------
    ax = axes[1][0]
    for a, color in (("dcqcnGInit", "C0"), ("dcqcnGDecay", "C1")):
        pts = sorted(oat[a].items())
        xs, ys = zip(*pts)
        ax.plot(xs, ys, color=color, marker="o", linewidth=2,
                label=AXIS_LABELS[a])
        for x, y in pts:
            ax.annotate(f"{y / 1000:.2f}", (x, y), textcoords="offset points",
                        xytext=(0, 7), ha="center", fontsize=7, color=color)
    ax.axhline(dcqcn0[8], color="C3", linestyle="--", linewidth=1.2)
    ax.text(0.02, dcqcn0[8], f"no CC {dcqcn0[8] / 1000:.2f}ms", fontsize=7,
            color="C3", va="bottom")
    ax.set_xlabel("parameter value (dimensionless)")
    ax.set_ylabel("completion time (µs)")
    ax.set_title("The multiplicative backoff step (U-shaped)\n"
                 "drop=8, defaults elsewhere", fontsize=10)
    ax.legend(fontsize=8)
    ax.grid(alpha=0.3)

    # --- Panel 4: saturation contrast -------------------------------------------
    ax = axes[1][1]
    arms = ["dcqcn0", "default", "ginit"]
    arm_labels = ["no CC", "DCQCN default", "GInit=0.125"]
    width = 0.25
    for i, (arm, label) in enumerate(zip(arms, arm_labels)):
        ds = [8, 16, 64]
        ys = [rand[(arm, d)][0] for d in ds]
        ax.bar([j + (i - 1) * width for j in range(len(ds))], ys,
               width=width, color=f"C{i}", label=label)
    ax.axhline(rand[("lossless", 0)][0], color="gray", linestyle=":",
               linewidth=1.5)
    ax.text(2.4, rand[("lossless", 0)][0], "lossless 642.3", ha="right",
            va="bottom", fontsize=7, color="gray")
    ax.set_xticks(range(3), ["drop=8", "drop=16", "drop=64"])
    ax.set_ylabel("delivered Gbps (randTraffic 4:1 leafspine)")
    ax.set_title("Saturation contrast: drops are ~free for goodput\n"
                 "2n×4g leafspine, injection 0.8 (~4.4k drops, ~300k retx "
                 "per run)", fontsize=10)
    ax.legend(fontsize=8)
    ax.set_ylim(600, 660)
    ax.grid(axis="y", alpha=0.3)

    for path in (HERE / "lossy_roce_dcqcn.pdf", HERE / "lossy_roce_dcqcn.png"):
        fig.savefig(path, dpi=150)
        print(f"wrote {path}")


if __name__ == "__main__":
    main()
