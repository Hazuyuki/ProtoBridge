#!/usr/bin/env python3
"""Credit-BDP sweep across traffic PATTERNS (not just allreduce):

  p2p     : 1 flow    rank0 -> rank1                    (1-to-1)
  incast  : 7 flows   ranks 1..7 -> rank0               (many-to-1)
  fanout  : 7 flows   rank0 -> ranks 1..7               (1-to-many)
  alltoall: 56 flows  every rank -> every other rank    (via AlltoAllInjector)

All on the single-node 8-GPU Su fabric, per-FLOW volume fixed at 256 MiB
(alltoall passes 8x256 MiB so each pair carries 256 MiB), so the arms share
the same per-flow BDP. Same physics as run_credit_bdp.py: a window of W
credits (1 credit = one 8-MiB bulk chunk) sustains at most
    W * chunkBytes / (serialization + 2 * latency)
per FLOW; patterns differ in how many flows share each link and how the
windows interact (independent per-sender windows on incast, ONE shared
per-endpoint window across all 7 destinations on fanout).

TOPOLOGY axis (switched, fullmesh): on fullmesh every pair is a DIRECT
1-hop link, so the per-chunk credit RTT is uniformly ser + 2*lat and the
window-sharing effect is isolated with no hop-count confound (fanout =
224 chunks x one window = 7.1x p2p at W=1; p2p == incast); on the
switched fabric every pair is 2 hops through the switch, adding a switch
traversal to the credit RTT.

Writes results_credit_patterns.csv; plot with plot_credit_patterns.py.
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
PER_FLOW = 256 << 20

# alltoall's injector divides dataSize by N for the per-pair chunk, so pass
# 8x the per-flow volume to keep every pair at 256 MiB.
PATTERNS = {
    "p2p":     ("p2p", PER_FLOW),
    "incast":  ("incast", PER_FLOW),
    "fanout":  ("fanout", PER_FLOW),
    "alltoall": ("alltoall", PER_FLOW * NUM_GPUS),
}

BANDWIDTHS = [100, 200, 400, 800, 1600]
LATENCIES_NS = [500, 10000, 50000, 100000, 200000]
CREDITS = [1, 2, 4, 8, 24, 100000000]
TOPOLOGIES = ["fullmesh", "switched"]

MAX_PARALLEL = 16
TIMEOUT_S = 600

FIELDS = ["pattern", "topology", "bandwidthGbps", "latencyNs", "vcCredits",
          "status", "simTimeNs", "timeMs", "flows", "bytesPerFlow",
          "totalBytes", "utilization"]


def run_one(job):
    pattern, topo, bw, lat, credits = job
    collective, size = PATTERNS[pattern]
    cmd = [
        str(BIN),
        f"--numGpus={NUM_GPUS}",
        f"--collective={collective}",
        f"--dataSize={size}",
        f"--bandwidth={bw}",
        f"--latency={lat}",
        f"--vcCredits={credits}",
        f"--topology={topo}",
        "--RngRun=1",
    ]
    if collective == "alltoall":
        cmd.insert(3, "--algorithm=ring")
    flows = {"p2p": 1, "incast": NUM_GPUS - 1, "fanout": NUM_GPUS - 1,
             "alltoall": NUM_GPUS * (NUM_GPUS - 1)}[pattern]
    row = dict.fromkeys(FIELDS, "")
    row.update(pattern=pattern, topology=topo, bandwidthGbps=bw, latencyNs=lat,
               vcCredits=credits, status="error", flows=flows,
               bytesPerFlow=PER_FLOW, totalBytes=flows * PER_FLOW)
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
    jobs = [(p, topo, bw, lat, c)
            for p in PATTERNS
            for topo in TOPOLOGIES
            for bw in BANDWIDTHS
            for lat in LATENCIES_NS
            for c in CREDITS]
    print(f"{len(jobs)} runs, {MAX_PARALLEL} parallel...")
    rows = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=MAX_PARALLEL) as ex:
        for i, row in enumerate(ex.map(run_one, jobs)):
            rows.append(row)
            print(f"[{i + 1}/{len(jobs)}] {row['pattern']:8s} {row['topology']:8s} "
                  f"bw={row['bandwidthGbps']}G "
                  f"lat={row['latencyNs']}ns W={row['vcCredits']} "
                  f"{row['status']:8s} {row['simTimeNs']}")
    for r in rows:
        if r["simTimeNs"]:
            r["timeMs"] = f"{int(r['simTimeNs']) / 1e6:.3f}"
    unlim = {(r["pattern"], r["topology"], r["bandwidthGbps"], r["latencyNs"]): r["simTimeNs"]
             for r in rows if r["vcCredits"] == CREDITS[-1]}
    for r in rows:
        key = (r["pattern"], r["topology"], r["bandwidthGbps"], r["latencyNs"])
        t_u = unlim.get(key)
        if r["simTimeNs"] and t_u:
            r["utilization"] = f"{int(t_u) / int(r['simTimeNs']):.3f}"
    out = HERE / "results_credit_patterns.csv"
    with open(out, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=FIELDS)
        w.writeheader()
        w.writerows(rows)
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
