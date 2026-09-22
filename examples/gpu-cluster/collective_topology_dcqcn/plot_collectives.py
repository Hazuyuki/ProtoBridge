#!/usr/bin/env python3
"""Plot collective completion time + bus bandwidth vs data size per topology
from results.csv (see run_sweep.py). Writes collective_vs_topology.pdf/.png."""
import csv
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

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
COLLECTIVES = ["allreduce", "alltoall"]


def load():
    rows = []
    with (HERE / "results.csv").open() as f:
        for r in csv.DictReader(f):
            if r["status"] != "complete":
                continue
            rows.append(r)
    return rows


def main():
    rows = load()
    fig, axes = plt.subplots(2, 2, figsize=(11, 8))
    for i, coll in enumerate(COLLECTIVES):
        ax_t, ax_bw = axes[i]
        for topo, style in STYLES.items():
            pts = sorted(
                ((int(r["dataSize"]), float(r["timeUs"]), float(r["busbwGbps"]))
                 for r in rows if r["collective"] == coll and r["topology"] == topo),
                key=lambda p: p[0])
            if not pts:
                continue
            sizes = [p[0] / (1 << 20) for p in pts]
            ax_t.plot(sizes, [p[1] for p in pts], label=topo, lw=1.5, ms=5, **style)
            ax_bw.plot(sizes, [p[2] for p in pts], label=topo, lw=1.5, ms=5, **style)
        ax_t.set_xscale("log", base=2)
        ax_t.set_yscale("log")
        ax_t.set_xlabel("data size per GPU (MiB)")
        ax_t.set_ylabel("completion time (µs)")
        ax_t.set_title(f"({'ab'[i]}) {coll} — completion time")
        ax_t.grid(True, which="both", alpha=0.3)
        ax_bw.set_xscale("log", base=2)
        ax_bw.set_xlabel("data size per GPU (MiB)")
        ax_bw.set_ylabel("bus bandwidth (Gbps)")
        ax_bw.set_title(f"({'cd'[i]}) {coll} — bus bandwidth")
        ax_bw.grid(True, which="both", alpha=0.3)
    axes[0][0].legend(fontsize=8, ncol=2)
    fig.suptitle("Hierarchical collectives, 16 nodes × 8 GPUs, RoCE 400G, DCQCN on", y=0.995)
    fig.tight_layout()
    for ext in ("pdf", "png"):
        fig.savefig(HERE / f"collective_vs_topology.{ext}", dpi=150)
    print("wrote collective_vs_topology.pdf/.png")


if __name__ == "__main__":
    main()
