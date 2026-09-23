#!/usr/bin/env python3
"""Sweep: DCQCN parameter sensitivity on the one scenario where the chain
actually fires — host topology + ncclring allreduce, 16 nodes x 8 GPUs, RoCE
400G, 256 MiB (host's inter-switch peer links see real incast there; every
other So shape stays below the ECN threshold — see results_ncclring.csv).

Axes (all opt-in CLI flags, defaults = the center cell):
  ecnThreshold : 1 / 4 / 8* / 16 / 32   (switch ECN mark threshold, pkts)
  dcqcnGInit   : 0.25 / 0.5* / 0.75     (initial multiplicative backoff g)
  dcqcnTFastNs : 10k / 50k* / 100k      (fast-recovery timer)

Plus a dcqcn=0 baseline (no CC) for the completion-time delta.
Writes results_dcqcn_params.csv; plot with plot_dcqcn_params.py.
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

NUM_NODES = 16
GPUS_PER_NODE = 8
NUM_RANKS = NUM_NODES * GPUS_PER_NODE
SIZE = 256 << 20  # per GPU
BW_GBPS = 400
LAT_NS = 1000

ECN_THRESHOLDS = [1, 4, 8, 16, 32]
GINITS = [0.25, 0.5, 0.75]
TFASTS = [10000, 50000, 100000]

MAX_PARALLEL = 16
TIMEOUT_S = 1800

FIELDS = [
    "runType", "ecnThreshold", "dcqcnGInit", "dcqcnTFastNs", "status",
    "simTimeNs", "timeUs", "busbwGbps", "ecnMarkedPackets",
    "cnpPacketsSent", "cnpPacketsReceived",
]


def busbw_gbps(size_bytes, sim_ns):
    if sim_ns <= 0:
        return 0.0
    n = NUM_RANKS
    return 2.0 * (n - 1) / n * size_bytes * 8.0 / sim_ns  # bytes*8/ns == Gbps


def base_cmd():
    return [
        str(BIN),
        f"--numNodes={NUM_NODES}",
        f"--gpusPerNode={GPUS_PER_NODE}",
        "--collective=allreduce",
        "--algorithm=ncclring",
        f"--dataSize={SIZE}",
        "--interNodeFabricType=RoCE",
        "--interNodeTopology=host",
        f"--interNodeBandwidth={BW_GBPS}",
        f"--interNodeLatency={LAT_NS}",
        "--RngRun=1",
    ]


def run_one(job):
    kind, ecn, ginit, tfast = job
    cmd = base_cmd()
    if kind == "dcqcn0":
        pass  # no CC baseline: no --dcqcn
    else:
        cmd += [
            "--dcqcn=1",
            f"--ecnThreshold={ecn}",
            f"--dcqcnGInit={ginit}",
            f"--dcqcnTFastNs={tfast}",
        ]
    row = {f: "" for f in FIELDS}
    row.update(runType=kind, ecnThreshold=ecn, dcqcnGInit=ginit,
               dcqcnTFastNs=tfast, status="error")
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
        row["busbwGbps"] = f"{busbw_gbps(SIZE, sim_ns):.2f}"
    for k in ("ecnMarkedPackets", "cnpPacketsSent", "cnpPacketsReceived"):
        row[k] = kv.get(k, "")
    return row


def main():
    jobs = [("dcqcn0", 8, 0.5, 50000)]  # placeholder coords for the baseline
    jobs += [("grid", e, g, t)
             for e in ECN_THRESHOLDS
             for g in GINITS
             for t in TFASTS]
    rows = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=MAX_PARALLEL) as ex:
        for row in ex.map(run_one, jobs):
            if row["runType"] == "dcqcn0":
                desc = "dcqcn=0 (no CC)"
            else:
                desc = (f"ecn={row['ecnThreshold']:>2} g={row['dcqcnGInit']:<4} "
                        f"tF={int(row['dcqcnTFastNs']) // 1000:3d}us")
            print(f"{desc}  {row['status']:8s} t={row['timeUs'] or '-'}us "
                  f"busbw={row['busbwGbps'] or '-'} ecn={row['ecnMarkedPackets'] or '-'}",
                  flush=True)
            rows.append(row)
    order = {"dcqcn0": 0, "grid": 1}
    rows.sort(key=lambda r: (order[r["runType"]],
                             int(r["ecnThreshold"]), float(r["dcqcnGInit"]),
                             int(r["dcqcnTFastNs"])))
    out = HERE / "results_dcqcn_params.csv"
    with out.open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=FIELDS)
        w.writeheader()
        w.writerows(rows)
    bad = [r for r in rows if r["status"] != "complete"]
    print(f"\nwrote {out} ({len(rows)} rows, {len(bad)} not complete)")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
