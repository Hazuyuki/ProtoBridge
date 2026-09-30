#!/usr/bin/env python3
"""Plot the credit-BDP PATTERN sweep at FIXED bandwidth: utilization vs the
credit window W, one line per one-way latency (results_credit_patterns.csv).

Figure: credit_patterns_wlat.{pdf,png}, 3 rows x 4 cols, bandwidth fixed at
BW_GBPS (default 800 G).

  Rows 1-2: one panel per traffic pattern (top row fullmesh fabric, second
            row switched), x = W credits, y = utilization = T_inf / T_W, one
            line per one-way latency 0.5-200 us. Reads as "how many credits
            does each pattern need at this latency to reach full
            utilization": the curves rise to 1.0 and the required W shifts
            right ~linearly with latency (the credit RTT).
  Row 3:    (a/b) utilization at W=CEIL_W (default 4) vs latency per
            pattern (fullmesh / switched) -- at a small window every pattern
            is window-bound and latency-responsive (CEIL_W=24 restores
            the "can it reach 1.0 at all" ceiling view); (c) key numbers;
            (d) min W for >=95% utilization.

Latency is an ordered magnitude, so the line series uses a sequential
single-hue ramp (light -> dark = 0.5 -> 200 us) with distinct markers, not
a categorical palette; identity is carried by the legend + the CSV table
view. Static matplotlib: no hover layer.
"""
import csv
import os
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = Path(__file__).resolve().parent

BW_GBPS = int(os.environ.get("BW_GBPS", "800"))
# Window shown in the bottom-row cross-pattern panels. Small (4) by default:
# at W=24 three of the four patterns are already saturated flat at 1.000 on
# the switched fabric, so only the capped curve moves -- at a small W every
# curve is window-bound and latency-responsive. Set CEIL_W=24 for the
# "can this pattern reach 1.0 at all" ceiling view.
CEIL_W = int(os.environ.get("CEIL_W", "4"))
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
# Sequential single-hue ramp: light -> dark = 0.5 -> 200 us one-way latency.
LAT_COLORS = ["#c6dbef", "#9ecae1", "#6baed6", "#3182bd", "#08519c"]
P_COLORS = ["#0072B2", "#E69F00", "#009E73", "#D55E00"]
REF_COLOR = "#7f7f7f"


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


def main():
    if BW_GBPS not in BANDWIDTHS:
        raise SystemExit(f"BW_GBPS must be one of {BANDWIDTHS}, got {BW_GBPS}")
    if CEIL_W not in CREDITS:
        raise SystemExit(f"CEIL_W must be one of {CREDITS}, got {CEIL_W}")
    by_key = load()
    fig, axes = plt.subplots(3, 4, figsize=(18, 11), constrained_layout=True)

    # Rows 1-2: utilization vs W, one line per latency, at fixed bandwidth.
    for row, topo in enumerate(TOPOS):
        for i, pat in enumerate(PATTERNS):
            ax = axes[row][i]
            for c, lat in zip(LAT_COLORS, LATENCIES_US):
                xs, ys = [], []
                for w in CREDITS:
                    u = util(by_key, pat, topo, BW_GBPS, lat * 1000, w)
                    if u is not None:
                        xs.append(w)
                        ys.append(u)
                ax.plot(xs, ys, "o-", lw=2, ms=6, color=c,
                        label=f"lat = {lat:g} µs")
            ax.axhline(1.0, ls="--", lw=1.5, color=REF_COLOR)
            ax.set_ylim(0, 1.05)
            ax.set_xscale("log")
            ax.set_xticks(CREDITS)
            ax.set_xticklabels([str(w) for w in CREDITS])
            ax.minorticks_off()
            ax.set_xlabel("credit window W (chunks in flight)")
            if i == 0:
                ax.set_ylabel(f"{TOPO_LABEL[topo]}\nutilization = T$_∞$ / T$_W$",
                              fontsize=9)
            # First latency where W=24 no longer reaches 1.0.
            flat = [lat for lat in LATENCIES_US
                    if (util(by_key, pat, topo, BW_GBPS, lat * 1000, 24) or 0) < 0.999]
            note = f"; W=24 < 1.0 from {min(flat):g} µs" if flat else ""
            ax.set_title(f"{PATTERN_LABEL[pat]}\n{BW_GBPS} G{note}", fontsize=10)
            ax.grid(True, alpha=0.3)
            if row == 0 and i == 0:
                ax.legend(fontsize=8, loc="lower right")

    # Row 3 (a/b): utilization at W=24 vs latency per pattern.
    for k, topo in enumerate(TOPOS):
        ax = axes[2][k]
        for c, pat in zip(P_COLORS, PATTERNS):
            xs, ys = [], []
            for lat in LATENCIES_US:
                u = util(by_key, pat, topo, BW_GBPS, lat * 1000, CEIL_W)
                if u is not None:
                    xs.append(lat)
                    ys.append(u)
            ax.plot(xs, ys, "o-", lw=2, ms=6, color=c, label=PATTERN_LABEL[pat])
        ax.axhline(1.0, ls="--", lw=1.5, color=REF_COLOR)
        ax.set_xscale("log")
        ax.set_ylim(0, 1.05)
        ax.set_xlabel("one-way latency (µs, log)")
        ax.set_ylabel(f"utilization (W = {CEIL_W})")
        extra = "\np2p == incast" if topo == "fullmesh" else ""
        ax.set_title(f"W={CEIL_W}, {BW_GBPS} G, {topo}:\n"
                     "window-bound — every pattern\nmoves with latency" + extra,
                     fontsize=10)
        ax.grid(True, alpha=0.3, which="both")
        ax.legend(fontsize=8)

    # Row 3 (c): key numbers.
    ax = axes[2][2]
    ax.axis("off")
    lines = [f"Utilization at {BW_GBPS} G, W = {CEIL_W}\n",
             "lat(µs)   0.5    10    50   100   200"]
    for topo in TOPOS:
        lines.append(f"--- {topo} ---")
        for pat in PATTERNS:
            us = [util(by_key, pat, topo, BW_GBPS, lat * 1000, CEIL_W) or float("nan")
                  for lat in LATENCIES_US]
            lines.append(f"{pat:9s}" + "".join(f"{u:6.2f}" for u in us))
    lines += [
        "",
        "W needed for full utilization shifts",
        "right ~linearly with latency (credit",
        "RTT); fanout/alltoall need ~W×flows",
        "credits (shared 1-VC window). Fullmesh",
        "T∞ is 7 links wide → fanout/alltoall",
        "capped even at W=24 (0.26/0.23).",
    ]
    ax.text(0.02, 0.95, "\n".join(lines), va="top", fontsize=7.5,
            family="monospace")

    # Row 3 (d): minimum W reaching 0.95 utilization vs latency (fullmesh).
    ax = axes[2][3]
    for c, pat in zip(P_COLORS, PATTERNS):
        xs, ys = [], []
        for lat in LATENCIES_US:
            for w in CREDITS:
                u = util(by_key, pat, "fullmesh", BW_GBPS, lat * 1000, w)
                if u is not None and u >= 0.95:
                    xs.append(lat)
                    ys.append(w)
                    break
        if xs:
            ax.plot(xs, ys, "o-", lw=2, ms=6, color=c, label=PATTERN_LABEL[pat])
    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.set_yticks(CREDITS)
    ax.set_yticklabels([str(w) for w in CREDITS])
    ax.minorticks_off()
    ax.set_xlabel("one-way latency (µs, log)")
    ax.set_ylabel("min W with utilization ≥ 0.95")
    ax.set_title(f"credits needed for ≥95% utilization, {BW_GBPS} G\n"
                 "(fullmesh; missing = not reached even at W=24)", fontsize=10)
    ax.grid(True, alpha=0.3, which="both")
    ax.legend(fontsize=8)

    fig.suptitle(f"Credit flow control across traffic patterns at FIXED "
                 f"bandwidth ({BW_GBPS} G, 8-GPU Su fabric, 256 MiB per "
                 f"flow):\n"
                 f"utilization vs credit window W and one-way latency — "
                 f"dedicated (p2p/incast) vs shared (fanout/alltoall) "
                 f"windows, fullmesh vs switched")
    for ext in ("pdf", "png"):
        fig.savefig(HERE / f"credit_patterns_wlat.{ext}", dpi=150)
    print(f"wrote credit_patterns_wlat.pdf/.png (bw={BW_GBPS}G)")


if __name__ == "__main__":
    main()
