#!/usr/bin/env python3
"""
Plot the web-measured anchors gathered for the So-fabric comparison:

Figure 1 (srnic_fig11_comparison.{png,pdf}): SRNIC (NSDI'23) Figure 11
  measured curves (ConnectX-6 go-back-N, SRNIC selective repeat; digitized in
  srnic_fig11_cx6_reference.csv) vs the ns-3 two-armed sweep
  (eth_gbn_loss_sweep.json, llrMode=gobackn / llrMode=sack). Normalized
  goodput G(p)/G(0) per curve vs injected packet loss rate.

Figure 2 (crusoe_nccl_busbw.{png,pdf}): measured 4-node x 8xH100 (IB NDR,
  8x400G rails/node) all_reduce_perf output from
  https://saturncloud.io/blog/crusoe-nccl-cluster-validation/ -- algbw and
  busbw vs message size -- vs the ns-3 counterpart (4 nodes x 8 GPU,
  hierarchical allreduce, --interNodeBandwidth=3200). The 12.8x gap is the
  documented algorithm regime mismatch (NCCL runs NVLS intra-node phases at
  these sizes, our hierarchical injector's intra phases are the calibrated
  ring); ns-3 points exist only at 2/4 GiB (>=8 GiB overflows the stock ns-3
  uint32 Queue byte counter, queue.h:551). See doc/CALIBRATION.md.

Run:  python3 configs/calibration/plot_measured_anchors.py
"""

from __future__ import annotations

import csv
import json
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = Path(__file__).resolve().parent
REFERENCE = HERE / "srnic_fig11_cx6_reference.csv"
SWEEP = HERE / "eth_gbn_loss_sweep.json"

# saturncloud blog all_reduce_perf table (out-of-place columns):
# size(B), time(us), algbw(GB/s), busbw(GB/s)
CRUSOE_ROWS = [
    (2147483648, 5765.2, 372.56, 698.55),
    (4294967296, 11443.3, 375.30, 703.69),
    (8589934592, 22815.1, 376.53, 706.00),
    (17179869184, 45553.2, 377.13, 707.12),
    (34359738368, 91027.4, 377.47, 707.76),
]
# ns-3 counterpart (our data): 4 nodes x 8 GPU hierarchical allreduce,
#   ./build/scratch/ns3.47-gpu-cluster-sim-debug --numNodes=4 --gpusPerNode=8
#     --interNodeTopology=host --interNodeFabricType=RoCE
#     --interNodeBandwidth=3200 --collective=allreduce
#     --algorithm=hierarchical --dataSize=<size>
# size(B), simTimeUs. Only 2/4 GiB: >=8 GiB aborts on the stock ns-3 uint32
# Queue byte counter (queue.h:551 m_nBytes assert).
NS3_ROWS = [
    (2147483648, 74041.6),
    (4294967296, 147491.3),
]


def load_reference() -> dict[str, dict[float, float]]:
    ref: dict[str, dict[float, float]] = {}
    with open(REFERENCE) as f:
        for row in csv.DictReader(r for r in f if not r.startswith("#")):
            ref.setdefault(row["curve"], {})[float(row["loss_rate"])] = float(row["normalized"])
    return ref


def load_sweep() -> dict[str, dict[float, float]]:
    data = json.loads(SWEEP.read_text())
    out: dict[str, dict[float, float]] = {}
    for arm, rows in data["results"].items():
        out[arm] = {r["plr"]: r["normalized"] for r in rows if r.get("normalized") and r["plr"] > 0}
    return out


def fig_srnic() -> None:
    ref = load_reference()
    ns3 = load_sweep()
    fig, ax = plt.subplots(figsize=(6.4, 4.4))

    def curve(d: dict[float, float]):
        xs = sorted(d)
        return xs, [d[x] for x in xs]

    ax.plot(*curve(ref["cx6"]), "o-", color="#c0392b",
            label="CX-6 measured (go-back-N, SRNIC Fig. 11)")
    ax.plot(*curve(ns3["gobackn"]), "s--", color="#e67e22",
            label="ns-3 llrMode=gobackn (3-seed mean)")
    ax.plot(*curve(ref["srnic"]), "o-", color="#1f618d",
            label="SRNIC measured (selective repeat, Fig. 11)")
    ax.plot(*curve(ns3["sack"]), "s--", color="#2e86c1",
            label="ns-3 llrMode=sack (3-seed mean)")

    ax.set_xscale("log")
    ax.set_xlabel("injected packet loss rate")
    ax.set_ylabel("normalized goodput  G(p) / G(0)")
    ax.set_ylim(0, 1.05)
    ax.set_title("Loss recovery: measured 100G RNICs vs ns-3 LLR modes\n"
                 "(perftest 4 KB messages, CC disabled, uniform random loss)")
    ax.grid(True, which="both", alpha=0.3)
    ax.legend(fontsize=8, loc="lower left")

    ax.annotate("whole-message GBN window:\nearly collapse, ~0.50 floor",
                xy=(5e-3, 0.515), xytext=(1.5e-5, 0.32), fontsize=8,
                arrowprops=dict(arrowstyle="->", color="0.4"), color="0.25")

    fig.tight_layout()
    for ext in ("png", "pdf"):
        fig.savefig(HERE / f"srnic_fig11_comparison.{ext}", dpi=200)
    plt.close(fig)


def fig_crusoe() -> None:
    sizes = [r[0] / 2**30 for r in CRUSOE_ROWS]
    algbw = [r[2] for r in CRUSOE_ROWS]
    busbw = [r[3] for r in CRUSOE_ROWS]
    ns3_sizes = [r[0] / 2**30 for r in NS3_ROWS]
    ns3_algbw = [r[0] / r[1] / 1000.0 for r in NS3_ROWS]  # B/us -> GB/s

    fig, ax = plt.subplots(figsize=(6.4, 4.0))
    ax.plot(sizes, busbw, "o-", color="#1f618d", label="busbw measured (blog)")
    ax.plot(sizes, algbw, "s-", color="#e67e22", label="algbw measured (blog)")
    ax.plot(ns3_sizes, ns3_algbw, "D--", color="#27ae60",
            label="algbw ns-3 (hierarchical, ring-intra)")
    ax.set_xscale("log", base=2)
    ax.set_yscale("log")
    ax.set_xlabel("all-reduce message size (GiB)")
    ax.set_ylabel("bandwidth (GB/s)")
    ax.set_ylim(10, 1000)
    ax.set_title("NCCL all_reduce_perf, 4 nodes × 8 GPUs:\n"
                 "measured (saturncloud.io Crusoe blog, H100 IB NDR) vs ns-3")
    ax.grid(True, which="both", alpha=0.3)
    ax.legend(fontsize=8, loc="center right")

    ax.annotate("12.8× regime gap: NCCL runs NVLS intra-node\n"
                "phases here; ns-3 hierarchical uses the calibrated\n"
                "ring intra (≥8 GiB exceeds packet-level range)",
                xy=(ns3_sizes[1], ns3_algbw[1]), xytext=(2.05, 65), fontsize=8,
                arrowprops=dict(arrowstyle="->", color="0.4"), color="0.25")

    fig.tight_layout()
    for ext in ("png", "pdf"):
        fig.savefig(HERE / f"crusoe_nccl_busbw.{ext}", dpi=200)
    plt.close(fig)


if __name__ == "__main__":
    fig_srnic()
    fig_crusoe()
    print("wrote", HERE / "srnic_fig11_comparison.{png,pdf}")
    print("wrote", HERE / "crusoe_nccl_busbw.{png,pdf}")
