#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Demystifying NCCL (arXiv:2507.04786) Fig. 6 intra-node transfer test.

Third external measured anchor, and the first for the *Su* (NVLink) protocol
axis: hardware-measured LL / LL128 / Simple x Ring / Tree AllReduce runtimes
on a 4x GH200 quad (CSCS Alps, NCCL 2.19.1) vs the ns-3 NcclProtocolModel
with ONLY physical parameters changed to the GH200 quad spec:

  ring arm : --topology=ring --algorithm=ring --numGpus=4
             --numLanes=18 --bandwidth=200   (450 GB/s per-GPU egress --
             NCCL multi-channel rings aggregate all 3 pair-links; same
             modeling decision as the H200 ring preset)
  tree arm : --topology=fullmesh --algorithm=tree --numGpus=4
             --numLanes=6 --bandwidth=200    (one 150 GB/s pair-link per
             tree edge -- NCCL's intra-node tree/chain is pair-limited)

The ONLY fitted constants are the 6 startup intercepts (one per
algorithm x protocol), anchored at the smallest size (2^14): startup =
measured(2^14) - sim_wire(2^14, startup=0).  Everything else -- LL 0.5 /
LL128 0.9375 / Simple 1.0 wire efficiencies, chunk sizes, credit flow,
spray -- is the stock protocol model.  Sizes 2^16..2^32 are therefore a
pure transfer test of the protocol shape constants on third-party hardware.

Usage:
    python3 examples/gpu-cluster/demystify_nccl_sweep.py            # run + report
    python3 examples/gpu-cluster/demystify_nccl_sweep.py --plot     # + figure
"""

import csv
import glob
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
HERE = os.path.dirname(os.path.abspath(__file__))
REF = os.path.join(HERE, "demystify_nccl_fig6_reference.csv")
OUT_JSON = os.path.join(HERE, "demystify_nccl_sweep.json")

SIZES = [2 ** k for k in range(14, 33, 2)]
PROTOS = {"ll": 1, "ll128": 2, "simple": 3}
STARTUP_FLAG = {"ll": "--startupLL", "ll128": "--startupLL128", "simple": "--startupSIMPLE"}

ARMS = {
    "ring": ["--topology=ring", "--algorithm=ring", "--numLanes=18"],
    "tree": ["--topology=fullmesh", "--algorithm=tree", "--numLanes=6"],
}

COMMON = [
    "--collective=allreduce",
    "--numGpus=4",
    "--bandwidth=200",     # Gbps per NVLink4 lane (25 GB/s)
    "--latency=300",       # ns per hop, NVLink-scale
    "--vcCredits=104857600",
    "--sprayChunkSize=131072",
    "--RngRun=1",
]


def find_binary():
    for b in sorted(glob.glob(os.path.join(ROOT, "build", "scratch", "*gpu-cluster-sim*"))):
        if not b.endswith(".o") and os.access(b, os.X_OK):
            return b
    raise RuntimeError("gpu-cluster-sim binary not found; build first")


def run_sim(binary, arm, proto, size, startup_ns):
    args = [binary] + ARMS[arm] + COMMON + [
        "--dataSize={}".format(size),
        "--forceProtocol={}".format(PROTOS[proto]),
        "--startupLL=0", "--startupLL128=0", "--startupSIMPLE=0",
    ]
    # overwrite the forced protocol's startup (later flag wins in ns-3 CLI)
    args.append("{}={}".format(STARTUP_FLAG[proto], startup_ns))
    p = subprocess.run(args, cwd=ROOT, capture_output=True, text=True, timeout=1800)
    out = p.stdout + p.stderr
    m = re.search(r"^simTimeNs=(\d+)", out, re.MULTILINE)
    if not m or not re.search(r"^status=complete", out, re.MULTILINE):
        raise RuntimeError("run failed: {} {} {}\n{}".format(arm, proto, size, out[-800:]))
    return int(m.group(1))


def load_reference():
    ref = {}
    with open(REF) as f:
        for row in csv.DictReader(r for r in f if not r.startswith("#")):
            ref[(row["algorithm"], int(row["size_bytes"]), row["protocol"])] = \
                float(row["runtime_ms"])
    return ref


def main():
    binary = find_binary()
    ref = load_reference()

    # 1) startup intercepts anchored at 2^14
    startups = {}
    for arm in ARMS:
        for proto in PROTOS:
            t0 = run_sim(binary, arm, proto, SIZES[0], 0)
            meas_ns = ref[(arm, SIZES[0], proto)] * 1e6
            startups[(arm, proto)] = max(0, int(round(meas_ns - t0)))
    print("startup intercepts (ns):")
    for k, v in sorted(startups.items()):
        print("  {:4s} {:6s} {:8d}".format(k[0], k[1], v))

    # 2) full sweep
    results = []
    for arm in ARMS:
        for proto in PROTOS:
            su = startups[(arm, proto)]
            for size in SIZES:
                sim_ns = run_sim(binary, arm, proto, size, su)
                meas_ms = ref[(arm, size, proto)]
                sim_ms = sim_ns / 1e6
                results.append(dict(algorithm=arm, protocol=proto, size=size,
                                    measured_ms=meas_ms, sim_ms=sim_ms,
                                    ratio=sim_ms / meas_ms))
                print("{:4s} {:6s} 2^{:<2d} meas={:9.4f}  sim={:9.4f}  ratio={:5.3f}".format(
                    arm, proto, size.bit_length() - 1, meas_ms, sim_ms, sim_ms / meas_ms))

    # 3) per-curve MAPE over the transfer range (2^16..2^32; 2^14 is the anchor)
    mapes = {}
    for arm in ARMS:
        for proto in PROTOS:
            pts = [r for r in results
                   if r["algorithm"] == arm and r["protocol"] == proto and r["size"] > SIZES[0]]
            mapes["{}/{}".format(arm, proto)] = \
                sum(abs(r["ratio"] - 1) for r in pts) / len(pts) * 100
    print("\nMAPE over 2^16..2^32 (2^14 = startup anchor, excluded):")
    for k, v in sorted(mapes.items()):
        print("  {:12s} {:6.2f}%".format(k, v))

    with open(OUT_JSON, "w") as f:
        json.dump(dict(startups={"{}/{}".format(*k): v for k, v in startups.items()},
                       results=results, mape_pct=mapes), f, indent=1)
    print("\nwrote", OUT_JSON)

    if "--plot" in sys.argv:
        plot(results)


def plot(results):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    colors = {"simple": "tab:blue", "ll": "tab:orange", "ll128": "tab:green"}
    fig, axes = plt.subplots(1, 2, figsize=(11, 4.2), sharey=True)
    for ax, arm in zip(axes, ("ring", "tree")):
        for proto in ("simple", "ll", "ll128"):
            pts = sorted((r for r in results
                          if r["algorithm"] == arm and r["protocol"] == proto),
                         key=lambda r: r["size"])
            xs = [r["size"] for r in pts]
            ax.plot(xs, [r["measured_ms"] for r in pts], "o", color=colors[proto],
                    label="{} (GH200 quad, Fig. 6)".format(proto.upper()))
            ax.plot(xs, [r["sim_ms"] for r in pts], "-", color=colors[proto],
                    label="{} (ns-3)".format(proto.upper()))
        ax.set_xscale("log", base=2)
        ax.set_yscale("log")
        ax.set_xlabel("message size (bytes)")
        ax.set_title("AllReduce {} , intra-node 4x GH200".format(arm))
        ax.grid(True, which="both", alpha=0.3)
    axes[0].set_ylabel("runtime (ms)")
    axes[0].legend(fontsize=7)
    fig.suptitle("Demystifying NCCL (arXiv:2507.04786) Fig. 6 intra-node vs ns-3 "
                 "(startup anchored at 2^14; protocol constants stock)", fontsize=10)
    fig.tight_layout()
    for ext in ("png", "pdf"):
        out = os.path.join(HERE, "demystify_nccl_fig6_comparison." + ext)
        fig.savefig(out, dpi=150)
        print("wrote", out)


if __name__ == "__main__":
    main()
