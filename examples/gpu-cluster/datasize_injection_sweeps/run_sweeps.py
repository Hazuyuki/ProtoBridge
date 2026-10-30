#!/usr/bin/env python3
"""Re-run the five single-knob sweeps and regenerate the committed CSVs.

Every sweep drives the debug build of `scratch/gpu-cluster-sim.cc` (build
first: `./ns3 configure -d debug --enable-tests --enable-examples && ./ns3
build`). The scenario is the ns3-rdma-matched fabric from
doc/PFC_NS3RDMA_ALIGNMENT.md / doc/SCALEOUT_ALIGNMENT.md — single shared-leaf
`tor`, 40 Gbps RoCE links, 1 us latency, 1000-B bulk chunks, soTxWindow=64 —
so the incast panels are directly comparable with the alignment tables.

Sweeps (each writes one CSV next to this script):
  1. datasize.csv       DCQCN incast completion time / ECN / CNP vs data
                        size, 1 MB .. 200 MB, 3 nodes.
  2. injection_rate.csv Random-traffic offered vs delivered goodput vs
                        per-source injection rate (0.1 .. 1.0 of line rate),
                        4-KiB messages, 2 ms warmup + 4 ms window.
  3. rate_msgsize.csv   Injection rate x message size (1/16/128 KiB at
                        0.5/0.7/0.9): delivered goodput matrix.
  4. nodes.csv          DCQCN incast node-count scaling, 2/4/8 nodes.
  5. bulkchunk.csv      DCQCN incast 50 MB vs bulk chunk size (1000 B ..
                        8192 B).

Total 32 ns-3 runs, each seconds to ~1 min. RngRun is left at the default
(1) so re-runs are reproducible; the committed CSVs came from commit
01e97a8.

Run:  python3 examples/gpu-cluster/datasize_injection_sweeps/run_sweeps.py
"""

from __future__ import annotations

import csv
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
SIM = REPO / "build/scratch/ns3.47-gpu-cluster-sim-debug"
HERE = Path(__file__).resolve().parent

# The ns3-rdma-matched fabric base (see doc/PFC_NS3RDMA_ALIGNMENT.md).
BASE = [
    "--topology=switched",
    "--collective=incast",
    "--gpusPerNode=1",
    "--interNodeTopology=tor",
    "--interNodeBandwidth=40",
    "--interNodeLatency=1000",
    "--bulkChunkSize=1000",
    "--soTxWindow=64",
    "--soRoceWireEfficiency=1.0",
]

RESULT_RE = re.compile(r"^([A-Za-z]+)=(\S+)$", re.MULTILINE)


def run_sim(extra: list[str]) -> dict[str, str]:
    cmd = [str(SIM), *BASE, *extra]
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=1200)
    if proc.returncode != 0:
        sys.exit(f"ns-3 failed ({proc.returncode}): {' '.join(cmd)}\n{proc.stdout[-2000:]}")
    return dict(RESULT_RE.findall(proc.stdout))


def write_csv(name: str, header: list[str], rows: list[list]) -> None:
    path = HERE / name
    with path.open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(header)
        w.writerows(rows)
    print(f"wrote {path} ({len(rows)} rows)")


def sweep_datasize() -> None:
    rows = []
    for ds in (1_000_000, 5_000_000, 10_000_000, 25_000_000, 50_000_000,
               100_000_000, 200_000_000):
        r = run_sim(["--numNodes=3", f"--dataSize={ds}", "--dcqcn=1"])
        rows.append([ds, r["simTimeUs"], r["ecnMarkedPackets"], r["cnpPacketsReceived"]])
        print(f"  dataSize={ds}: {r['simTimeUs']} us, ECN {r['ecnMarkedPackets']}")
    write_csv("datasize.csv",
              ["dataSize_bytes", "simTime_us", "ecn_marked", "cnp_rx"], rows)


def sweep_injection_rate() -> None:
    rows = []
    for rate in (0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.85, 0.9, 0.95, 1.0):
        r = run_sim(["--numNodes=3", "--dataSize=50000000", "--randTraffic=1",
                     f"--randInjectionRate={rate}", "--randMsgBytes=4096",
                     "--randWarmupNs=2000000", "--randMeasureNs=4000000"])
        rows.append([rate, r["randOfferedGbps"], r["randDeliveredGbps"],
                     r["ecnMarkedPackets"]])
        print(f"  rate={rate}: offered {r['randOfferedGbps']}, "
              f"delivered {r['randDeliveredGbps']} Gbps")
    write_csv("injection_rate.csv",
              ["injection_rate", "offered_gbps", "delivered_gbps", "ecn_marked"], rows)


def sweep_rate_msgsize() -> None:
    rows = []
    for rate in (0.5, 0.7, 0.9):
        for msg in (1024, 16384, 131072):
            r = run_sim(["--numNodes=3", "--dataSize=50000000", "--randTraffic=1",
                         f"--randInjectionRate={rate}", f"--randMsgBytes={msg}",
                         "--randWarmupNs=2000000", "--randMeasureNs=4000000"])
            rows.append([rate, msg, r["randOfferedGbps"], r["randDeliveredGbps"],
                         r["ecnMarkedPackets"]])
            print(f"  rate={rate} msg={msg}: delivered {r['randDeliveredGbps']} Gbps")
    write_csv("rate_msgsize.csv",
              ["injection_rate", "msg_bytes", "offered_gbps", "delivered_gbps",
               "ecn_marked"], rows)


def sweep_nodes() -> None:
    rows = []
    for nn in (2, 4, 8):
        r = run_sim([f"--numNodes={nn}", "--dataSize=50000000", "--dcqcn=1"])
        per_sender = (nn - 1) * 50_000_000 * 8 / float(r["simTimeNs"])
        rows.append([nn, r["simTimeUs"], r["ecnMarkedPackets"],
                     f"{per_sender:.2f}", f"{per_sender * (nn - 1):.2f}"])
        print(f"  numNodes={nn}: {r['simTimeUs']} us, ECN {r['ecnMarkedPackets']}")
    write_csv("nodes.csv",
              ["num_nodes", "simTime_us", "ecn_marked", "per_sender_gbps",
               "aggregate_gbps"], rows)


def sweep_bulkchunk() -> None:
    rows = []
    for ch in (1000, 2690, 4096, 8192):
        r = run_sim(["--numNodes=3", "--dataSize=50000000", "--dcqcn=1",
                     f"--bulkChunkSize={ch}"])
        rows.append([ch, r["simTimeUs"], r["ecnMarkedPackets"]])
        print(f"  bulkChunk={ch}: {r['simTimeUs']} us, ECN {r['ecnMarkedPackets']}")
    write_csv("bulkchunk.csv",
              ["bulk_chunk_bytes", "simTime_us", "ecn_marked"], rows)


def main() -> None:
    if not SIM.exists():
        sys.exit(f"{SIM} not found — build first "
                 "(./ns3 configure -d debug --enable-tests --enable-examples && ./ns3 build)")
    sweep_datasize()
    sweep_injection_rate()
    sweep_rate_msgsize()
    sweep_nodes()
    sweep_bulkchunk()
    print("all sweeps done — regenerate the figure with plot_sweeps.py")


if __name__ == "__main__":
    main()
