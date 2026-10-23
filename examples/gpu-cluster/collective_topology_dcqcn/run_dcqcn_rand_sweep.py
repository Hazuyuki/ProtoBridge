#!/usr/bin/env python3
"""Sweep: DCQCN parameter sensitivity in the CONGESTED sustained-flow regime
-- randTraffic on a 4:1-oversubscribed leafspine (1 spine), 16 nodes x 8
GPUs, RoCE 400G. The random-traffic flowId is the SOURCE rank (long-lived
flows), so -- unlike the collective regime's one-shot per-step flowIds --
an ECN->CNP->Alpha cycle can throttle the flow's LATER messages. Metric:
delivered Gbps in the measurement window (+ ECN/CNP counters).

Same axes as run_dcqcn_param_sweep.py. Writes results_dcqcn_rand.csv.
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
BW_GBPS = 400
LAT_NS = 1000
INJECTION = 0.8
WARMUP_NS = 200000
MEASURE_NS = 800000

ECN_THRESHOLDS = [1, 4, 8, 16, 32]
GINITS = [0.25, 0.5, 0.75]
TFASTS = [10000, 50000, 100000]

MAX_PARALLEL = 16
TIMEOUT_S = 600

FIELDS = [
    "runType", "ecnThreshold", "dcqcnGInit", "dcqcnTFastNs", "status",
    "offeredGbps", "deliveredGbps", "ecnMarkedPackets",
    "cnpPacketsSent", "cnpPacketsReceived",
]


def run_one(job):
    kind, ecn, ginit, tfast = job
    cmd = [
        str(BIN),
        f"--numNodes={NUM_NODES}",
        f"--gpusPerNode={GPUS_PER_NODE}",
        "--interNodeFabricType=RoCE",
        "--interNodeTopology=leafspine",
        "--interNodeSpines=1",
        f"--interNodeBandwidth={BW_GBPS}",
        f"--interNodeLatency={LAT_NS}",
        "--randTraffic=1",
        f"--randInjectionRate={INJECTION}",
        f"--randWarmupNs={WARMUP_NS}",
        f"--randMeasureNs={MEASURE_NS}",
        "--RngRun=1",
    ]
    if kind != "dcqcn0":
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
    for k, outk in (("offeredGbps", "randOfferedGbps"), ("deliveredGbps", "randDeliveredGbps"),
                    ("ecnMarkedPackets", "ecnMarkedPackets"),
                    ("cnpPacketsSent", "cnpPacketsSent"), ("cnpPacketsReceived", "cnpPacketsReceived")):
        row[k] = kv.get(outk, "")
    return row


def main():
    jobs = [("dcqcn0", 8, 0.5, 50000)]
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
            print(f"{desc}  {row['status']:8s} del={row['deliveredGbps'] or '-'} "
                  f"off={row['offeredGbps'] or '-'} ecn={row['ecnMarkedPackets'] or '-'}",
                  flush=True)
            rows.append(row)
    order = {"dcqcn0": 0, "grid": 1}
    rows.sort(key=lambda r: (order[r["runType"]],
                             int(r["ecnThreshold"]), float(r["dcqcnGInit"]),
                             int(r["dcqcnTFastNs"])))
    out = HERE / "results_dcqcn_rand.csv"
    with out.open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=FIELDS)
        w.writeheader()
        w.writerows(rows)
    bad = [r for r in rows if r["status"] != "complete"]
    print(f"\nwrote {out} ({len(rows)} rows, {len(bad)} not complete)")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
