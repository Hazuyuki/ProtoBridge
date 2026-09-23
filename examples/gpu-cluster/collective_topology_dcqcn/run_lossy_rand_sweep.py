#!/usr/bin/env python3
"""Sweep: lossy-RoCE saturation contrast -- where drops do NOT cost (and
DCQCN cannot help) vs the completion-time regime where they do.

Counterpart to run_lossy_roce_sweep.py (hierarchical allreduce, completion
time): this one drives the sustained randTraffic regime (2n x 4g leafspine
4:1 oversubscription, injection 0.8) under the same lossy-RoCE opt-in.
Under saturation the bottleneck uplink is full either way, so a VOQ
tail-drop + LLR retransmission only swaps useful bytes for retransmitted
bytes -- delivered goodput stays ~flat (the lossless reference delivered
642 Gbps; the lossy arms land within ~1%). The point of the arm is to pin
that contrast: completion-time loads feel the drops, goodput loads do not.

Arms (all lossy RoCE, ecnThreshold=4 < drop threshold):
  dropthr x dcqcn0 / default (dcqcn=1) / aggressive gInit=0.125
  plus a lossless dcqcn0 reference (no --voqDropThreshold).

Writes results_lossy_rand.csv.
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
GPUS_PER_NODE = 4
BW_GBPS = 400
LAT_NS = 1000
ECN = 4

DROP_THRS = [8, 16, 64]

MAX_PARALLEL = 16
TIMEOUT_S = 1800

FIELDS = [
    "arm", "voqDropThreshold", "status",
    "randDeliveredGbps", "randOfferedGbps", "switchVoqDrops",
    "retransmittedPackets", "ecnMarkedPackets", "cnpPacketsSent",
    "cnpPacketsReceived", "permanentLosses",
]


def base_cmd(lossy=True, drop=0):
    cmd = [
        str(BIN),
        f"--numNodes={NUM_NODES}",
        f"--gpusPerNode={GPUS_PER_NODE}",
        "--interNodeTopology=leafspine",
        "--interNodeSpines=1",  # 4:1 oversubscription at the leaf uplink
        "--interNodeFabricType=RoCE",
        f"--interNodeBandwidth={BW_GBPS}",
        f"--interNodeLatency={LAT_NS}",
        "--randTraffic=1",
        "--randInjectionRate=0.8",
        "--randWarmupNs=200000",
        "--randMeasureNs=800000",
        "--RngRun=1",
    ]
    if lossy:
        cmd += [f"--voqDropThreshold={drop}", f"--ecnThreshold={ECN}"]
    return cmd


def run_one(job):
    arm, drop = job
    cmd = base_cmd(lossy=arm != "lossless", drop=drop)
    if arm not in ("lossless", "dcqcn0"):
        cmd += ["--dcqcn=1"]
        if arm == "ginit":
            cmd += ["--dcqcnGInit=0.125"]
    row = {f: "" for f in FIELDS}
    row.update(arm=arm, voqDropThreshold=drop, status="error")
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
    for k in ("randDeliveredGbps", "randOfferedGbps", "switchVoqDrops",
              "retransmittedPackets", "ecnMarkedPackets", "cnpPacketsSent",
              "cnpPacketsReceived", "permanentLosses"):
        row[k] = kv.get(k, "")
    return row


def main():
    jobs = [("lossless", 0)]
    for d in DROP_THRS:
        jobs += [("dcqcn0", d), ("default", d), ("ginit", d)]
    print(f"{len(jobs)} runs, {MAX_PARALLEL} parallel...")
    rows = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=MAX_PARALLEL) as ex:
        for i, row in enumerate(ex.map(run_one, jobs)):
            rows.append(row)
            print(f"[{i + 1}/{len(jobs)}] {row['arm']:8s} drop={row['voqDropThreshold']!s:3s} "
                  f"{row['status']:8s} delivered={row['randDeliveredGbps']} "
                  f"voqDrops={row['switchVoqDrops']} retx={row['retransmittedPackets']}")
    out = HERE / "results_lossy_rand.csv"
    with open(out, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=FIELDS)
        w.writeheader()
        w.writerows(rows)
    print(f"wrote {out}")


if __name__ == "__main__":
    sys.exit(main())
