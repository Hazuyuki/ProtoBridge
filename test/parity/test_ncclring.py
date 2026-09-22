#!/usr/bin/env python3
"""Gate for --algorithm=ncclring (real-NCCL global pipelined ring AllReduce).

Pins two invariants on a 2-node x 8-GPU RoCE 400G leafspine allreduce 16 MiB:
 1. ncclring completes with the pinned deterministic completion time.
 2. ncclring is faster than the 4-phase hierarchical schedule on the same
    config (the whole point of chunk pipelining: intra/inter overlap replaces
    the sum of barrier-separated phases).

The default algorithm path is untouched by the ncclring feature; hierarchical
pins live in test_so_hierarchical_regression.py / test_collective_dcqcn.py.

Run directly (NOT via pytest collection): python3 test/parity/test_ncclring.py
"""
import os
import signal
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
BIN = REPO / "build" / "scratch" / "ns3.47-gpu-cluster-sim-debug"

COMMON = [
    "--numNodes=2",
    "--gpusPerNode=8",
    "--numGpus=16",
    "--collective=allreduce",
    "--dataSize=16777216",
    "--interNodeFabricType=RoCE",
    "--interNodeTopology=leafspine",
    "--interNodeBandwidth=400",
    "--interNodeLatency=1000",
    "--RngRun=1",
]

NCCLRING_PIN_NS = 444189


def run(extra):
    cmd = [str(BIN)] + COMMON + extra
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                            text=True, start_new_session=True)
    try:
        out, _ = proc.communicate(timeout=600)
    except subprocess.TimeoutExpired:
        os.killpg(proc.pid, signal.SIGKILL)
        proc.wait()
        raise SystemExit(f"FAIL: timeout: {' '.join(cmd)}")
    kv = {}
    for line in out.splitlines():
        if "=" in line and " " not in line.split("=", 1)[0]:
            k, v = line.split("=", 1)
            kv[k] = v
    return kv


def main():
    ncclring = run(["--algorithm=ncclring"])
    assert ncclring.get("status") == "complete", f"ncclring did not complete: {ncclring}"
    sim_ns = int(ncclring["simTimeNs"])
    assert sim_ns == NCCLRING_PIN_NS, (
        f"ncclring pin drift: simTimeNs={sim_ns} expected {NCCLRING_PIN_NS}")
    print(f"PASS: ncclring 2n x 8g leafspine AR 16MiB simTimeNs={sim_ns}")

    hier = run(["--algorithm=hierarchical"])
    assert hier.get("status") == "complete", f"hierarchical did not complete: {hier}"
    hier_ns = int(hier["simTimeNs"])
    assert sim_ns < hier_ns, (
        f"ncclring ({sim_ns}ns) should beat hierarchical ({hier_ns}ns) via pipelining")
    print(f"PASS: ncclring {sim_ns}ns < hierarchical {hier_ns}ns")
    print("PASS: test_ncclring")


if __name__ == "__main__":
    sys.exit(main())
