#!/usr/bin/env python3
"""Plot the Su credit-window BDP sweep (results_credit_bdp.csv).

Figure: credit_bdp.{pdf,png}, four panels.

  (0,0) Completion vs bandwidth at 200 us one-way latency — one line per
        credit window W, plus the unlimited-credit reference (gray dashed).
        Small windows flatten: past the knee, more bandwidth does NOT buy
        shorter completion. This is the phenomenon under test.
  (0,1) Utilization (T_unlimited / T_W) vs bandwidth at 200 us — the same
        data as the knee curves: W=1 collapses to 21% at 1600 G, W>=8 stays
        at 100%. The knee bandwidth scales ~ W*chunk/(serialization+2*lat).
  (1,0) Utilization vs bandwidth for W=1, one line per latency — the knee
        also scales with latency (the credit return path pays 2*lat).
  (1,1) The oscillation itself: ConsumeCredit event rate over sim time for
        W=2 / 800 G / 200 us. Bursts drain the window, then the sender sits
        idle waiting for CREDIT packets to return over 2*lat before the
        next burst — a sawtooth whose idle gap ~= 2*lat (credit RTT).
        The trace is (re)generated with NS_LOG=CreditManager=level_debug
        and cached in sawtooth_trace.log.

Palette: Okabe-Ito derived, validated (CVD separation / chroma / lightness
all PASS); contrast WARN is relieved by the table view (results CSV + README
grid). Unlimited-credit is a neutral reference, not a series.
"""
import csv
import os
import re
import subprocess
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = Path(__file__).resolve().parent
BIN = HERE.parents[2] / "build" / "scratch" / "ns3.47-gpu-cluster-sim-debug"

BANDWIDTHS = [100, 200, 400, 800, 1600]
LATENCIES_US = [0.5, 10, 50, 100, 200]
CREDITS = [1, 2, 4, 8, 24]        # unlimited drawn separately (reference)
COLORS = ["#0072B2", "#E69F00", "#009E73", "#D55E00", "#CC79A7"]
REF_COLOR = "#7f7f7f"

# Sawtooth trace scenario (W=2, deep in the window-bound regime).
SAW_BW, SAW_LAT, SAW_CREDITS = 800, 200000, 2
TRACE = HERE / "sawtooth_trace.log"


def load():
    by_key = {}
    with (HERE / "results_credit_bdp.csv").open() as f:
        for r in csv.DictReader(f):
            if r["status"] != "complete":
                continue
            by_key[(int(r["bandwidthGbps"]), int(r["latencyNs"]),
                    int(r["vcCredits"]))] = float(r["simTimeNs"])
    return by_key


def series(by_key, lat_ns, field="time"):
    """Per-W series at one latency. field: 'time' (ns) or 'util'."""
    out = {}
    for w in CREDITS:
        xs, ys = [], []
        for bw in BANDWIDTHS:
            t_w = by_key.get((bw, lat_ns, w))
            t_u = by_key.get((bw, lat_ns, 100000000))
            if t_w is None or t_u is None:
                continue
            xs.append(bw)
            ys.append(t_w / 1e6 if field == "time" else t_u / t_w)
        out[w] = (xs, ys)
    xs, ys = [], []
    for bw in BANDWIDTHS:
        t_u = by_key.get((bw, lat_ns, 100000000))
        if t_u is not None:
            xs.append(bw)
            ys.append(t_u / 1e6)
    out["ref"] = (xs, ys)
    return out


def gen_trace():
    """Run the sawtooth scenario with credit logging; cache the trace."""
    if TRACE.exists():
        return TRACE.read_text()
    cmd = [
        str(BIN),
        "--numGpus=8", "--collective=allreduce", "--algorithm=ring",
        "--dataSize=268435456", f"--bandwidth={SAW_BW}",
        f"--latency={SAW_LAT}", f"--vcCredits={SAW_CREDITS}", "--RngRun=1",
    ]
    env = dict(os.environ, NS_LOG="CreditManager=level_debug|prefix_time")
    out = subprocess.run(cmd, capture_output=True, text=True, env=env,
                         timeout=600)
    text = out.stderr
    TRACE.write_text(text)
    return text


def sawtooth(text):
    """Parse ConsumeCredit timestamps -> per-10us-bin counts + burst period."""
    times = []
    for line in text.splitlines():
        m = re.match(r"\+(\d+)\.(\d+)s CreditManager:ConsumeCredit", line)
        if m:
            times.append(int(m.group(1)) * 1e9
                         + int(m.group(2).ljust(9, "0")))
    bin_ns = 10000
    bins = {}
    for t in times:
        bins[int(t // bin_ns)] = bins.get(int(t // bin_ns), 0) + 1
    # Burst period: median gap between non-zero bins (in us).
    nz = sorted(k for k, v in bins.items() if v > 0)
    gaps = [b - a for a, b in zip(nz, nz[1:])]
    period_us = sorted(gaps)[len(gaps) // 2] * bin_ns / 1e3 if gaps else 0
    return bins, period_us, len(times)


def main():
    by_key = load()
    fig, axes = plt.subplots(2, 2, figsize=(12, 8), constrained_layout=True)

    # (0,0) completion vs bandwidth at lat=200us
    s = series(by_key, 200000, "time")
    ax = axes[0][0]
    for c, w in zip(COLORS, CREDITS):
        ax.plot(*s[w], "o-", lw=2, ms=6, color=c, label=f"W = {w}")
    ax.plot(*s["ref"], "--", lw=2, color=REF_COLOR, label="unlimited credits")
    ax.set_yscale("log")
    ax.set_xlabel("link bandwidth (Gbps)")
    ax.set_ylabel("allreduce completion (ms, log)")
    ax.set_title("latency = 200 µs one-way: past the knee, bandwidth\n"
                 "cannot shorten completion (W caps in-flight bytes)",
                 fontsize=10)
    ax.grid(True, alpha=0.3)
    ax.legend(fontsize=8, ncol=2)

    # (0,1) utilization vs bandwidth at lat=200us
    s = series(by_key, 200000, "util")
    ax = axes[0][1]
    for c, w in zip(COLORS, CREDITS):
        ax.plot(*s[w], "o-", lw=2, ms=6, color=c, label=f"W = {w}")
    ax.set_ylim(0, 1.05)
    ax.set_xlabel("link bandwidth (Gbps)")
    ax.set_ylabel("utilization = T$_{∞}$ / T$_W$")
    ax.set_title("same data as utilization: W=1 → 21%, W≥8 → 100% "
                 "at 1600 G", fontsize=10)
    ax.grid(True, alpha=0.3)
    ax.legend(fontsize=8, ncol=2)

    # (1,0) utilization vs bandwidth, W=1, one line per latency
    ax = axes[1][0]
    for c, lat in zip(COLORS, LATENCIES_US):
        xs, ys = [], []
        for bw in BANDWIDTHS:
            t_w = by_key.get((bw, lat * 1000, 1))
            t_u = by_key.get((bw, lat * 1000, 100000000))
            if t_w and t_u:
                xs.append(bw)
                ys.append(t_u / t_w)
        ax.plot(xs, ys, "o-", lw=2, ms=6, color=c, label=f"lat = {lat:g} µs")
    ax.set_ylim(0, 1.05)
    ax.set_xlabel("link bandwidth (Gbps)")
    ax.set_ylabel("utilization (W = 1)")
    ax.set_title("W = 1: the knee also scales with latency\n"
                 "(credit return pays 2·lat)", fontsize=10)
    ax.grid(True, alpha=0.3)
    ax.legend(fontsize=8)

    # (1,1) sawtooth
    bins, period_us, n_events = sawtooth(gen_trace())
    ax = axes[1][1]
    ks = sorted(bins)
    xs = [k * 10 for k in ks]                      # us
    ys = [bins[k] for k in ks]
    ax.fill_between(xs, ys, step="mid", alpha=0.35, color="#0072B2")
    ax.plot(xs, ys, lw=1, color="#0072B2")
    ax.set_xlabel("sim time (µs)")
    ax.set_ylabel("ConsumeCredit events per 10 µs bin")
    ax.set_title(f"the oscillation: W=2, 800 G, 200 µs —\n"
                 f"burst-drain-refill; idle gap between bursts ≈ "
                 f"{period_us:.0f} µs ≈ 2·lat (credit return RTT)", fontsize=10)
    ax.grid(True, alpha=0.3)

    fig.suptitle("Credit flow control on the Su fabric: window × chunk vs "
                 "bandwidth-delay product\n"
                 "8-GPU ring allreduce, 256 MiB, 8 MiB credit = one bulk "
                 "chunk; utilization = T$_{∞-credits}$ / T$_{W-credits}$")
    for ext in ("pdf", "png"):
        fig.savefig(HERE / f"credit_bdp.{ext}", dpi=150)
    print(f"wrote credit_bdp.pdf/.png "
          f"(sawtooth: {n_events} consume events, "
          f"median burst period {period_us:.0f} us)")


if __name__ == "__main__":
    main()
