#!/usr/bin/env python3
"""Sweep: collective completion time vs data size for the 8 --interNodeTopology
values, 16 nodes x 8 GPUs (128 ranks), RoCE 400G So fabric with DCQCN enabled.

Deterministic-traffic counterpart of ../random_traffic_dcqcn/: instead of a
uniform-random synthetic load, runs the calibrated hierarchical collectives
(allreduce + alltoall, algorithm=hierarchical) and records completion time and
the ECN/CNP activity counters. Writes results.csv; plot with plot_collectives.py.
"""
import concurrent.futures
import csv
import os
import signal
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
BIN = REPO / "build" / "scratch" / "ns3.47-gpu-cluster-sim-debug"

TOPOLOGIES = ["fullmesh", "ring", "host", "tor", "rail", "leafspine", "torus2d", "dragonfly"]
COLLECTIVES = ["allreduce", "alltoall"]
SIZES = [1 << 20, 4 << 20, 16 << 20, 64 << 20, 256 << 20]  # 1..256 MiB per GPU

NUM_NODES = 16
GPUS_PER_NODE = 8
NUM_RANKS = NUM_NODES * GPUS_PER_NODE
BW_GBPS = 400
LAT_NS = 1000
MAX_PARALLEL = 16
TIMEOUT_S = 1800

FIELDS = [
    "collective", "topology", "dataSize", "status", "simTimeNs", "timeUs",
    "busbwGbps", "ecnMarkedPackets", "cnpPacketsSent", "cnpPacketsReceived",
]


def busbw_gbps(collective, size_bytes, sim_ns):
    """NCCL bus-bandwidth convention, N = total ranks."""
    if sim_ns <= 0:
        return 0.0
    n = NUM_RANKS
    factor = 2.0 * (n - 1) / n if collective == "allreduce" else (n - 1) / n
    return factor * size_bytes * 8.0 / sim_ns  # bytes*8/ns == Gbps


def run_one(job):
    collective, topology, size = job
    cmd = [
        str(BIN),
        f"--numNodes={NUM_NODES}",
        f"--gpusPerNode={GPUS_PER_NODE}",
        f"--collective={collective}",
        "--algorithm=hierarchical",
        f"--dataSize={size}",
        "--interNodeFabricType=RoCE",
        f"--interNodeTopology={topology}",
        f"--interNodeBandwidth={BW_GBPS}",
        f"--interNodeLatency={LAT_NS}",
        "--dcqcn=1",
        "--RngRun=1",
    ]
    row = {f: "" for f in FIELDS}
    row.update(collective=collective, topology=topology, dataSize=size, status="error")
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                            text=True, start_new_session=True)
    try:
        out, _ = proc.communicate(timeout=TIMEOUT_S)
    except subprocess.TimeoutExpired:
        os.killpg(proc.pid, signal.SIGKILL)
        proc.wait()
        row["status"] = "timeout"
        return row
    kv = {}
    for line in out.splitlines():
        if "=" in line and " " not in line.split("=", 1)[0]:
            k, v = line.split("=", 1)
            kv[k] = v
    row["status"] = kv.get("status", "error")
    if "simTimeNs" in kv:
        sim_ns = int(kv["simTimeNs"])
        row["simTimeNs"] = sim_ns
        row["timeUs"] = f"{sim_ns / 1e3:.1f}"
        row["busbwGbps"] = f"{busbw_gbps(collective, size, sim_ns):.2f}"
    for k in ("ecnMarkedPackets", "cnpPacketsSent", "cnpPacketsReceived"):
        row[k] = kv.get(k, "")
    return row


def main():
    jobs = [(c, t, s) for c in COLLECTIVES for t in TOPOLOGIES for s in SIZES]
    rows = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=MAX_PARALLEL) as ex:
        for row in ex.map(run_one, jobs):
            print(f"{row['collective']:9s} {row['topology']:10s} {int(row['dataSize'])>>20:4d}MiB "
                  f"{row['status']:8s} t={row['timeUs'] or '-'}us busbw={row['busbwGbps'] or '-'} "
                  f"ecn={row['ecnMarkedPackets'] or '-'}", flush=True)
            rows.append(row)
    order = {t: i for i, t in enumerate(TOPOLOGIES)}
    corder = {c: i for i, c in enumerate(COLLECTIVES)}
    rows.sort(key=lambda r: (corder[r["collective"]], order[r["topology"]], int(r["dataSize"])))
    out = HERE / "results.csv"
    with out.open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=FIELDS)
        w.writeheader()
        w.writerows(rows)
    bad = [r for r in rows if r["status"] != "complete"]
    print(f"\nwrote {out} ({len(rows)} rows, {len(bad)} not complete)")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
