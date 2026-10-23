#!/usr/bin/env python3
"""Gate: DCQCN chain on the inline COLLECTIVE path (So payload-builder seam).

Inline runs mount the NCCL payload builder, which never stamps a SoHeader --
so before the So payload-builder seam, --dcqcn=1 on an inline hierarchical
collective left the whole ECN->CNP->DCQCN chain dormant (zero ECN marks under
genuine congestion). With the seam, --dcqcn=1 on a RoCE fabric mounts a
dedicated So RocePayloadBuilder: inter-node sends carry SoHeader +
RoceProtocol::SIMPLE while Su (intra-node) framing keeps the NCCL builder.

Checks (4n x 8g, leafspine --interNodeSpines=1 = 8:1 oversubscription,
hierarchical allreduce 16 MiB):
  1. --dcqcn=0: completes, ecnMarkedPackets == 0 (seam unmounted, dormant).
  2. --dcqcn=1: completes, ecnMarkedPackets > 0, cnpPacketsSent > 0,
     cnpPacketsReceived > 0 (chain alive on the collective path).

Run directly (not under pytest): python3 test/parity/test_collective_dcqcn.py
"""
import os
import signal
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
BIN = REPO / "build" / "scratch" / "ns3.47-gpu-cluster-sim-debug"

COMMON = [
    "--numNodes=4",
    "--gpusPerNode=8",
    "--collective=allreduce",
    "--algorithm=hierarchical",
    "--dataSize=16777216",
    "--interNodeFabricType=RoCE",
    "--interNodeTopology=leafspine",
    "--interNodeSpines=1",
    "--interNodeBandwidth=400",
    "--interNodeLatency=1000",
    "--RngRun=1",
]


def run(extra):
    proc = subprocess.Popen([str(BIN)] + COMMON + extra,
                            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                            text=True, start_new_session=True)
    try:
        out, _ = proc.communicate(timeout=600)
    except subprocess.TimeoutExpired:
        os.killpg(proc.pid, signal.SIGKILL)
        proc.wait()
        raise SystemExit("FAIL: run timed out")
    kv = {}
    for line in out.splitlines():
        if "=" in line and " " not in line.split("=", 1)[0]:
            k, v = line.split("=", 1)
            kv[k] = v
    return kv


def main():
    off = run(["--dcqcn=0"])
    assert off.get("status") == "complete", f"dcqcn=0 status={off.get('status')}"
    assert int(off.get("ecnMarkedPackets", -1)) == 0, \
        f"dcqcn=0 must be dormant, ecn={off.get('ecnMarkedPackets')}"
    print(f"PASS dcqcn=0: complete, ecn=0 (dormant), simTimeNs={off.get('simTimeNs')}")

    on = run(["--dcqcn=1"])
    assert on.get("status") == "complete", f"dcqcn=1 status={on.get('status')}"
    ecn = int(on.get("ecnMarkedPackets", 0))
    tx = int(on.get("cnpPacketsSent", 0))
    rx = int(on.get("cnpPacketsReceived", 0))
    assert ecn > 0 and tx > 0 and rx > 0, \
        f"dcqcn=1 chain dead: ecn={ecn} cnpTx={tx} cnpRx={rx}"
    print(f"PASS dcqcn=1: complete, ecn={ecn} cnpTx={tx} cnpRx={rx}, "
          f"simTimeNs={on.get('simTimeNs')}")
    print("OK: DCQCN chain alive on the inline collective path")


if __name__ == "__main__":
    main()
