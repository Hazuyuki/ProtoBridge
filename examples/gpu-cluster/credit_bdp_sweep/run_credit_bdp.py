#!/usr/bin/env python3
"""BDP (bandwidth-delay-product) credit-window sweep on the Su (intra-node)
fabric: does the credit return delay cap achievable bandwidth utilization?

Scenario: single-node 8-GPU ring allreduce, 256 MiB (the su-parity scenario,
so the unlimited-credit row reproduces the pinned 4769616 ns at 800G/500ns).
Credits are per-packet (1 credit = 1 8-MiB bulk chunk in flight).

Axes:
  bandwidth : 100 / 200 / 400 / 800 / 1600 Gbps
  latency   : 0.5 / 10 / 50 / 100 / 200 us   (one-way link delay; the credit
              return path pays it twice: data -> delivery -> CREDIT -> sender)
  credits   : 1 / 2 / 4 / 8 / 24 / 100000000 (unlimited)

Physics under test: a credit window of W packets sustains at most
    W * chunkBytes / (serialization + 2 * latency)
so once bandwidth exceeds that, raising it further cannot shorten completion:
utilization = T_unlimited / T_credits falls below 1 and stays flat. That is
the "bandwidth rises, utilization cannot reach 100%" phenomenon.

Writes results_credit_bdp.csv; plot with plot_credit_bdp.py.
"""
import concurrent.futures
import csv
import os
import signal
import subprocess
from pathlib import Path

HERE = Path(__file__).resolve().parent
BIN = HERE.parents[2] / "build" / "scratch" / "ns3.47-gpu-cluster-sim-debug"

NUM_GPUS = 8
SIZE = 256 << 20

BANDWIDTHS = [100, 200, 400, 800, 1600]
LATENCIES_NS = [500, 10000, 50000, 100000, 200000]
CREDITS = [1, 2, 4, 8, 24, 100000000]

MAX_PARALLEL = 16
TIMEOUT_S = 600

FIELDS = ["bandwidthGbps", "latencyNs", "vcCredits", "status", "simTimeNs",
          "timeMs", "busBwGbps", "utilization"]


def run_one(job):
    bw, lat, credits = job
    cmd = [
        str(BIN),
        f"--numGpus={NUM_GPUS}",
        "--collective=allreduce",
        "--algorithm=ring",
        f"--dataSize={SIZE}",
        f"--bandwidth={bw}",
        f"--latency={lat}",
        f"--vcCredits={credits}",
        "--RngRun=1",
    ]
    row = dict.fromkeys(FIELDS, "")
    row.update(bandwidthGbps=bw, latencyNs=lat, vcCredits=credits, status="error")
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
    return row


def main():
    jobs = [(bw, lat, c)
            for bw in BANDWIDTHS
            for lat in LATENCIES_NS
            for c in CREDITS]
    print(f"{len(jobs)} runs, {MAX_PARALLEL} parallel...")
    rows = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=MAX_PARALLEL) as ex:
        for i, row in enumerate(ex.map(run_one, jobs)):
            rows.append(row)
            print(f"[{i + 1}/{len(jobs)}] bw={row['bandwidthGbps']}G "
                  f"lat={row['latencyNs']}ns credits={row['vcCredits']} "
                  f"{row['status']:8s} {row['simTimeNs']}")
    # Derived metrics: bus bandwidth (ring AR moves 2*(N-1)/N * S bytes) and
    # utilization vs the unlimited-credit run at the same (bw, lat).
    n = NUM_GPUS
    for r in rows:
        if r["simTimeNs"]:
            t_ns = int(r["simTimeNs"])
            r["timeMs"] = f"{t_ns / 1e6:.3f}"
            r["busBwGbps"] = f"{2 * (n - 1) / n * SIZE * 8 / t_ns / 1e9:.1f}"
    unlim = {(r["bandwidthGbps"], r["latencyNs"]): r["simTimeNs"]
             for r in rows if r["vcCredits"] == CREDITS[-1]}
    for r in rows:
        key = (r["bandwidthGbps"], r["latencyNs"])
        t_unlim = unlim.get(key)
        if r["simTimeNs"] and t_unlim:
            r["utilization"] = f"{int(t_unlim) / int(r['simTimeNs']):.3f}"
    out = HERE / "results_credit_bdp.csv"
    with open(out, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=FIELDS)
        w.writeheader()
        w.writerows(rows)
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
