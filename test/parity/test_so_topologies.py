#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Completion + losslessness smoke across ALL inter-node (So) topologies.

``MultiNodeTopologyHelper`` supports eight ``--interNodeTopology`` values in
three wiring shapes:

- one-NIC-per-GPU switched (``host`` / ``tor`` / ``leafspine``): every remote
  rank goes out the GPU's own NIC, the switch tier forwards by static route;
- rdmaRouteMap + intra-node proxy (``fullmesh`` / ``rail``): same-local-rank
  pairs get a direct path (private link / one rail-switch hop), cross-rank
  goes PXN-style via the same-local-rank proxy GPU;
- GPU-0 gateway (``ring`` / ``torus2d`` / ``dragonfly``): all inter-node
  traffic funnels through each node's GPU 0, multi-hop paths are forwarded
  hop-by-hop by the intermediate gateways' routing tables.

For each topology this test runs a small hierarchical allreduce over the RoCE
So fabric and asserts it completes losslessly at the PINNED simTimeNs (the
sim is deterministic; a pin change means the topology wiring changed).
Gateway topologies use 4 nodes (so torus2d is a genuine 2x2 grid and
dragonfly has 2 groups of 2 with a real global link); switched/proxy
topologies use 2 nodes. Data size is kept at 1 MiB so the gateway funnels
stay fast; ``test_so_hierarchical_regression.py`` remains the 256 MiB
losslessness gate for the pinned ``host`` path.

Run:
    python3 -m pytest test/parity/test_so_topologies.py -v
    # or directly:
    python3 test/parity/test_so_topologies.py
"""

import glob
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

DATA_SIZE = 1 << 20  # 1 MiB

# topology -> (numNodes, extra args, pinned simTimeNs or None on first record)
TOPOLOGIES = {
    "fullmesh": (2, []),
    "ring": (2, []),
    "host": (2, []),
    "tor": (2, []),
    "rail": (2, []),
    "leafspine": (2, []),
    "torus2d": (4, ["--interNodeTorusWidth=2"]),
    "dragonfly": (4, ["--interNodeDragonflyGroupSize=2"]),
}

# Deterministic pins (RngRun=1, gpusPerNode=8, 1 MiB hierarchical allreduce,
# RoCE So fabric). Regenerate deliberately with --record if a topology's
# wiring is intentionally changed.
PINS_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                         "so_topologies_pins.json")


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


def run_sim(args, timeout=600):
    """Run gpu-cluster-sim with args; return parsed RESULT block fields."""
    binary = find_sim_binary()
    if binary:
        cmd = [binary] + args
    else:
        cmd = ["./ns3", "run", "gpu-cluster-sim " + " ".join(args)]
    proc = subprocess.run(
        cmd, cwd=ROOT, capture_output=True, text=True, timeout=timeout
    )
    out = proc.stdout + proc.stderr
    fields = {}

    def grab(key, cast):
        m = re.search(r"^{}=(\S+)".format(re.escape(key)), out, re.MULTILINE)
        if m:
            try:
                fields[key] = cast(m.group(1))
            except ValueError:
                fields[key] = m.group(1)

    grab("status", str)
    grab("simTimeNs", int)
    grab("switchDroppedPackets", int)
    grab("permanentLosses", int)
    return fields, out


def run_topology(topo):
    num_nodes, extra = TOPOLOGIES[topo]
    args = [
        "--numNodes={}".format(num_nodes),
        "--gpusPerNode=8",
        "--interNodeTopology={}".format(topo),
        "--interNodeFabricType=RoCE",
        "--collective=allreduce",
        "--algorithm=hierarchical",
        "--dataSize={}".format(DATA_SIZE),
        "--RngRun=1",
    ] + extra
    return run_sim(args)


def load_pins():
    import json
    if not os.path.exists(PINS_FILE):
        return {}
    with open(PINS_FILE) as f:
        return json.load(f)


def check_topology(topo, pins):
    fields, out = run_topology(topo)
    assert fields.get("status") == "complete", (
        "{}: did not complete: {}".format(topo, out[-2000:]))
    assert fields.get("switchDroppedPackets", 0) == 0, (
        "{}: switch drops on lossless RoCE fabric".format(topo))
    assert fields.get("permanentLosses", 0) == 0, (
        "{}: permanent losses on lossless RoCE fabric".format(topo))
    sim_ns = fields["simTimeNs"]
    if topo in pins:
        assert sim_ns == pins[topo], (
            "{}: simTimeNs={} != pinned {} (topology wiring changed?)".format(
                topo, sim_ns, pins[topo]))
    return sim_ns


def main(record=False):
    import json
    pins = {} if record else load_pins()
    results = {}
    for topo in TOPOLOGIES:
        sim_ns = check_topology(topo, pins)
        results[topo] = sim_ns
        pinned = " (== pin)" if topo in pins else " (recorded)"
        print("PASS  {:<10} {}n  simTimeNs={}{}".format(
            topo, TOPOLOGIES[topo][0], sim_ns, pinned))
    if record or not os.path.exists(PINS_FILE):
        with open(PINS_FILE, "w") as f:
            json.dump(results, f, indent=2, sort_keys=True)
            f.write("\n")
        print("pins written to", PINS_FILE)
    return 0


# pytest entry points
def _make_test(topo):
    def _test():
        check_topology(topo, load_pins())
    _test.__name__ = "test_so_topology_{}".format(topo)
    return _test


for _topo in TOPOLOGIES:
    globals()["test_so_topology_{}".format(_topo)] = _make_test(_topo)


if __name__ == "__main__":
    sys.exit(main(record="--record" in sys.argv))
