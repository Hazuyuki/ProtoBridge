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
16 MiB, 4 KB chunks, seed 1). These pins are the 1b9235d strict-GBN
calibration values, restored after the bulkChunkSize-cap fix made
--bulkChunkSize=4096 actually bind (the spray-chunk coalescing had been
silently emitting 128 KiB fragments since the 1fb92ba linksPerGpu=18
re-anchor set the endpoint spray chunk to 131072; a 2197d25 re-anchor had
pinned the over-cap behavior -- 2602110/108, 2263080/165, 5156136/478).
The fixed regime is bit-identical to the 1b9235d-era runs that calibrated
the CX-6 comparison in doc/CALIBRATION.md and
examples/gpu-cluster/eth_gbn_loss_sweep.json -- those artifacts are
unchanged, so the pins come home to them:

* ``test_default_off_legacy_pin`` — without the flag, the lossy GBN run is
  byte-identical to the pre-strict behavior at this fabric (w=16, plr=1e-2:
  simTimeNs=3116865, retx=696; spray reordering makes blanket retransmits
  near-free duplicates, so the legacy count is LOWER than strict's). A
  regression here means the strict branch leaks into the default path. The
  pin was re-anchored twice, both without touching the regime: first
  (3185174/918 -> 3116759/696) by the 8701736 duplicate-branch credit
  mirror: a retransmitted So duplicate used to emit a phantom credit packet
  the remote sender never consumed (its sends bypass credits), and
  suppressing it removes those reverse-direction events from this RoCE wire;
  then (3116759 -> 3116865, counts unchanged) by the round-12 LLR
  wire-arm fix: retry timeout deadlines now start at first wire
  transmission instead of at enqueue, so each of the 696 retransmit
  deadlines lands one FEC-encode hold (~1.1us compounded) later. The
  strict-ON pins below never had the phantom (strict dedup ACKs instead of
  re-crediting) and their lossy timeline already tracked wire egress.
* ``test_strict_lossless_baseline`` — strict ON at plr=0 (retx=0): with
  the 4 KB cap enforced the TX window (w=64 chunks) bounds in-flight below
  the reorder depth, so arrivals are in-order and strict acceptance is
  inert -- exactly the "no semantic leakage at zero loss" baseline. The
  pin is the deterministic RngRun=1 value; a change means the acceptance
  semantics shifted.
* ``test_strict_lossy_pin`` — strict ON at plr=1e-2 (w=64 = the calibrated
  rewind span): completes with permLoss=0, real retransmission work
  (retx=3029 -- in-order-only acceptance discards reordered arrivals
  rather than keeping them as cheap duplicates, and each of the ~43
  real 1% drops rewinds the whole 64-chunk window), and the pinned
  simTimeNs=4276850 -- the degradation that brings the GBN arm to the
  measured CX-6 curve (normalized 0.580 vs 0.467 at 1e-2).

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
    assert fields.get("simTimeNs") == 3116865, (
        "default-off GBN lossy run changed: simTimeNs={} != 3116865 "
        "(strict branch leaking into the default path, or the fabric "
        "convention moved under the pin?)".format(fields.get("simTimeNs")))
    assert fields.get("retransmittedPackets") == 696, fields
    assert fields.get("permanentLosses") == 0, fields


def test_strict_lossless_baseline():
    fields, out = run_gbn(tx_window=64, plr="0", strict=True)
    assert fields.get("status") == "complete", out[-500:]
    assert fields.get("simTimeNs") == 2521509, (
        "strict-on lossless baseline changed: simTimeNs={} != 2521509 "
        "(strict acceptance semantics shifted)".format(fields.get("simTimeNs")))
    # The 4 KiB cap-enforced regime keeps w=64 in-flight chunks in-order on
    # the wire, so strict acceptance is inert at zero loss (retx=0).
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
