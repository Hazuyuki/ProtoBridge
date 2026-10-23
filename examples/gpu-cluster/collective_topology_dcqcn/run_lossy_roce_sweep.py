#!/usr/bin/env python3
"""Sweep: DCQCN parameter sensitivity in the LOSSY-RoCE (no-PFC) regime.

The lossless sweeps (run_dcqcn_param_sweep.py / run_dcqcn_rand_sweep.py)
established that the alpha knobs are outcome-neutral when the fabric cannot
drop (one-shot flowIds + soft-deferral absorption). This sweep opts the RoCE
fabric into the lossy regime (--voqDropThreshold=8 on the 2-node hierarchical
allreduce, where the accidental-threshold runs showed ~830 VOQ tail-drops) and
turns on mark-before-drop (ecnThreshold=4 < drop threshold 8), so the
ECN -> CNP -> alpha chain has a real job: keep the VOQ below the drop
threshold and cut the NACK->LLR retransmission stalls.

Axes (one-at-a-time around the default centre, plus loss-severity arms):
  dcqcnGInit     : 0.0625 / 0.125 / 0.25 / 0.5* / 1.0
  dcqcnGDecay    : 0.125 / 0.25 / 0.5* / 0.75 / 1.0
  dcqcnTFastNs   : 5k / 10k / 25k / 50k* / 100k
  dcqcnFastCycles: 1 / 2 / 5* / 10 / 20
  dcqcnTAiNs     : 25k / 50k / 100k* / 200k / 400k
  dcqcnRai       : 0.005 / 0.01* / 0.02 / 0.05 / 0.1
  dcqcnAlphaFloor: 0* / 0.25 / 0.5 / 0.75 / 1.0

Arms:
  lossless   : no --voqDropThreshold (RoCE parity reference)
  dcqcn0     : lossy, no CC (the stall baseline)
  default    : lossy + dcqcn=1, all params at TypeId defaults
  oat-*      : lossy + dcqcn=1, one parameter overridden
  dropthr-N  : loss severity sweep (drop 8/16/32) x {dcqcn0, default, gInit=0.125}

Writes results_lossy_roce.csv; plot with plot_lossy_roce.py.
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
ECN = 4          # mark-before-drop: ecnThreshold < drop threshold
DROP = 8

# (flag, values, default-index) -- 0 value means "pass nothing" for the floor.
OAT_AXES = [
    ("dcqcnGInit", [0.0625, 0.125, 0.25, 0.5, 1.0], 3),
    ("dcqcnGDecay", [0.125, 0.25, 0.5, 0.75, 1.0], 2),
    ("dcqcnTFastNs", [5000, 10000, 25000, 50000, 100000], 3),
    ("dcqcnFastCycles", [1, 2, 5, 10, 20], 2),
    ("dcqcnTAiNs", [25000, 50000, 100000, 200000, 400000], 2),
    ("dcqcnRai", [0.005, 0.01, 0.02, 0.05, 0.1], 1),
    ("dcqcnAlphaFloor", [0.0, 0.25, 0.5, 0.75, 1.0], 0),
]

DROP_THRS = [8, 16, 32]

MAX_PARALLEL = 16
TIMEOUT_S = 1800

FIELDS = [
    "arm", "param", "value", "voqDropThreshold", "status",
    "simTimeNs", "timeUs", "switchVoqDrops", "retransmittedPackets",
    "ecnMarkedPackets", "cnpPacketsSent", "cnpPacketsReceived",
    "permanentLosses",
]


def base_cmd(lossy=True, drop=DROP):
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
        cmd += [f"--voqDropThreshold={drop}", f"--ecnThreshold={ECN}"]
    return cmd


def run_one(job):
    arm, param, value, drop = job
    cmd = base_cmd(lossy=arm != "lossless", drop=drop)
    # No-CC arms: the plain dcqcn0 baseline AND the dropthr severity arm that
    # uses "dcqcn0" as its param label (the arm field says "dropthr").
    no_cc = arm in ("lossless", "dcqcn0") or (arm == "dropthr" and param == "dcqcn0")
    if not no_cc:
        cmd += ["--dcqcn=1"]
        if param and value:
            if param in ("dcqcnTFastNs", "dcqcnTAiNs"):
                cmd += [f"--{param}={int(value)}"]
            elif param == "dcqcnFastCycles":
                cmd += [f"--{param}={int(value)}"]
            else:
                cmd += [f"--{param}={value}"]
    row = {f: "" for f in FIELDS}
    row.update(arm=arm, param=param or "", value=value if value != "" else "",
               voqDropThreshold=drop if arm != "lossless" else 0,
               status="error")
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
    jobs = []
    # References: lossless parity + lossy without CC.
    jobs.append(("lossless", "", "", 0))
    jobs.append(("dcqcn0", "", "", DROP))
    # Default-centre run (all params at TypeId defaults).
    jobs.append(("default", "", "", DROP))
    # One-at-a-time around the default centre.
    for flag, values, _ in OAT_AXES:
        for v in values:
            jobs.append(("oat", flag, v, DROP))
    # Loss severity: drop threshold x {no CC, default CC, aggressive gInit}.
    for d in DROP_THRS:
        jobs.append(("dropthr", "dcqcn0", "", d))
        jobs.append(("dropthr", "default", "", d))
        jobs.append(("dropthr", "dcqcnGInit", 0.125, d))
    # Keep one run per unique job (the default centre repeats across arms).
    seen = set()
    uniq = []
    for j in jobs:
        key = (j[0], j[1], str(j[2]), j[3])
        if key not in seen:
            seen.add(key)
            uniq.append(j)
    jobs = uniq

    print(f"{len(jobs)} runs, {MAX_PARALLEL} parallel...")
    rows = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=MAX_PARALLEL) as ex:
        for i, row in enumerate(ex.map(run_one, jobs)):
            rows.append(row)
            print(f"[{i + 1}/{len(jobs)}] {row['arm']:8s} {row['param']:18s} "
                  f"{str(row['value']):8s} drop={row['voqDropThreshold']!s:3s} "
                  f"{row['status']:8s} {row['timeUs']}us "
                  f"voqDrops={row['switchVoqDrops']} retx={row['retransmittedPackets']}")

    out = HERE / "results_lossy_roce.csv"
    with open(out, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=FIELDS)
        w.writeheader()
        w.writerows(rows)
    print(f"wrote {out}")


if __name__ == "__main__":
    sys.exit(main())
