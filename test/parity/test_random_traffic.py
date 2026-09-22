#!/usr/bin/env python3
"""Smoke gate for the --randTraffic uniform-random So traffic mode.

Two checks (run this script DIRECTLY, not under pytest -- it drives the
debug sim binary like the other parity gates):

1. Non-blocking baseline (tor, 2n x 4g, rate 0.8): the run must complete
   in fixed duration and deliver ~offered load (no congestion, no ECN).
2. Oversubscribed DCQCN activation (leafspine --interNodeSpines=1, 4:1
   oversubscription, rate 0.8, --dcqcn): the ECN -> CNP -> DCQCN chain
   must be ALIVE: ecnMarkedPackets > 0, cnpPacketsSent > 0, and
   cnpPacketsReceived > 0 (CNPs make it back through the congested
   fabric within the window; needs the 1 us link latency + 1 ms window
   below -- with the 10 us default latency the echo path is too slow).

Delivered throughput is NOT pinned to a byte-exact value here (it is a
measurement, not a protocol invariant); we assert the physical bounds
instead: delivered <= offered, and delivered under 4:1 oversubscription
lands well below offered (congestion is real).
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
    "--gpusPerNode=4",
    "--interNodeFabricType=RoCE",
    "--interNodeBandwidth=400",
    "--interNodeLatency=1000",
    "--randTraffic=1",
    "--randInjectionRate=0.8",
    "--randWarmupNs=200000",
    "--randMeasureNs=800000",
]


def run(args, timeout=600):
    """Run the sim; parse the RESULT block into a dict."""
    proc = subprocess.Popen(
        [str(BIN)] + args,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
        start_new_session=True,
    )
    try:
        out, _ = proc.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        os.killpg(proc.pid, signal.SIGKILL)
        raise SystemExit(f"FAIL: sim timed out: {' '.join(args)}")
    result = {}
    for line in out.splitlines():
        if "=" in line:
            k, _, v = line.partition("=")
            result[k.strip()] = v.strip()
    return result


def main():
    if not BIN.exists():
        raise SystemExit(f"FAIL: sim binary not found: {BIN} (run ./ns3 build)")

    # --- Check 1: non-blocking tor completes and delivers ~offered ---------
    r = run(COMMON + ["--interNodeTopology=tor"])
    assert r.get("status") == "complete", f"tor status={r.get('status')}"
    offered = float(r["randOfferedGbps"])
    delivered = float(r["randDeliveredGbps"])
    assert delivered > 0, "tor delivered nothing"
    assert delivered >= 0.9 * offered, (
        f"tor (non-blocking) delivered {delivered} << offered {offered}"
    )
    print(f"PASS tor: delivered {delivered:.1f} / offered {offered:.1f} Gbps")

    # --- Check 2: oversubscribed leafspine lights up ECN/CNP/DCQCN ---------
    r = run(COMMON + [
        "--interNodeTopology=leafspine",
        "--interNodeSpines=1",  # 4:1 oversubscription at the leaf uplink
        "--dcqcn=1",
    ])
    assert r.get("status") == "complete", f"leafspine status={r.get('status')}"
    offered = float(r["randOfferedGbps"])
    delivered = float(r["randDeliveredGbps"])
    ecn = int(r["ecnMarkedPackets"])
    cnp_tx = int(r["cnpPacketsSent"])
    cnp_rx = int(r["cnpPacketsReceived"])
    assert delivered > 0, "leafspine delivered nothing"
    assert delivered < 0.5 * offered, (
        f"4:1 oversubscription should congest: delivered {delivered} "
        f"vs offered {offered}"
    )
    assert ecn > 0, "ECN never marked under real congestion"
    assert cnp_tx > 0, "receivers never echoed CNPs"
    assert cnp_rx > 0, "CNPs never reached the senders"
    print(
        f"PASS leafspine 4:1: delivered {delivered:.1f} / offered "
        f"{offered:.1f} Gbps, ecnMarked={ecn} cnpTx={cnp_tx} cnpRx={cnp_rx}"
    )
    print("OK: random-traffic mode + DCQCN chain alive")


if __name__ == "__main__":
    main()
