#!/usr/bin/env python3
"""Plot the credit-BDP PATTERN sweep (results_credit_patterns.csv).

Figure: credit_patterns.{pdf,png}, 3 rows x 4 cols.

  Row 1: per-pattern utilization vs bandwidth (lat = 200 us) on the
         FULLMESH fabric -- every pair a DIRECT 1-hop link, so the
         per-chunk credit RTT is uniformly ser + 2*lat and the
         window-sharing effect is isolated with no hop-count confound.
  Row 2: same panels on the SWITCHED fabric -- every pair 2 hops through
         the switch, which adds a switch traversal to the credit RTT.
  Row 3: W=1 cross-pattern comparisons (fullmesh + switched), W=1 vs
         latency (fullmesh), and the key-numbers panel.

Patterns:
  - p2p:     one dedicated window for one flow -> the clean W-scaling law.
  - incast:  7 INDEPENDENT per-sender windows converging on one receiver.
  - fanout:  ONE shared per-endpoint window across 7 flows (1 VC per
             endpoint) -> the sender serializes ALL flows through W.
  - alltoall: fanout per rank, all ranks at once -- at W=1 the shared
             window dominates and alltoall == fanout.

Palette: the validated Okabe-Ito 5-hue set (W series); pattern series use
its first 4 hues (a subset of a passing categorical palette). Contrast
WARN is relieved by the table view (CSV + README). Static matplotlib: no
hover layer.
"""
import csv
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = Path(__file__).resolve().parent

BANDWIDTHS = [100, 200, 400, 800, 1600]
LATENCIES_US = [0.5, 10, 50, 100, 200]
CREDITS = [1, 2, 4, 8, 24]
PATTERNS = ["p2p", "incast", "fanout", "alltoall"]
TOPOS = ["fullmesh", "switched"]
PATTERN_LABEL = {"p2p": "1-to-1 (p2p)", "incast": "many-to-1 (incast, 7→1)",
                 "fanout": "1-to-many (fanout, 1→7)",
                 "alltoall": "all-to-all (56 flows)"}
TOPO_LABEL = {"fullmesh": "fullmesh (every pair a direct 1-hop link)",
              "switched": "switched fabric (every pair 2 hops)"}
W_COLORS = ["#0072B2", "#E69F00", "#009E73", "#D55E00", "#CC79A7"]
P_COLORS = ["#0072B2", "#E69F00", "#009E73", "#D55E00"]
REF_COLOR = "#7f7f7f"
LAT_US = 200000


def load():
    by_key = {}
    with (HERE / "results_credit_patterns.csv").open() as f:
        for r in csv.DictReader(f):
            if r["status"] != "complete":
                continue
            by_key[(r["pattern"], r["topology"], int(r["bandwidthGbps"]),
                    int(r["latencyNs"]), int(r["vcCredits"]))] = float(r["simTimeNs"])
    return by_key


def util(by_key, pattern, topo, bw, lat, w):
    t_w = by_key.get((pattern, topo, bw, lat, w))
    t_u = by_key.get((pattern, topo, bw, lat, 100000000))
    if t_w is None or t_u is None or t_w == 0:
        return None
    return t_u / t_w


def series(by_key, pattern, topo, w, lat=LAT_US):
    xs, ys = [], []
    for bw in BANDWIDTHS:
        u = util(by_key, pattern, topo, bw, lat, w)
        if u is not None:
            xs.append(bw)
            ys.append(u)
    return xs, ys


def main():
    by_key = load()
    fig, axes = plt.subplots(3, 4, figsize=(18, 11), constrained_layout=True)

    # Rows 1-2: per-pattern utilization vs bandwidth at lat=200us.
    for row, topo in enumerate(TOPOS):
        for i, pat in enumerate(PATTERNS):
            ax = axes[row][i]
            for c, w in zip(W_COLORS, CREDITS):
                xs, ys = series(by_key, pat, topo, w)
                ax.plot(xs, ys, "o-", lw=2, ms=6, color=c, label=f"W = {w}")
            ax.axhline(1.0, ls="--", lw=1.5, color=REF_COLOR)
            ax.set_ylim(0, 1.05)
            ax.set_xlabel("link bandwidth (Gbps)")
            if i == 0:
                ax.set_ylabel(f"{TOPO_LABEL[topo]}\nutilization = T$_∞$ / T$_W$",
                              fontsize=9)
            w1 = util(by_key, pat, topo, 1600, LAT_US, 1)
            title = PATTERN_LABEL[pat]
            if w1:
                title += f"\nlat=200 µs  (W=1 @1600G: {w1:.3f})"
            ax.set_title(title, fontsize=10)
            ax.grid(True, alpha=0.3)
            if row == 0 and i == 0:
                ax.legend(fontsize=8, loc="upper right")

    # Row 3 left: W=1 across patterns, switched.
    ax = axes[2][0]
    for c, pat in zip(P_COLORS, PATTERNS):
        xs, ys = series(by_key, pat, "switched", 1)
        ax.plot(xs, ys, "o-", lw=2, ms=6, color=c, label=PATTERN_LABEL[pat])
    ax.set_ylim(0, 1.05)
    ax.set_xlabel("link bandwidth (Gbps)")
    ax.set_ylabel("utilization (W = 1)")
    ax.set_title("W=1, switched, lat=200 µs: shared vs dedicated\n"
                 "2-hop credit RTT (switch traversal) shifts every knee",
                 fontsize=10)
    ax.grid(True, alpha=0.3)
    ax.legend(fontsize=8)

    # Row 3 second: W=1 across patterns, fullmesh.
    ax = axes[2][1]
    for c, pat in zip(P_COLORS, PATTERNS):
        xs, ys = series(by_key, pat, "fullmesh", 1)
        ax.plot(xs, ys, "o-", lw=2, ms=6, color=c, label=PATTERN_LABEL[pat])
    ax.set_ylim(0, 1.05)
    ax.set_xlabel("link bandwidth (Gbps)")
    ax.set_ylabel("utilization (W = 1)")
    ax.set_title("W=1, fullmesh, lat=200 µs: uniform 1-hop credit RTT\n"
                 "incast == p2p exactly; fanout/alltoall 7x slower", fontsize=10)
    ax.grid(True, alpha=0.3)
    ax.legend(fontsize=8)

    # Row 3 third: W=1 vs latency at 800G, fullmesh.
    ax = axes[2][2]
    for c, pat in zip(P_COLORS, PATTERNS):
        xs, ys = [], []
        for lat in LATENCIES_US:
            u = util(by_key, pat, "fullmesh", 800, lat * 1000, 1)
            if u is not None:
                xs.append(lat)
                ys.append(u)
        ax.plot(xs, ys, "o-", lw=2, ms=6, color=c, label=PATTERN_LABEL[pat])
    ax.set_xscale("log")
    ax.set_ylim(0, 1.05)
    ax.set_xlabel("one-way latency (µs, log)")
    ax.set_ylabel("utilization (W = 1)")
    ax.set_title("W=1, fullmesh, 800 G: utilization vs latency\n(the knee "
                 "scales with the credit RTT)", fontsize=10)
    ax.grid(True, alpha=0.3, which="both")
    ax.legend(fontsize=8)

    # Row 3 right: key numbers.
    ax = axes[2][3]
    ax.axis("off")
    lines = ["Raw times @800 G, lat=200 µs, W=1 (ms)\n"]
    t_p2p = by_key.get(("p2p", "fullmesh", 800, LAT_US, 1), 0) / 1e6
    for topo in TOPOS:
        for pat in PATTERNS:
            t = by_key.get((pat, topo, 800, LAT_US, 1), 0) / 1e6
            ratio = t / t_p2p if (t_p2p and topo == "fullmesh") else None
            extra = f"  ({ratio:.2f}x p2p)" if ratio else ""
            lines.append(f"{topo:8s} {PATTERN_LABEL[pat]:26s} {t:8.2f}{extra}")
    lines += [
        "",
        "BDP law per FLOW: W·chunk/(ser + 2·lat).",
        "One VC per endpoint -> fanout and alltoall",
        "share ONE window across their flows, so the",
        "sender serializes 224 chunks through W:",
        "W=1 fullmesh = 7.1x p2p (uniform 1-hop RTT;",
        "the switched fabric adds a switch traversal",
        "to every credit RTT and doubles the p2p",
        "cycle).",
    ]
    ax.text(0.02, 0.95, "\n".join(lines), va="top", fontsize=9,
            family="monospace")

    fig.suptitle("Credit flow control across traffic patterns on the Su fabric "
                 "(8-GPU, 256 MiB per flow, fullmesh + switched):\n"
                 "window × chunk vs bandwidth-delay product — dedicated "
                 "(p2p/incast) vs shared (fanout/alltoall) credit windows")
    for ext in ("pdf", "png"):
        fig.savefig(HERE / f"credit_patterns.{ext}", dpi=150)
    print("wrote credit_patterns.pdf/.png")


if __name__ == "__main__":
    main()
