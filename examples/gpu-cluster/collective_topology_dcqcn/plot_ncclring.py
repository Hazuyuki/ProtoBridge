#!/usr/bin/env python3
"""Compare --algorithm=ncclring (real-NCCL global pipelined multi-channel ring)
against the default hierarchical allreduce schedule per topology.

Reads results.csv (hierarchical, run_sweep.py) and results_ncclring.csv
(run_ncclring_sweep.py); writes ncclring_vs_hierarchical.pdf/.png.
Solid lines = hierarchical, dashed = ncclring, colors match plot_collectives.py.
"""
import csv
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.lines import Line2D

HERE = Path(__file__).resolve().parent

STYLES = {
    "fullmesh":  dict(color="#1f77b4", marker="o"),
    "ring":      dict(color="#ff7f0e", marker="s"),
    "host":      dict(color="#2ca02c", marker="^"),
    "tor":       dict(color="#d62728", marker="v"),
    "rail":      dict(color="#9467bd", marker="D"),
    "leafspine": dict(color="#8c564b", marker="P"),
    "torus2d":   dict(color="#e377c2", marker="X"),
    "dragonfly": dict(color="#7f7f7f", marker="*"),
}


def load(name, coll="allreduce"):
    out = {}
    with (HERE / name).open() as f:
        for r in csv.DictReader(f):
            if r["status"] != "complete" or r["collective"] != coll:
                continue
            out.setdefault(r["topology"], []).append(
                (int(r["dataSize"]), float(r["timeUs"]), float(r["busbwGbps"])))
    for pts in out.values():
        pts.sort()
    return out


def main():
    hier = load("results.csv")
    nccl = load("results_ncclring.csv")
    fig, (ax_t, ax_bw) = plt.subplots(1, 2, figsize=(11, 4.5))
    for topo, style in STYLES.items():
        for data, ls in ((hier, "-"), (nccl, "--")):
            pts = data.get(topo)
            if not pts:
                continue
            sizes = [p[0] / (1 << 20) for p in pts]
            ax_t.plot(sizes, [p[1] for p in pts], ls=ls, lw=1.5, ms=5, **style)
            ax_bw.plot(sizes, [p[2] for p in pts], ls=ls, lw=1.5, ms=5, **style)
    ax_t.set_xscale("log", base=2)
    ax_t.set_yscale("log")
    ax_t.set_xlabel("data size per GPU (MiB)")
    ax_t.set_ylabel("completion time (µs)")
    ax_t.set_title("(a) allreduce — completion time")
    ax_t.grid(True, which="both", alpha=0.3)
    ax_bw.set_xscale("log", base=2)
    ax_bw.set_xlabel("data size per GPU (MiB)")
    ax_bw.set_ylabel("bus bandwidth (Gbps)")
    ax_bw.set_title("(b) allreduce — bus bandwidth")
    ax_bw.grid(True, which="both", alpha=0.3)
    topo_handles = [Line2D([], [], lw=1.5, ms=5, **s, label=t)
                    for t, s in STYLES.items()]
    algo_handles = [Line2D([], [], color="k", ls="-", label="hierarchical"),
                    Line2D([], [], color="k", ls="--", label="ncclring")]
    ax_t.legend(handles=topo_handles, fontsize=8, ncol=2)
    ax_bw.legend(handles=algo_handles, fontsize=8, loc="upper left")
    fig.suptitle("hierarchical vs ncclring allreduce, 16 nodes × 8 GPUs, RoCE 400G, DCQCN on",
                 y=0.995)
    fig.tight_layout()
    for ext in ("pdf", "png"):
        fig.savefig(HERE / f"ncclring_vs_hierarchical.{ext}", dpi=150)
    print("wrote ncclring_vs_hierarchical.pdf/.png")


if __name__ == "__main__":
    main()
