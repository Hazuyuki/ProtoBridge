#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Regression smoke test for the scale-out (Ethernet RoCE v2) So fabric.

The config-file primary path (``--protocolConfig=...cfg``) sources only
single-node fabric-hardware keys + the ``collective``/``algorithm``/``topology``
op axes into the validated inline injectors; it does **not** source
``numNodes``/``gpusPerNode``/``interNode*``. The multi-node So fabric is therefore
exercised via the scratch inline path (``MultiNodeTopologyHelper`` + the
hierarchical injectors dispatched at ``numNodes > 1``), not via a ``.cfg``.
This test locks in that scratch So path's behaviour: a hierarchical allreduce
over the real ``EthernetSwitch`` RoCE fabric completes, is lossless, and costs
more than the single-node scale-up (Su) allreduce (the So phase adds inter-node
time).

This is the So-path counterpart to ``test_config_vs_inline_parity.py``: where
that test gates the config-vs-inline bit-identity of the Su calibrated path,
this test gates the existence + losslessness + latency ordering of the So
RoCEv2 fabric. Config-primary So parity (a ``.cfg`` reproducing the hierarchical
inline path) is deferred until the config path sources multi-node axes.

Run:
    python3 -m pytest test/parity/test_so_hierarchical_regression.py -v
    # or directly:
    python3 test/parity/test_so_hierarchical_regression.py
"""

import glob
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


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
        cwd = ROOT
    else:
        cmd = ["./ns3", "run", "gpu-cluster-sim " + " ".join(args)]
        cwd = ROOT
    proc = subprocess.run(
        cmd, cwd=cwd, capture_output=True, text=True, timeout=timeout
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


# (numNodes, dataSizeBytes). 2 nodes = the minimal So fabric (one inter-node
# hop); 4 nodes = losslessness across a multi-hop fullmesh-of-switches. Both
# at 256 MiB (the WU4 e2e size). Kept to two multi-node runs to bound test
# time; the non-blocking fullmesh plateaus (per-node NIC is the bottleneck, not
# the inter-switch fabric), so this asserts losslessness across N, not N-scaling.
SO_MATRIX = [
    (2, 1 << 20 << 8),
    (4, 1 << 20 << 8),
]


def run_so_hierarchical(num_nodes, data_size):
    args = [
        "--numNodes={}".format(num_nodes),
        "--gpusPerNode=8",
        "--interNodeTopology=host",
        "--interNodeFabricType=RoCE",
        "--collective=allreduce",
        "--algorithm=hierarchical",
        "--dataSize={}".format(data_size),
        "--RngRun=1",
    ]
    return run_sim(args)


def run_su_baseline(data_size):
    """Single-node Su ring allreduce (no So phase) -- the lower bound."""
    args = [
        "--numGpus=8",
        "--collective=allreduce",
        "--algorithm=ring",
        "--dataSize={}".format(data_size),
        "--RngRun=1",
    ]
    return run_sim(args, timeout=300)


def assert_lossless(fields, label):
    assert fields.get("status") == "complete", (
        "{}: status={} (expected complete)".format(label, fields.get("status"))
    )
    assert fields.get("switchDroppedPackets") == 0, (
        "{}: switchDroppedPackets={} (RoCE So fabric must be lossless)".format(
            label, fields.get("switchDroppedPackets")
        )
    )
    assert fields.get("permanentLosses") == 0, (
        "{}: permanentLosses={} (So fabric must not lose data)".format(
            label, fields.get("permanentLosses")
        )
    )
    assert fields.get("simTimeNs") is not None, "{}: missing simTimeNs".format(label)


# Byte-identical RoCE parity reference (RngRun=1, deterministic). These are
# the canonical "byte-identical" numbers cited in PLAN.md / TASKS.md /
# CHANGES.md; pinning them turns that doc claim into a machine-checked gate --
# a 1-ns drift means the void->bool OnPacketEnqueue change or the ETHERNET-only
# LLR force has disturbed the RoCE path.
PARITY_NS = {
    "su-baseline": 4769616,
    # 2/4-node re-pinned 15307388/13421212 -> 15287388/13403612 (cut-through
    # accounting fix: egress busy-until is serialization-only; the 200-ns
    # pipeline overlaps the serializer instead of derating small-chunk
    # throughput). Su single-node fabric has no NVSwitch egress hop —
    # unchanged.
    2: 15287388,
    4: 13403612,
}


def test_so_hierarchical_regression():
    # Single-node Su baseline: the hierarchical So run must cost MORE than this
    # (the So inter-node phase adds reduce-scatter + allgather across nodes).
    su, _ = run_su_baseline(1 << 20 << 8)
    assert_lossless(su, "su-baseline")
    su_ns = su["simTimeNs"]
    assert su_ns == PARITY_NS["su-baseline"], (
        "su-baseline simTimeNs={} (parity drift; expected {})".format(
            su_ns, PARITY_NS["su-baseline"]))

    for num_nodes, data_size in SO_MATRIX:
        label = "so-hier {}node {}B".format(num_nodes, data_size)
        so, _ = run_so_hierarchical(num_nodes, data_size)
        assert_lossless(so, label)
        assert so["simTimeNs"] == PARITY_NS[num_nodes], (
            "{}: simTimeNs={} (parity drift; expected {})".format(
                label, so["simTimeNs"], PARITY_NS[num_nodes]))
        assert so["simTimeNs"] > su_ns, (
            "{}: simTimeNs={} not > Su baseline {} (So phase must add time)".format(
                label, so["simTimeNs"], su_ns
            )
        )


if __name__ == "__main__":
    ok = True
    su, _ = run_su_baseline(1 << 20 << 8)
    try:
        assert_lossless(su, "su-baseline")
        su_ns = su["simTimeNs"]
        assert su_ns == PARITY_NS["su-baseline"], (
            "su-baseline parity drift: {} != {}".format(su_ns, PARITY_NS["su-baseline"]))
        print("PASS  su-baseline  256MiB 8gpu  simTimeNs={}  (lossless, parity)".format(su_ns))
    except AssertionError as e:
        ok = False
        su_ns = None
        print("FAIL  {}".format(e))
    for num_nodes, data_size in SO_MATRIX:
        label = "so-hier {}node {}B".format(num_nodes, data_size)
        try:
            so, _ = run_so_hierarchical(num_nodes, data_size)
            assert_lossless(so, label)
            assert so["simTimeNs"] == PARITY_NS[num_nodes], (
                "{} parity drift: {} != {}".format(
                    label, so["simTimeNs"], PARITY_NS[num_nodes]))
            if su_ns is not None:
                assert so["simTimeNs"] > su_ns, (
                    "{}: simTimeNs={} not > Su baseline {}".format(
                        label, so["simTimeNs"], su_ns
                    )
                )
            print("PASS  {:14s}  simTimeNs={}  (lossless, >Su)".format(
                label, so["simTimeNs"]))
        except AssertionError as e:
            ok = False
            print("FAIL  {}".format(e))
    sys.exit(0 if ok else 1)
