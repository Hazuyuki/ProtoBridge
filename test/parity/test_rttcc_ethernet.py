#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Automated gate for RTTCC (RTT-probe-based congestion control) on Ethernet.

RTTCC is the delay-based congestion-control option for the plain lossy
Ethernet So fabric, where DCQCN cannot operate (EthernetProtocolModel locks
WireEfficiency=1.0 and the fabric has no ECN/CNP loop at all). The sender
emits RTT_PROBE control packets alongside data (at most one per ProbeInterval
per flow); the receiver reflects each as an RTT_RESPONSE; the per-endpoint
``RttccRateMachine`` turns echoes into RTT samples and adjusts the flow rate
Swift-style (over-target -> multiplicative decrease, below-target -> additive
increase), enforced as a true per-flow pacer at the SendPacketOnDevice
chokepoint.

Two cases on the ``p2p-eth-incast.cfg`` scenario (2 senders -> rank 3 through
a voqDropThreshold=2 ToR switch -- the congestion RTTCC exists to sense):

* ``test_rttcc_default_off_parity`` -- without ``--rttcc`` the run must be
  BYTE-IDENTICAL to the pre-RTTCC baseline (simTimeNs=68664, drops=87,
  retx=285, permLoss=0): no machine is mounted, no probe is emitted, the new
  code is invisible. This is the parity foundation for every existing
  calibration.

* ``test_rttcc_on_incast`` -- with ``--rttcc=1`` the full loop must light up:
  probes sent, RTT samples collected, and at least one flow throttled below
  the 400G line rate (rttccMinRateGbps < 400) while the run still COMPLETES.
  The pinned numbers (simTimeNs=73700, drops=93, retx=279) are the observed
  deterministic RngRun=1 values; on this SHORT 256KB burst RTTCC reacts too
  late to cut drops (probe RTT feedback lags the initial burst), whereas the
  longer 4MB incast shows the expected reduction (drops 16388->16317, retx
  25151->24911, permanentLosses 280->272 -- documented, not asserted, to keep
  the gate fast).

Run:
    python3 -m pytest test/parity/test_rttcc_ethernet.py -v
    python3 test/parity/test_rttcc_ethernet.py
"""

import glob
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
INCAST_CFG = "configs/protocol_configs/p2p-eth-incast.cfg"

# Pre-RTTCC incast baseline (debug binary, RngRun=1) -- must stay pinned.
BASELINE = {
    "simTimeNs": 68664,
    "switchVoqDrops": 87,
    "retransmittedPackets": 285,
    "permanentLosses": 0,
}

# Observed deterministic values with --rttcc=1 (RngRun=1).
RTTCC_ON = {
    "simTimeNs": 73700,
    "switchVoqDrops": 93,
    "retransmittedPackets": 279,
    "permanentLosses": 0,
}


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


def run_incast(rttcc, timeout=600):
    """Run the 2-flow ETH incast; return parsed metric fields."""
    binary = find_sim_binary()
    args = [
        "--numNodes=2",
        "--gpusPerNode=2",
        "--interNodeFabricType=ETH",
        "--interNodeTopology=tor",
        "--interNodeBandwidth=400",
        "--interNodeLatency=100",
        "--voqDropThreshold=2",
        "--bulkChunkSize=16384",
        "--llrMode=sack",
        "--llrRetryTimeout=5000",
        "--llrRetryLimit=64",
        "--ber=0",
        "--packetLossRate=0",
        "--protocolConfig=" + INCAST_CFG,
        "--dataSize=262144",
        "--RngRun=1",
    ]
    if rttcc:
        args.append("--rttcc=1")
    if binary:
        cmd = [binary] + args
    else:
        cmd = ["./ns3", "run", "gpu-cluster-sim " + " ".join(args)]
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

    for k in ("status", "rttccEnabled"):
        grab(k, str)
    for k in ("simTimeNs", "switchVoqDrops", "retransmittedPackets",
              "permanentLosses", "rttccProbesSent", "rttccRttSamples",
              "rttccMinRttNs", "rttccMaxRttNs"):
        grab(k, int)
    grab("rttccMinRateGbps", float)
    return fields, out


def test_rttcc_default_off_parity():
    fields, out = run_incast(rttcc=False)
    assert fields.get("status") == "complete", (
        "default-off incast did not complete:\n" + out[-500:])
    assert fields.get("rttccEnabled") == "false", (
        "rttccEnabled must report false by default")
    for k, want in BASELINE.items():
        assert fields.get(k) == want, (
            "default-off incast {}={} != pinned pre-RTTCC baseline {} "
            "(--rttcc off must be byte-identical)".format(
                k, fields.get(k), want))


def test_rttcc_on_incast():
    fields, out = run_incast(rttcc=True)
    assert fields.get("status") == "complete", (
        "--rttcc=1 incast did not complete:\n" + out[-500:])
    assert fields.get("rttccEnabled") == "true", "rttccEnabled must report true"
    # The control loop must actually run: probes out, echoes back, throttle on.
    assert fields.get("rttccProbesSent", 0) > 0, (
        "rttccProbesSent={} (MaybeStartProbe never fired at the "
        "chokepoint?)".format(fields.get("rttccProbesSent")))
    assert fields.get("rttccRttSamples", 0) > 0, (
        "rttccRttSamples={} (probe/response reflection loop broken -- "
        "RTT_PROBE dispatch or SendRttResponsePacket regressed?)".format(
            fields.get("rttccRttSamples")))
    assert fields.get("rttccMinRateGbps", 400.0) < 400.0, (
        "rttccMinRateGbps={} (no flow throttled below line rate under a "
        "voqDropThreshold=2 incast -- OnProbeResponse MD regressed?)".format(
            fields.get("rttccMinRateGbps")))
    assert 0 < fields.get("rttccMinRttNs", 0) <= fields.get("rttccMaxRttNs", 0), (
        "RTT stats malformed: min={} max={}".format(
            fields.get("rttccMinRttNs"), fields.get("rttccMaxRttNs")))
    for k, want in RTTCC_ON.items():
        assert fields.get(k) == want, (
            "--rttcc=1 incast {}={} != pinned deterministic value {}".format(
                k, fields.get(k), want))


if __name__ == "__main__":
    ok = True
    for name, fn in (("default_off_parity", test_rttcc_default_off_parity),
                     ("rttcc_on_incast", test_rttcc_on_incast)):
        try:
            fn()
            print("PASS {}".format(name))
        except AssertionError as e:
            ok = False
            print("FAIL {}: {}".format(name, e))
    sys.exit(0 if ok else 1)
