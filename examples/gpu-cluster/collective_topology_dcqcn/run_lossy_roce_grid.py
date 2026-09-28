#!/usr/bin/env python3
"""Grid sweep: DCQCN parameter sensitivity in the LOSSY-RoCE (no-PFC) regime,
in the same 5x3x3 grid form as the lossless run_dcqcn_param_sweep.py.

Scenario: 2n x 8g hierarchical allreduce 256 MiB, RoCE 400G host topology,
--voqDropThreshold=8 (explicit lossy opt-in; 816 tail-drops + 892 retx at
dcqcn=0, +32.4% completion vs lossless) -- the regime the OAT sweep
(run_lossy_roce_sweep.py) showed is where the knobs bite.

Axes (mirroring the lossless grid, adapted to the lossy semantics):
  ecnThreshold : 1 / 2 / 4 / 6 / 8   (mark-before-drop requires ecn <= drop=8;
                                       >8 would be a dead column: the drop
                                       check shadows the mark at depth 8)
  dcqcnGInit   : 0.125 / 0.25 / 0.5  (around the OAT optimum 0.125-0.25)
  dcqcnTFastNs : 10k / 50k / 100k

Plus a dcqcn=0 lossy baseline and a lossless reference.
Writes results_lossy_grid.csv; plot with plot_lossy_roce_grid.py.
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

NUM_NODES = 2
GPUS_PER_NODE = 8
SIZE = 256 << 20  # per GPU
BW_GBPS = 400
LAT_NS = 1000
DROP = 8

ECN_THRESHOLDS = [1, 2, 4, 6, 8]
GINITS = [0.125, 0.25, 0.5]
TFASTS = [10000, 50000, 100000]

MAX_PARALLEL = 16
TIMEOUT_S = 1800

FIELDS = [
    "runType", "ecnThreshold", "dcqcnGInit", "dcqcnTFastNs", "status",
    "simTimeNs", "timeUs", "switchVoqDrops", "retransmittedPackets",
    "ecnMarkedPackets", "cnpPacketsSent", "cnpPacketsReceived",
    "permanentLosses",
]


def base_cmd(lossy):
    cmd = [
        str(BIN),
        f"--numNodes={NUM_NODES}",
        f"--gpusPerNode={GPUS_PER_NODE}",
        "--interNodeTopology=host",
        "--interNodeFabricType=RoCE",
        f"--interNodeBandwidth={BW_GBPS}",
        f"--interNodeLatency={LAT_NS}",
        "--collective=allreduce",
        "--algorithm=hierarchical",
        f"--dataSize={SIZE}",
        "--RngRun=1",
    ]
    if lossy:
        cmd += [f"--voqDropThreshold={DROP}"]
    return cmd


def run_one(job):
    kind, ecn, ginit, tfast = job
    cmd = base_cmd(lossy=kind != "lossless")
    if kind == "grid":
        cmd += [
            "--dcqcn=1",
            f"--ecnThreshold={ecn}",
            f"--dcqcnGInit={ginit}",
            f"--dcqcnTFastNs={tfast}",
        ]
    elif kind == "dcqcn0":
        cmd += [f"--ecnThreshold={ecn}"]  # lossy, no CC; ecn irrelevant
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
        row["simTimeNs"] = kv["simTimeNs"]
        row["timeUs"] = f"{int(kv['simTimeNs']) / 1e3:.1f}"
    for k in ("switchVoqDrops", "retransmittedPackets", "ecnMarkedPackets",
              "cnpPacketsSent", "cnpPacketsReceived", "permanentLosses"):
        row[k] = kv.get(k, "")
    return row


def main():
    jobs = [("dcqcn0", 8, 0.5, 50000), ("lossless", 8, 0.5, 50000)]
    jobs += [("grid", e, g, t)
             for e in ECN_THRESHOLDS
             for g in GINITS
             for t in TFASTS]
    print(f"{len(jobs)} runs, {MAX_PARALLEL} parallel...")
    rows = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=MAX_PARALLEL) as ex:
        for i, row in enumerate(ex.map(run_one, jobs)):
            rows.append(row)
            print(f"[{i + 1}/{len(jobs)}] {row['runType']:8s} ecn={row['ecnThreshold']} "
                  f"g={row['dcqcnGInit']} tFast={row['dcqcnTFastNs']} "
                  f"{row['status']:8s} {row['timeUs']}us "
                  f"drops={row['switchVoqDrops']} retx={row['retransmittedPackets']} "
                  f"ecn={row['ecnMarkedPackets']}")
    out = HERE / "results_lossy_grid.csv"
    with open(out, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=FIELDS)
        w.writeheader()
        w.writerows(rows)
    print(f"wrote {out}")


if __name__ == "__main__":
    sys.exit(main())
