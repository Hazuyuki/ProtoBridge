#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Surrogate-vs-ns3 anchor for the full-mesh collective (ns-3-calibrated).

Unlike the hierarchical surrogate (an UNCALIBRATED lower bound), the full-mesh
surrogate is a CALIBRATED BACK-FIT to ns-3: ``fullmesh_bw`` is the single scalar
fit to the ns-3 ``FullMeshAllReduce`` / ``FullMeshAllGather`` sweep
(``configs/calibration/fullmesh_ns3_sweep.py``), which itself runs the ns-3
full-mesh injectors on ``--topology=fullmesh`` (every GPU-pair gets a dedicated
``--numLanes=18`` NVLink bundle, no switch). The fit achieved MAPE 0.10% on the
SIMPLE-regime points, so this test asserts a tight symmetric band
(0.95x .. 1.05x) rather than a one-sided lower bound.

The model ``T = startup + steps*(lat + (D/N)/fullmesh_bw)`` (AR steps=2, AG
steps=1) omits per-packet credit flow, VOQ arbitration and the inter-phase
barrier that ns-3 models -- the residual shows up as the ~0.1% gap, not a
systematic offset.

Run:
    python3 -m pytest test/parity/test_surrogate_vs_ns3_fullmesh.py -v
    python3 test/parity/test_surrogate_vs_ns3_fullmesh.py
"""

import glob
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "surrogate", "theory"))
import whitebox_surrogate_v2 as wb  # noqa: E402

# ns-3 fabric params -- must match the calibration sweep so the ns-3 numbers
# here are the same ground truth fullmesh_bw was fit to.
BANDWIDTH_GBPS = 200          # 200 Gbps/link = 25 GB/s NVLink4
LATENCY_NS = 400
NUM_LANES = 18
STARTUP_SIMPLE_NS = 15000     # == surrogate startup_us=15.0
NUM_GPUS = 8

# SIMPLE-regime sizes (chunk D/N >= 2 MB) -- the scope fullmesh_bw was fit on.
SIZES = (1 << 24, 1 << 26, 1 << 28)   # 16 MiB, 64 MiB, 256 MiB
COLLECTIVES = ("allreduce", "allgather")

# Calibrated band (fit MAPE 0.10%): symmetric, not a one-sided lower bound.
RATIO_LO, RATIO_HI = 0.95, 1.05


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


def run_ns3_fullmesh(collective, data_bytes):
    """Run one ns-3 full-mesh point; return simTimeUs (float)."""
    binary = find_sim_binary()
    args = [
        "--numGpus={}".format(NUM_GPUS),
        "--topology=fullmesh",
        "--algorithm=fullmesh",
        "--collective={}".format(collective),
        "--dataSize={}".format(data_bytes),
        "--bandwidth={}".format(BANDWIDTH_GBPS),
        "--latency={}".format(LATENCY_NS),
        "--numLanes={}".format(NUM_LANES),
        "--startupSIMPLE={}".format(STARTUP_SIMPLE_NS),
        "--RngRun=1",
    ]
    if binary:
        cmd = [binary] + args
    else:
        cmd = ["./ns3", "run", "gpu-cluster-sim " + " ".join(args)]
    proc = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True,
                          timeout=300)
    out = proc.stdout + proc.stderr
    status = re.search(r"^status=(\S+)", out, re.MULTILINE)
    sim_us = re.search(r"^simTimeUs=(\S+)", out, re.MULTILINE)
    assert status and status.group(1) == "complete", \
        "ns-3 fullmesh {} {}B did not complete: {}".format(
            collective, data_bytes, out[-400:])
    assert sim_us is not None, "ns-3 missing simTimeUs"
    return float(sim_us.group(1))


def surrogate_fullmesh(collective, data_bytes):
    s = wb.make_h200_fullmesh_theory()
    # topology= is optional for full-mesh: the per-phase bundle rate
    # (fullmesh_bw) is far below the full-mesh bisection, so the bisection
    # cap never binds and topology=None is byte-identical. Omitted for
    # parity with the calibrated unit tests.
    return s.predict(data_bytes, NUM_GPUS, "fullmesh", credits=128, ber=0,
                     collective=collective)


def _check(collective, data_bytes):
    ns3_us = run_ns3_fullmesh(collective, data_bytes)
    surr_us = surrogate_fullmesh(collective, data_bytes)
    ratio = surr_us / ns3_us
    return ns3_us, surr_us, ratio


def test_surrogate_matches_ns3_within_calibrated_band():
    for coll in COLLECTIVES:
        for d in SIZES:
            ns3_us, surr_us, ratio = _check(coll, d)
            assert RATIO_LO <= ratio <= RATIO_HI, (
                "{} {}B: surrogate {:.2f}us vs ns-3 {:.2f}us, ratio {:.3f} "
                "outside calibrated band [{}, {}]".format(
                    coll, d, surr_us, ns3_us, ratio, RATIO_LO, RATIO_HI))


if __name__ == "__main__":
    ok = True
    for coll in COLLECTIVES:
        for d in SIZES:
            try:
                ns3_us, surr_us, ratio = _check(coll, d)
                verdict = "PASS" if RATIO_LO <= ratio <= RATIO_HI else "FAIL"
                if verdict == "FAIL":
                    ok = False
                print("{}  {} {:6d}B  surrogate={:.2f}us  ns3={:.2f}us  "
                      "ratio={:.3f}".format(
                          verdict, coll, d, surr_us, ns3_us, ratio))
            except Exception as e:  # noqa: BLE001
                ok = False
                print("FAIL  {} {:6d}B  {}".format(coll, d, e))
    sys.exit(0 if ok else 1)
