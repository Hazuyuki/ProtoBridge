#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Surrogate-vs-ns3 anchor for the hierarchical (Su+So) collective.

The hierarchical surrogate (``make_h200_hierarchical_theory``) is an
UNCALIBRATED LOWER BOUND anchored to ns-3, not to real HW: its So term is
spec-derived (400GbE RoCEv2, 40 us startup, 0.95 wire efficiency) and its Su
term reuses the HW-calibrated NVLink ring rate. This test ties that lower bound
to the ns-3 packet-level simulation: for a 2-node hierarchical allreduce, the
surrogate's predicted latency must be at or below the ns-3 measured latency
(the analytical bus-bandwidth model omits per-packet credit flow, VOQ
arbitration, PFC/MTU overhead, and per-phase barrier syncs that ns-3 models).

The band is deliberately LOOSE: ns-3 here runs with DEFAULT fabric params
(800 GB/s Su, 400 GbE So, ``intraNodeTopology=switched``), while the surrogate
uses the HW-calibrated Su ring rate + spec-derived So -- so the gap reflects
both the analytical-vs-packet-level difference AND the Su-fabric mismatch
(ns-3's hierarchical Su phase runs on the switched fabric, which the
h200-ring-allreduce.cfg warns is ~6x slower than the calibrated P2P ring the
surrogate's Su term assumes). A tight, param-matched anchor is deferred
(uncalibrated-lower-bound scope; would require back-fitting the So
startup/congestion term to ns-3, i.e. calibration).

Run:
    python3 -m pytest test/parity/test_surrogate_vs_ns3_hierarchical.py -v
    python3 test/parity/test_surrogate_vs_ns3_hierarchical.py
"""

import glob
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "surrogate", "theory"))
import whitebox_surrogate_v2 as wb  # noqa: E402

# Declared lower-bound band: the surrogate must sit BELOW ns-3 (it is a lower
# bound) but not absurdly far below (floor guards against a degenerate model).
LOWER_BOUND_FLOOR = 0.05       # surrogate >= 5% of ns-3 (sanity, not tightness)

# So fabric the surrogate models; must match the ns-3 --interNodeTopology=
# host (fullmesh-of-switches, non-blocking) <=> surrogate so_fullmesh.
SO_FAMILY = "so_fullmesh"
GPUS_PER_NODE = 8
DATA_BYTES = 1 << 20 << 8       # 256 MiB -- the WU4 e2e size
NODE_COUNTS = (2, 4)


def find_sim_binary():
    """Locate the built gpu-cluster-sim binary; None => use `./ns3 run`."""
    cands = []
    for b in glob.glob(os.path.join(ROOT, "build", "scratch", "*gpu-cluster-sim*")):
        if b.endswith(".o") or not os.access(b, os.X_OK):
            continue
        cands.append(b)
    rel = [c for c in cands if "debug" not in os.path.basename(c)]
    if rel:
        return rel[0]
    return cands[0] if cands else None


def run_ns3_hierarchical(num_nodes):
    """Run the ns-3 hierarchical allreduce; return parsed simTimeNs (us)."""
    binary = find_sim_binary()
    args = [
        "--numNodes={}".format(num_nodes),
        "--gpusPerNode={}".format(GPUS_PER_NODE),
        "--interNodeTopology=host",
        "--interNodeFabricType=RoCE",
        "--collective=allreduce",
        "--algorithm=hierarchical",
        "--dataSize={}".format(DATA_BYTES),
        "--RngRun=1",
    ]
    if binary:
        cmd = [binary] + args
    else:
        cmd = ["./ns3", "run", "gpu-cluster-sim " + " ".join(args)]
    proc = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True,
                          timeout=600)
    out = proc.stdout + proc.stderr
    status = re.search(r"^status=(\S+)", out, re.MULTILINE)
    sim_ns = re.search(r"^simTimeNs=(\d+)", out, re.MULTILINE)
    assert status and status.group(1) == "complete", \
        "ns-3 hierarchical {}-node did not complete: {}".format(num_nodes, out[-400:])
    assert sim_ns is not None, "ns-3 {}-node missing simTimeNs".format(num_nodes)
    return int(sim_ns.group(1)) / 1000.0      # ns -> us


def surrogate_hierarchical(num_nodes):
    h = wb.make_h200_hierarchical_theory(
        gpus_per_node=GPUS_PER_NODE, num_nodes=num_nodes,
        so_topology_family=SO_FAMILY)
    return h.predict(DATA_BYTES, "ring", credits=128, collective="allreduce")


def test_surrogate_is_lower_bound_of_ns3():
    for n in NODE_COUNTS:
        ns3_us = run_ns3_hierarchical(n)
        surr_us = surrogate_hierarchical(n)
        ratio = surr_us / ns3_us
        assert surr_us <= ns3_us, (
            "{}-node: surrogate {:.1f}us > ns-3 {:.1f}us (lower bound violated)"
            .format(n, surr_us, ns3_us))
        assert ratio >= LOWER_BOUND_FLOOR, (
            "{}-node: surrogate/ns-3 ratio {:.3f} below sanity floor {} "
            "(degenerate surrogate?)".format(n, ratio, LOWER_BOUND_FLOOR))


if __name__ == "__main__":
    ok = True
    for n in NODE_COUNTS:
        try:
            ns3_us = run_ns3_hierarchical(n)
            surr_us = surrogate_hierarchical(n)
            ratio = surr_us / ns3_us
            verdict = "PASS" if surr_us <= ns3_us and ratio >= LOWER_BOUND_FLOOR \
                else "FAIL"
            if verdict == "FAIL":
                ok = False
            print("{}  {}-node  surrogate={:.1f}us  ns3={:.1f}us  "
                  "ratio={:.2f} (lower bound)".format(
                      verdict, n, surr_us, ns3_us, ratio))
        except Exception as e:  # noqa: BLE001
            ok = False
            print("FAIL  {}-node  {}".format(n, e))
    sys.exit(0 if ok else 1)
