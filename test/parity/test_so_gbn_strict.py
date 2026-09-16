#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Parity + behavior gate for strict So go-back-N receive semantics.

``--soGbnStrict`` (default off) switches the So receive path in
``llrMode=gobackn`` from the legacy bypass-mode dedup (which keeps
out-of-order originals, so blanket retransmits are cheap duplicates) to true
go-back-N semantics: in-order-only acceptance, out-of-order arrivals
discarded with no ACK, and exactly one NAK per gap (IB responder behavior).
Su (NVLink) traffic is never affected — the gate requires a remote source
rank.

Three pinned cases on the SRNIC Fig. 11 sweep scenario (RoCE tor 100G,
16 MiB, 4 KB chunks, seed 1):

* ``test_default_off_legacy_pin`` — without the flag, the lossy GBN run is
  byte-identical to the pre-strict behavior (w=16, plr=1e-2:
  simTimeNs=3185174, retx=918). A regression here means the strict branch
  leaks into the default path.
* ``test_strict_lossless_baseline`` — strict ON at plr=0 matches the legacy
  lossless baseline (2521509 ns, retx=0): with no loss every packet arrives
  in order, so strict acceptance is inert.
* ``test_strict_lossy_pin`` — strict ON at plr=1e-2 (w=64 = the calibrated
  rewind span): completes with permLoss=0, real retransmission work
  (retx=3029 > legacy's cheap 918 at the same loss), and the pinned
  simTimeNs=4276850 — the degradation that brings the GBN arm near the
  measured CX-6 curve (norm 0.580 vs 0.467 at 1%).

Run:
    python3 -m pytest test/parity/test_so_gbn_strict.py -v
    python3 test/parity/test_so_gbn_strict.py
"""

import glob
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CFG = "configs/protocol_configs/p2p-roce-lossy.cfg"


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


def run_gbn(tx_window, plr, strict, timeout=600):
    binary = find_sim_binary()
    args = [
        "--numNodes=2",
        "--gpusPerNode=2",
        "--interNodeFabricType=RoCE",
        "--interNodeTopology=tor",
        "--interNodeBandwidth=100",
        "--interNodeLatency=1000",
        "--llrRetryTimeout=500000",
        "--llrRetryLimit=64",
        "--llrBufferSize=8192",
        "--llrTimeoutRetransmit=1",
        "--soTxWindow={}".format(tx_window),
        "--ber=0",
        "--packetLossRate={}".format(plr),
        "--bulkChunkSize=4096",
        "--protocolConfig=" + CFG,
        "--dataSize=16777216",
        "--RngRun=1",
    ]
    if strict:
        args.append("--soGbnStrict=1")
    if binary:
        cmd = [binary] + args
    else:
        cmd = ["./ns3", "run", "gpu-cluster-sim " + " ".join(args)]
    proc = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True,
                          timeout=timeout)
    out = proc.stdout + proc.stderr
    fields = {}
    for k in ("status", "simTimeNs", "retransmittedPackets", "permanentLosses"):
        m = re.search(r"^{}=(\S+)".format(re.escape(k)), out, re.MULTILINE)
        if m:
            fields[k] = m.group(1) if k == "status" else int(m.group(1))
    return fields, out


def test_default_off_legacy_pin():
    fields, out = run_gbn(tx_window=16, plr="1e-2", strict=False)
    assert fields.get("status") == "complete", out[-500:]
    assert fields.get("simTimeNs") == 3185174, (
        "default-off GBN lossy run changed: simTimeNs={} != 3185174 "
        "(strict branch leaking into the default path?)".format(
            fields.get("simTimeNs")))
    assert fields.get("retransmittedPackets") == 918, fields
    assert fields.get("permanentLosses") == 0, fields


def test_strict_lossless_baseline():
    fields, out = run_gbn(tx_window=64, plr="0", strict=True)
    assert fields.get("status") == "complete", out[-500:]
    assert fields.get("simTimeNs") == 2521509, (
        "strict-on lossless baseline changed: simTimeNs={} != 2521509 "
        "(strict acceptance must be inert with in-order arrivals)".format(
            fields.get("simTimeNs")))
    assert fields.get("retransmittedPackets") == 0, fields


def test_strict_lossy_pin():
    fields, out = run_gbn(tx_window=64, plr="1e-2", strict=True)
    assert fields.get("status") == "complete", out[-500:]
    assert fields.get("permanentLosses") == 0, fields
    assert fields.get("retransmittedPackets") == 3029, (
        "strict-on retransmission volume changed: retx={} != 3029 "
        "(discard/NAK-per-gap semantics regressed?)".format(
            fields.get("retransmittedPackets")))
    assert fields.get("simTimeNs") == 4276850, (
        "strict-on lossy pin changed: simTimeNs={} != 4276850".format(
            fields.get("simTimeNs")))


if __name__ == "__main__":
    failures = 0
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            try:
                fn()
                print("PASS  {}".format(name))
            except AssertionError as e:
                failures += 1
                print("FAIL  {}: {}".format(name, e))
    sys.exit(1 if failures else 0)
