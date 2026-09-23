#!/usr/bin/env python3
"""Plot collective completion time + bus bandwidth vs data size per topology
from results.csv (see run_sweep.py). When results_ncclring.csv exists (see
run_ncclring_sweep.py), the allreduce panels also carry the ncclring rows as
dashed lines (solid = hierarchical). Writes collective_vs_topology.pdf/.png."""
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
COLLECTIVES = ["allreduce", "alltoall"]

# Analytic So wire bound for iso-hardware "one 400G NIC per GPU" (an ideal
# contention-free non-blocking fabric at the switched shapes' NIC count).
# Inter-node bytes through each GPU's NIC, as a fraction of the per-GPU data
# size S: hierarchical allreduce is rail-parallel (inter RS + inter AG move
# 2*(n-1)/(g*n)*S each way); alltoall moves the (N-g)/N inter-node fraction.
# busbw bound = busbw_factor / wire_fraction * B (S cancels, so it is a
# horizontal asymptote on the busbw panels and a diagonal on the log-log
# time panels). Fullmesh may exceed it: it has 15 dedicated 400G links per
# GPU, not one NIC.
NUM_NODES, GPUS, NRANKS = 16, 8, 128
B_GBPS = 400.0


def wire_fraction(coll):
    if coll == "allreduce":
        return 2.0 * (NUM_NODES - 1) / (GPUS * NUM_NODES)
    return (NRANKS - GPUS) / NRANKS


def busbw_factor(coll):
    if coll == "allreduce":
        return 2.0 * (NRANKS - 1) / NRANKS
    return (NRANKS - 1) / NRANKS


def wire_bound_us(coll, size_bytes):
    return wire_fraction(coll) * size_bytes * 8.0 / B_GBPS / 1e3  # ns -> us


def wire_bound_gbps(coll):
    return busbw_factor(coll) / wire_fraction(coll) * B_GBPS


def load():
    rows = []
    with (HERE / "results.csv").open() as f:
        for r in csv.DictReader(f):
            if r["status"] != "complete":
                continue
            rows.append(r)
    return rows


def load_ncclring():
    path = HERE / "results_ncclring.csv"
    if not path.exists():
        return []
    rows = []
    with path.open() as f:
        for r in csv.DictReader(f):
            if r["status"] != "complete":
                continue
            rows.append(r)
    return rows


def main():
    rows = load()
    nccl_rows = load_ncclring()
    fig, axes = plt.subplots(2, 2, figsize=(11, 8))
    for i, coll in enumerate(COLLECTIVES):
        ax_t, ax_bw = axes[i]
        bw_max = 0.0
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
            bw_max = max(bw_max, max(p[2] for p in pts))
            # ncclring overlay (allreduce only): same colors, dashed.
            npts = sorted(
                ((int(r["dataSize"]), float(r["timeUs"]), float(r["busbwGbps"]))
                 for r in nccl_rows
                 if r["collective"] == coll and r["topology"] == topo),
                key=lambda p: p[0])
            if npts:
                nsizes = [p[0] / (1 << 20) for p in npts]
                ax_t.plot(nsizes, [p[1] for p in npts], ls="--", lw=1.2, ms=4,
                          alpha=0.8, **style)
                ax_bw.plot(nsizes, [p[2] for p in npts], ls="--", lw=1.2, ms=4,
                           alpha=0.8, **style)
                bw_max = max(bw_max, max(p[2] for p in npts))
        # Analytic So wire bound (iso-hardware, 1x400G NIC per GPU):
        # a lower-bound diagonal on the time panel, a horizontal upper bound
        # on the busbw panel (annotated instead when it sits off-scale).
        all_sizes = sorted({int(r["dataSize"]) for r in rows if r["collective"] == coll})
        ax_t.plot([s / (1 << 20) for s in all_sizes],
                  [wire_bound_us(coll, s) for s in all_sizes],
                  "k:", lw=1.2, label="So wire bound\n(1×400G NIC/GPU)")
        bound = wire_bound_gbps(coll)
        if bound <= bw_max * 1.3:
            ax_bw.axhline(bound, color="k", ls=":", lw=1.2)
            ax_bw.annotate(f"So wire bound {bound:.0f} Gbps (1×400G NIC/GPU)",
                           xy=(0.02, bound), xycoords=("axes fraction", "data"),
                           xytext=(0, 4), textcoords="offset points", fontsize=8)
        else:
            ax_bw.annotate(f"So wire bound ≈{bound:.0f} Gbps (off-scale):\n"
                           "intra-node + startup bound, not the So fabric",
                           xy=(0.02, 0.97), xycoords="axes fraction",
                           va="top", fontsize=8)
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
    if nccl_rows:
        axes[0][1].legend(handles=[
            Line2D([], [], color="k", ls="-", lw=1.5, label="hierarchical"),
            Line2D([], [], color="k", ls="--", lw=1.2, label="ncclring"),
        ], fontsize=8, loc="center left", title="allreduce algorithm", title_fontsize=8)
    fig.suptitle("Collectives vs So topology, 16 nodes × 8 GPUs, RoCE 400G, DCQCN on", y=0.995)
    fig.tight_layout()
    for ext in ("pdf", "png"):
        fig.savefig(HERE / f"collective_vs_topology.{ext}", dpi=150)
    print("wrote collective_vs_topology.pdf/.png")


if __name__ == "__main__":
    main()
