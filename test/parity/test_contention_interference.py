#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Automated gate for multi-operation temporal contention modeling.

The ``--bgP2p*`` CLI (commit 93d86a2) injects a self-driven ``P2pTraffic`` flow
at ``--bgP2pStartTimeNs`` that shares the fabric with the primary collective,
exercising the WFQ contention model (multi-class ``numActive>1``) at the
endpoint injection side and the So switch VOQ arbitration. This test converts
the manual smoke (CHANGES.md / PLAN.md) into a machine-checked gate: the
interference run must (a) still complete, (b) actually emit background P2P
bytes, and (c) be strictly slower than the no-interference baseline -- a
regression that silently disables the contention gate (e.g. ``numActive``
mis-counted, the WFQ penalty branch skipped, or ``P2pTraffic`` not reaching
``SendPacketOnDevice``) collapses to ``bg simTimeNs == base simTimeNs`` and
``bgP2pBytesSent == 0``.

Two cases, mirroring the two topology regimes:

* ``test_contention_interference_multi_node`` -- 2-node RoCE hierarchical
  allreduce (16 MiB) + a cross-node background P2P (rank 0 -> rank 4). So
  contention is modeled at BOTH the endpoint WFQ and the So switch VOQ.
  Baseline 1248928 ns -> interference 1353663 ns (+8.4%).

* ``test_contention_interference_single_node`` -- single-node Su ring
  allreduce (16 MiB) + an intra-node background P2P (rank 0 -> rank 2).
  Contention is the shared-NVSwitch injection-side WFQ. Baseline 319658 ns
  -> interference 450982 ns (+41%); the Su impact is larger because the
  background flow contends directly at the shared NVSwitch injection side.

Note on the single- vs multi-node modeling difference (a DESIGN choice, not
a bug): the single-node ``GpuClusterTopologyHelper`` shares ONE
``ContentionModel`` clone across all its endpoints (backlog aggregates
cross-endpoint -- it models the shared NVSwitch egress), whereas the
multi-node path mounts an INDEPENDENT per-endpoint instance (it models each
NIC injection side). This test only checks that interference takes effect in
BOTH regimes, not that the two magnitudes match.

Run:
    python3 -m pytest test/parity/test_contention_interference.py -v
    python3 test/parity/test_contention_interference.py
"""

import glob
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def find_sim_binary():
    cands = []
    for b in glob.glob(os.path.join(ROOT, "build", "scratch", "*gpu-cluster-sim*")):
        if b.endswith(".o") or not os.access(b, os.X_OK):
            continue
        cands.append(b)
    rel = [c for c in cands if "debug" not in os.path.basename(c)]
    if rel:
        return rel[0]
    return cands[0] if cands else None


def run_sim(extra_args, timeout=300):
    """Run the sim with extra CLI args; return parsed RESULT fields + raw out."""
    binary = find_sim_binary()
    if binary:
        cmd = [binary] + extra_args
    else:
        cmd = ["./ns3", "run", "gpu-cluster-sim " + " ".join(extra_args)]
    proc = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True,
                          timeout=timeout)
    out = proc.stdout + proc.stderr
    fields = {}

    def grab(key, cast):
        m = re.search(r"^{}=(\S+)".format(re.escape(key)), out, re.MULTILINE)
        if m:
            try:
                fields[key] = cast(m.group(1))
            except ValueError:
                fields[key] = m.group(1)

    for k in ("status", "simTimeNs", "bgP2pEnabled",
              "bgP2pBytesSent", "bgP2pPacketsSent"):
        grab(k, (int if k not in ("status", "bgP2pEnabled") else str))
    return fields, out


# Multi-node: 2-node RoCE hierarchical allreduce, 16 MiB, cross-node bg P2P.
MULTI_BASE = [
    "--numNodes=2", "--gpusPerNode=4",
    "--collective=allreduce", "--algorithm=hierarchical",
    "--dataSize=16777216",
    "--interNodeFabricType=RoCE", "--interNodeTopology=host",
    "--interNodeBandwidth=400", "--interNodeLatency=100",
    "--RngRun=1",
]
MULTI_BG = MULTI_BASE + [
    "--bgP2pEnabled=1", "--bgP2pSrc=0", "--bgP2pDst=4",
    "--bgP2pSizeBytes=1048576", "--bgP2pStartTimeNs=5000",
]

# Single-node: Su ring allreduce, 16 MiB, intra-node bg P2P.
SINGLE_BASE = [
    "--numGpus=4",
    "--collective=allreduce", "--algorithm=ring",
    "--dataSize=16777216",
    "--RngRun=1",
]
SINGLE_BG = SINGLE_BASE + [
    "--bgP2pEnabled=1", "--bgP2pSrc=0", "--bgP2pDst=2",
    "--bgP2pSizeBytes=1048576", "--bgP2pStartTimeNs=5000",
]


def _assert_interference(base, bg, out, regime):
    assert base.get("status") == "complete", (
        "{} baseline did not complete:\n{}".format(regime, out[-500:]))
    assert bg.get("status") == "complete", (
        "{} interference run did not complete (bg P2P may have deadlocked the "
        "collective):\n{}".format(regime, out[-500:]))
    assert bg.get("bgP2pEnabled") == "true", (
        "{}: bgP2pEnabled not reflected in result block".format(regime))
    assert bg.get("bgP2pBytesSent", 0) > 0, (
        "{}: bgP2pBytesSent={} (the background P2pTraffic never reached the "
        "wire -- SendP2p/SendPacketOnDevice regressed?)"
        .format(regime, bg.get("bgP2pBytesSent")))
    assert bg.get("bgP2pPacketsSent", 0) > 0, (
        "{}: bgP2pPacketsSent={}".format(regime, bg.get("bgP2pPacketsSent")))
    base_ns = base.get("simTimeNs")
    bg_ns = bg.get("simTimeNs")
    assert base_ns is not None and bg_ns is not None, (
        "{}: simTimeNs missing (base={} bg={})".format(regime, base_ns, bg_ns))
    assert bg_ns > base_ns, (
        "{}: interference simTimeNs ({}) not strictly greater than baseline "
        "({}) -- the WFQ contention gate did not slow the collective under the "
        "background flow (numActive>1 penalty branch regressed, or P2P class "
        "not classified).".format(regime, bg_ns, base_ns))


def test_contention_interference_multi_node():
    base, _ = run_sim(MULTI_BASE)
    bg, out = run_sim(MULTI_BG)
    _assert_interference(base, bg, out, "multi-node")


def test_contention_interference_single_node():
    base, _ = run_sim(SINGLE_BASE)
    bg, out = run_sim(SINGLE_BG)
    _assert_interference(base, bg, out, "single-node")


if __name__ == "__main__":
    ok = True
    for name, fn in (("multi_node", test_contention_interference_multi_node),
                     ("single_node", test_contention_interference_single_node)):
        try:
            fn()
            print("PASS  contention_interference  {}".format(name))
        except AssertionError as e:
            ok = False
            print("FAIL  contention_interference  {}  {}".format(name, e))
    sys.exit(0 if ok else 1)
