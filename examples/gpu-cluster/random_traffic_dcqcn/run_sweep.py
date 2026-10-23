#!/usr/bin/env python3
"""Sweep: delivered throughput vs injection rate for the 8 So topologies
under uniform-random cross-node traffic with DCQCN enabled.

Setup: 8 nodes x 4 GPUs (32 ranks), RoCE 400 Gbps links, 1 us link
latency, 64 KiB messages, 200 us warmup + 800 us measurement window,
--dcqcn on, RngRun=1. Every rank injects fixed-interval messages to
uniform-random ranks on OTHER nodes at the given fraction of the
inter-node line rate.

Output: results.csv (one row per topology x rate with offered/delivered
Gbps + ECN/CNP counters). Plot with plot_throughput.py.
"""

import csv
import os
import signal
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
BIN = REPO / "build" / "scratch" / "ns3.47-gpu-cluster-sim-debug"
OUT_CSV = HERE / "results.csv"

TOPOLOGIES = [
    "fullmesh", "ring", "host", "tor",
    "rail", "leafspine", "torus2d", "dragonfly",
]
RATES = [0.05, 0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 1.0]

NUM_NODES = 8
GPUS_PER_NODE = 4
BW_GBPS = 400
LAT_NS = 1000
MSG_BYTES = 65536
WARMUP_NS = 200_000
MEASURE_NS = 800_000
MAX_PARALLEL = 16
TIMEOUT_S = 1800

FIELDS = [
    "topology", "rate", "status", "offeredGbps", "deliveredGbps",
    "acceptedFraction", "msgsSent", "ecnMarkedPackets",
    "cnpPacketsSent", "cnpPacketsReceived",
]


def run_one(topo, rate):
    args = [
        str(BIN),
        f"--numNodes={NUM_NODES}",
        f"--gpusPerNode={GPUS_PER_NODE}",
        "--interNodeFabricType=RoCE",
        f"--interNodeTopology={topo}",
        f"--interNodeBandwidth={BW_GBPS}",
        f"--interNodeLatency={LAT_NS}",
        "--randTraffic=1",
        f"--randInjectionRate={rate}",
        f"--randMsgBytes={MSG_BYTES}",
        f"--randWarmupNs={WARMUP_NS}",
        f"--randMeasureNs={MEASURE_NS}",
        "--dcqcn=1",
        "--RngRun=1",
    ]
    proc = subprocess.Popen(
        args,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
        start_new_session=True,
    )
    try:
        out, _ = proc.communicate(timeout=TIMEOUT_S)
    except subprocess.TimeoutExpired:
        os.killpg(proc.pid, signal.SIGKILL)
        return {"topology": topo, "rate": rate, "status": "timeout"}
    r = {}
    for line in out.splitlines():
        if "=" in line:
            k, _, v = line.partition("=")
            r[k.strip()] = v.strip()
    offered = float(r.get("randOfferedGbps", 0))
    delivered = float(r.get("randDeliveredGbps", 0))
    line_agg = NUM_NODES * GPUS_PER_NODE * BW_GBPS
    row = {
        "topology": topo,
        "rate": rate,
        "status": r.get("status", "parse-error"),
        "offeredGbps": offered,
        "deliveredGbps": delivered,
        "acceptedFraction": delivered / line_agg if line_agg else 0.0,
        "msgsSent": r.get("randMsgsSent", ""),
        "ecnMarkedPackets": r.get("ecnMarkedPackets", ""),
        "cnpPacketsSent": r.get("cnpPacketsSent", ""),
        "cnpPacketsReceived": r.get("cnpPacketsReceived", ""),
    }
    print(f"  {topo:>10} rate={rate:<5} status={row['status']:<9} "
          f"delivered={delivered:8.1f} Gbps ecn={row['ecnMarkedPackets']}",
          flush=True)
    return row


def main():
    if not BIN.exists():
        raise SystemExit(f"sim binary not found: {BIN} (run ./ns3 build)")
    jobs = [(t, r) for t in TOPOLOGIES for r in RATES]
    print(f"{len(jobs)} runs ({len(TOPOLOGIES)} topologies x {len(RATES)} rates), "
          f"{MAX_PARALLEL} parallel", flush=True)
    with ThreadPoolExecutor(max_workers=MAX_PARALLEL) as pool:
        rows = list(pool.map(lambda j: run_one(*j), jobs))
    rows.sort(key=lambda r: (TOPOLOGIES.index(r["topology"]), r["rate"]))
    with open(OUT_CSV, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=FIELDS)
        w.writeheader()
        for row in rows:
            w.writerow({k: row.get(k, "") for k in FIELDS})
    bad = [r for r in rows if r["status"] != "complete"]
    print(f"wrote {OUT_CSV} ({len(rows)} rows, {len(bad)} not complete)")
    if bad:
        for r in bad:
            print(f"  INCOMPLETE: {r['topology']} rate={r['rate']} "
                  f"status={r['status']}")
        sys.exit(1)


if __name__ == "__main__":
    main()
