#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Surrogate-vs-ns3 anchor for the plain (non-RoCE) Ethernet So fabric.

Mirrors ``test_surrogate_vs_ns3_hierarchical.py`` (the RoCE So-collective
lower-bound anchor) but swaps the So fabric to plain Ethernet
(``--interNodeFabricType=ETH``) and the So profile to
:func:`make_h200_ethernet_theory` (via the ``so_factory`` seam). The ns-3
command is otherwise identical to the RoCE hierarchical anchor: a 2-/4-node
hierarchical allreduce (256 MiB) over a non-blocking ``host`` So fabric.

This is a LOWER-BOUND anchor, NOT a tight back-fit:

* The Ethernet So profile is spec-derived (store-and-forward latency + the
  400GbE wire), exactly like the RoCE So profile (both are "uncalibrated
  lower bounds anchored to ns-3"). There is no perftest/blog ground truth for
  plain Ethernet to back-fit against (unlike RoCE's ib_write_lat), so no
  single scalar is tuned -- the profile reproduces the ns-3 mean's *shape*,
  not its magnitude.
* The hierarchical model omits per-packet credit flow, VOQ arbitration, the
  inter-phase barrier, and per-packet/bulk-chunk serialization that ns-3
  models, so the surrogate sits *below* ns-3 (measured ratio ~0.2, identical
  to the RoCE hierarchical anchor's ~0.2 -- the So stage is a small fraction
  of the uncongested hierarchical, so Ethernet and RoCE are nearly
  indistinguishable here; the store-and-forward latency is the only So
  difference and it is small at 256 MiB).

The lossy-congestion behavior (VOQ tail-drop, the switch-emitted NACK, and
LLR/SACK retransmission) is ns-3-only -- the surrogate is the *uncongested*
mean, the same split as RoCE (whose DCQCN/PFC/per-QP-ceiling also live in
ns-3, not the surrogate). This test therefore runs ``--ber=0`` over a
non-blocking fabric so no drop/retransmit path engages.

Run:
    python3 -m pytest test/parity/test_surrogate_vs_ns3_ethernet.py -v
    python3 test/parity/test_surrogate_vs_ns3_ethernet.py
"""

import glob
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "surrogate", "theory"))
import whitebox_surrogate_v2 as wb  # noqa: E402

# So fabric the surrogate models; must match the ns-3 --interNodeTopology=
# host (fullmesh-of-switches, non-blocking) <=> surrogate so_fullmesh.
SO_FAMILY = "so_fullmesh"
GPUS_PER_NODE = 8
DATA_BYTES = 1 << 20 << 8       # 256 MiB -- the WU4 e2e size
NODE_COUNTS = (2, 4)

# Declared lower-bound band: the surrogate must sit BELOW ns-3 (it is a lower
# bound) but not absurdly far below (floor guards against a degenerate model).
# Measured ~0.2 (identical to the RoCE hierarchical anchor); 0.10 is a ~2x
# conservative sanity floor, not a tightness claim.
LOWER_BOUND_FLOOR = 0.10


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


def run_ns3_ethernet(num_nodes):
    """Run the ns-3 hierarchical allreduce over a plain-Ethernet So fabric;
    return parsed simTimeUs (float)."""
    binary = find_sim_binary()
    args = [
        "--numNodes={}".format(num_nodes),
        "--gpusPerNode={}".format(GPUS_PER_NODE),
        "--interNodeTopology=host",
        "--interNodeFabricType=ETH",
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
    sim_us = re.search(r"^simTimeUs=(\S+)", out, re.MULTILINE)
    assert status and status.group(1) == "complete", \
        "ns-3 ETH hierarchical {}-node did not complete: {}".format(
            num_nodes, out[-400:])
    assert sim_us is not None, "ns-3 ETH {}-node missing simTimeUs".format(num_nodes)
    return float(sim_us.group(1))


def surrogate_ethernet(num_nodes):
    h = wb.make_h200_hierarchical_theory(
        gpus_per_node=GPUS_PER_NODE, num_nodes=num_nodes,
        so_topology_family=SO_FAMILY, so_factory=wb.make_h200_ethernet_theory)
    return h.predict(DATA_BYTES, "ring", credits=128, collective="allreduce")


def _check(num_nodes):
    ns3_us = run_ns3_ethernet(num_nodes)
    surr_us = surrogate_ethernet(num_nodes)
    return ns3_us, surr_us, surr_us / ns3_us


def test_surrogate_is_lower_bound_of_ns3():
    for n in NODE_COUNTS:
        ns3_us, surr_us, ratio = _check(n)
        assert surr_us <= ns3_us, (
            "{}-node: Ethernet surrogate {:.1f}us > ns-3 {:.1f}us "
            "(lower bound violated)".format(n, surr_us, ns3_us))
        assert ratio >= LOWER_BOUND_FLOOR, (
            "{}-node: Ethernet surrogate/ns-3 ratio {:.3f} below sanity floor "
            "{} (degenerate surrogate?)".format(n, ratio, LOWER_BOUND_FLOOR))


if __name__ == "__main__":
    ok = True
    for n in NODE_COUNTS:
        try:
            ns3_us, surr_us, ratio = _check(n)
            verdict = "PASS" if (surr_us <= ns3_us
                                 and ratio >= LOWER_BOUND_FLOOR) else "FAIL"
            if verdict == "FAIL":
                ok = False
            print("{}  {}-node  Ethernet surrogate={:.1f}us  ns-3={:.1f}us  "
                  "ratio={:.3f}".format(verdict, n, surr_us, ns3_us, ratio))
        except Exception as e:  # noqa: BLE001
            ok = False
            print("FAIL  {}-node  {}".format(n, e))
    sys.exit(0 if ok else 1)
