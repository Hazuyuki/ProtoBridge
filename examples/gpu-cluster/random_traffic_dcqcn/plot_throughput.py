#!/usr/bin/env python3
"""Figure: delivered throughput vs injection rate per So topology (DCQCN on).

Reads results.csv (produced by run_sweep.py). Two panels:
  (a) aggregate delivered throughput (Gbps) vs injection rate;
  (b) ECN-marked packets vs injection rate (DCQCN congestion signal --
      nonzero only where an EthernetSwitch sits in the So path AND the
      offered load actually contends: the gateway topologies
      (ring/torus2d/dragonfly) and switchless fullmesh never mark, so
      DCQCN is structurally inert there even at saturation).

Outputs throughput_vs_injection.pdf/.png next to the CSV.
"""

import csv
from collections import defaultdict
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = Path(__file__).resolve().parent
CSV = HERE / "results.csv"

STYLES = {
    "fullmesh":   dict(color="#1f77b4", marker="o"),
    "ring":       dict(color="#d62728", marker="s"),
    "host":       dict(color="#2ca02c", marker="^"),
    "tor":        dict(color="#ff7f0e", marker="v"),
    "rail":       dict(color="#9467bd", marker="D"),
    "leafspine":  dict(color="#8c564b", marker="P"),
    "torus2d":    dict(color="#e377c2", marker="X"),
    "dragonfly":  dict(color="#7f7f7f", marker="*"),
}


def main():
    data = defaultdict(list)  # topo -> [(rate, delivered, offered, ecn)]
    with open(CSV) as f:
        for row in csv.DictReader(f):
            if row["status"] != "complete":
                continue
            data[row["topology"]].append((
                float(row["rate"]),
                float(row["deliveredGbps"]),
                float(row["offeredGbps"]),
                int(row["ecnMarkedPackets"] or 0),
            ))

    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(11, 4.2))

    # Offered-load reference line (ideal: delivered == offered).
    any_topo = next(iter(data.values()))
    rates = sorted(p[0] for p in any_topo)
    offered_by_rate = {p[0]: p[2] for p in any_topo}
    ax1.plot(rates, [offered_by_rate[r] for r in rates],
             ls="--", color="black", lw=1, label="offered (ideal)")

    for topo, style in STYLES.items():
        pts = sorted(data.get(topo, []))
        if not pts:
            continue
        ax1.plot([p[0] for p in pts], [p[1] for p in pts],
                 label=topo, lw=1.5, ms=5, **style)
        ax2.plot([p[0] for p in pts], [p[3] for p in pts],
                 label=topo, lw=1.5, ms=5, **style)

    ax1.set_xlabel("injection rate (fraction of 400G line rate per GPU)")
    ax1.set_ylabel("delivered throughput (Gbps, aggregate)")
    ax1.set_title("(a) Throughput vs injection rate\n"
                  "uniform-random cross-node, 8n x 4g, RoCE 400G, DCQCN on")
    ax1.grid(alpha=0.3)
    ax1.legend(fontsize=8, ncol=2)

    ax2.set_xlabel("injection rate (fraction of 400G line rate per GPU)")
    ax2.set_ylabel("ECN-marked packets (count)")
    ax2.set_title("(b) DCQCN congestion signal\n"
                  "(zero where no EthernetSwitch is in the So path)")
    ax2.grid(alpha=0.3)
    ax2.legend(fontsize=8, ncol=2)

    fig.tight_layout()
    for ext in ("pdf", "png"):
        out = HERE / f"throughput_vs_injection.{ext}"
        fig.savefig(out, dpi=150)
        print(f"wrote {out}")


if __name__ == "__main__":
    main()
