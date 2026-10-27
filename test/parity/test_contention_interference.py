#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Automated gate for multi-operation temporal contention modeling.

The ``--bgP2p*`` CLI (commit 93d86a2) injects a self-driven ``P2pTraffic`` flow
at ``--bgP2pStartTimeNs`` that shares the fabric with the primary collective.
Two DIFFERENT mechanisms slow the collective, and this test checks both, since
each can regress independently:

1. **Link sharing (FIFO queueing on the wire).** The background packets and
   the collective's packets serialize on the same NetDevice / So switch VOQ.
   This is what dominates the smoke numbers below and it is what the
   ``interference`` assertions check: the bg run must still complete, must
   inject bytes, and must be strictly slower than the baseline.
2. **WFQ contention penalty (``ContentionModel``).** An extra send-side delay
   applied only when more than one traffic class is active on the endpoint
   (``numActive>1`` in ``SendPacketOnDevice``). Setting all three class
   weights to 1.0 makes ``ComputeServiceTime`` return exactly 0 while the
   model stays mounted -- i.e. it neuters the penalty without touching the
   link sharing. The ``wfq`` assertions run the bg case a second time with
   weights=1.0 and require the result to DIFFER (either direction: WFQ
   reshuffles who waits, it does not add latency monotonically). A regression
   that skips the penalty branch, mis-counts ``numActive``, or fails to
   classify P2P collapses ``weights=default == weights=1.0``.

Pinned values (deterministic, ``--RngRun=1``; re-pin on an intentional model
change, the way ``test_so_hierarchical_regression.py`` pins its parity triple):

* multi-node, 2-node RoCE hierarchical allreduce 16 MiB:
  baseline 1248928; cross-node bg (rank 0->4) 2776201 (+122%) -- the WFQ term
  is OFF the critical path here (the bg flow, paced at the 800G intra-node
  rate onto a 400G link, is limited by NIC link sharing), so the WFQ
  discriminator uses an intra-node bg (rank 0->1): default 1330831 vs
  weights=1.0 1315771. NOTE: the multi-node bg pins were re-anchored by the
  HierarchicalAllReduce PERMANENT_LOSS/P2P-progress fix (only FabricPacketType
  ::DATA advances collective phases). The pre-fix values (1353663/1311031)
  were corrupted by phantom progress: the bg flow's P2P packets incremented
  the same phase counters the collective uses, so the run "completed" early
  on missing collective bytes. The fixed runs wait for real data, so bg
  interference now shows its true (much larger) cost.
* single-node, Su ring allreduce 16 MiB: baseline 319658; intra-node bg
  (rank 0->2) default 450982 vs weights=1.0 405462 (+27% is link sharing; the
  WFQ penalty on the P2P class is a -10% correction). weights=1.0 was
  re-pinned 468378 -> 405462 after the codex-round-15/finding-3 ring
  transaction-graph fix (sends now gated on input receipt, matching the wire
  path exactly); the old 468378 captured the pre-fix graph where sends fired
  at wire speed without waiting for their inputs.

Note on the single- vs multi-node modeling difference (a design choice, not a
bug): the single-node ``GpuClusterTopologyHelper`` shares ONE
``ContentionModel`` clone across all its endpoints (backlog aggregates
node-wide), whereas the multi-node path mounts an independent per-endpoint
instance (backlog local to that endpoint's injection side; bandwidth is the
intra-node rate in both). This test checks that both mechanisms take effect
in BOTH regimes, not that the two magnitudes match.

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


# Multi-node: 2-node RoCE hierarchical allreduce, 16 MiB.
MULTI_BASE = [
    "--numNodes=2", "--gpusPerNode=4",
    "--collective=allreduce", "--algorithm=hierarchical",
    "--dataSize=16777216",
    "--interNodeFabricType=RoCE", "--interNodeTopology=host",
    "--interNodeBandwidth=400", "--interNodeLatency=100",
    "--RngRun=1",
]
BG_COMMON = ["--bgP2pEnabled=1", "--bgP2pSrc=0",
             "--bgP2pSizeBytes=1048576", "--bgP2pStartTimeNs=5000"]
WEIGHTS_ONE = ["--contentionCollectiveWeight=1", "--contentionMemoryWeight=1",
               "--contentionP2pWeight=1"]
MULTI_BG_CROSS = MULTI_BASE + BG_COMMON + ["--bgP2pDst=4"]   # link-sharing smoke
MULTI_BG_INTRA = MULTI_BASE + BG_COMMON + ["--bgP2pDst=1"]   # WFQ discriminator

# Single-node: Su ring allreduce, 16 MiB, intra-node bg P2P.
SINGLE_BASE = [
    "--numGpus=4",
    "--collective=allreduce", "--algorithm=ring",
    "--dataSize=16777216",
    "--RngRun=1",
]
SINGLE_BG = SINGLE_BASE + BG_COMMON + ["--bgP2pDst=2"]

PINNED = {
    # Multi-node pins re-pinned (cut-through accounting fix: serialization-only
    # egress busy-until; the pipeline overlaps the serializer). Single-node
    # values unchanged -- the Su fabric has no switch egress hop.
    "multi-base": 1246928, "multi-bg-cross": 2734229,
    "multi-bg-intra": 1311036, "multi-bg-intra-w1": 1295976,
    # single-bg-w1 re-pinned 468378 -> 405462 (codex round 15, finding 3):
    # the old pin captured the pre-fix transaction graph, which chained each
    # rank's next send on its OWN outgoing delivery instead of input receipt.
    # The wire path (--useTransactionModel=0) always gave 405462; the fixed
    # transaction path now matches it bit-for-bit (per-step traces identical).
    "single-base": 319658, "single-bg": 450982, "single-bg-w1": 405462,
}


def _assert_interference(base, base_out, bg, bg_out, regime):
    assert base.get("status") == "complete", (
        "{} baseline did not complete:\n{}".format(regime, base_out[-500:]))
    assert bg.get("status") == "complete", (
        "{} interference run did not complete (bg P2P may have deadlocked the "
        "collective):\n{}".format(regime, bg_out[-500:]))
    assert bg.get("bgP2pEnabled") == "true", (
        "{}: bgP2pEnabled not reflected in result block".format(regime))
    # bgP2pBytesSent counts injection (SendP2p calls), not delivery; >0 only
    # proves the P2pTraffic was constructed and Start() fired.
    assert bg.get("bgP2pBytesSent", 0) > 0, (
        "{}: bgP2pBytesSent={} (the background P2pTraffic never started)"
        .format(regime, bg.get("bgP2pBytesSent")))
    assert bg.get("bgP2pPacketsSent", 0) > 0, (
        "{}: bgP2pPacketsSent={}".format(regime, bg.get("bgP2pPacketsSent")))
    base_ns = base.get("simTimeNs")
    bg_ns = bg.get("simTimeNs")
    assert base_ns is not None and bg_ns is not None, (
        "{}: simTimeNs missing (base={} bg={})".format(regime, base_ns, bg_ns))
    assert bg_ns > base_ns, (
        "{}: interference simTimeNs ({}) not strictly greater than baseline "
        "({}) -- the background flow did not share the wire with the "
        "collective (P2pTraffic not reaching SendPacketOnDevice / the "
        "NetDevice?).".format(regime, bg_ns, base_ns))


def _assert_pinned(fields, key, regime):
    got = fields.get("simTimeNs")
    assert got == PINNED[key], (
        "{}: {} simTimeNs={} != pinned {} (re-pin only on an intentional "
        "model change)".format(regime, key, got, PINNED[key]))


def _assert_wfq_discriminates(bg, bg_w1, regime):
    a = bg.get("simTimeNs")
    b = bg_w1.get("simTimeNs")
    assert a is not None and b is not None and a != b, (
        "{}: bg simTimeNs with default WFQ weights ({}) == with all weights=1.0 "
        "({}) -- the ContentionModel penalty has no effect on the critical "
        "path (numActive>1 branch skipped, P2P class not classified, or "
        "ContentionModel not mounted on this endpoint).".format(regime, a, b))


def test_contention_interference_multi_node():
    base, base_out = run_sim(MULTI_BASE)
    bg, bg_out = run_sim(MULTI_BG_CROSS)
    _assert_interference(base, base_out, bg, bg_out, "multi-node")
    _assert_pinned(base, "multi-base", "multi-node")
    _assert_pinned(bg, "multi-bg-cross", "multi-node")


def test_contention_wfq_multi_node():
    bg, _ = run_sim(MULTI_BG_INTRA)
    bg_w1, _ = run_sim(MULTI_BG_INTRA + WEIGHTS_ONE)
    assert bg.get("status") == "complete" and bg_w1.get("status") == "complete"
    _assert_wfq_discriminates(bg, bg_w1, "multi-node")
    _assert_pinned(bg, "multi-bg-intra", "multi-node")
    _assert_pinned(bg_w1, "multi-bg-intra-w1", "multi-node")


def test_contention_interference_single_node():
    base, base_out = run_sim(SINGLE_BASE)
    bg, bg_out = run_sim(SINGLE_BG)
    _assert_interference(base, base_out, bg, bg_out, "single-node")
    _assert_pinned(base, "single-base", "single-node")
    _assert_pinned(bg, "single-bg", "single-node")


def test_contention_wfq_single_node():
    bg, _ = run_sim(SINGLE_BG)
    bg_w1, _ = run_sim(SINGLE_BG + WEIGHTS_ONE)
    assert bg.get("status") == "complete" and bg_w1.get("status") == "complete"
    _assert_wfq_discriminates(bg, bg_w1, "single-node")
    _assert_pinned(bg_w1, "single-bg-w1", "single-node")


if __name__ == "__main__":
    ok = True
    for name, fn in (("multi_node", test_contention_interference_multi_node),
                     ("wfq_multi_node", test_contention_wfq_multi_node),
                     ("single_node", test_contention_interference_single_node),
                     ("wfq_single_node", test_contention_wfq_single_node)):
        try:
            fn()
            print("PASS  contention_interference  {}".format(name))
        except AssertionError as e:
            ok = False
            print("FAIL  contention_interference  {}  {}".format(name, e))
    sys.exit(0 if ok else 1)
