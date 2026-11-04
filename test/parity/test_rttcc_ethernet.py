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
  BYTE-IDENTICAL to the deterministic off-baseline (simTimeNs=15480,
  drops=27, retx=27, permLoss=0; pinned at the linksPerGpu=18 fabric
  convention and 192 KiB data -- see the dataSize note at the pins): no
  machine is mounted, no probe is emitted, the new code is invisible. This
  is the parity foundation for every existing calibration.

* ``test_rttcc_on_incast`` -- with ``--rttcc=1 --rttccTargetRttNs=1000`` the
  full loop must light up: probes sent, RTT samples collected, and at least
  one flow throttled below the 400G line rate (rttccMinRateGbps=354.9) while
  the run still COMPLETES. The non-default 1 us target is REQUIRED to engage
  the loop: selective SACK retransmit recovers each drop with one retransmit
  and (post cut-through accounting fix) the incast's max RTT is only
  1164 ns, below the 10 us default target. The gate's purpose is that the
  loop demonstrably engages, not that it wins at
  this size.

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

# Deterministic off-baseline (debug binary, RngRun=1) -- must stay pinned.
# Pinned at the 1fb92ba linksPerGpu=18 fabric convention AND 192 KiB data:
# the cfg's profile builds the 18-lane spray fabric, whose wider intra-node
# trunk absorbs a 256 KiB burst before the dest-port VOQ fills (drops 87 -> 1,
# too little congestion for RTTCC to sense). 192 KiB (12 x 16 KiB bulk chunks
# per flow) restores the incast pressure RTTCC exists to react to WITHOUT the
# runaway drop-storm a 4 MiB burst produces at the (now-enforced) 16 KiB bulk
# cap: 256 fragments/flow at voqDropThreshold=2 burn every retry budget
# (permLoss>0 even at rlim=64), which is a lossy-regime stress, not a
# congestion-control scenario.
# Re-pinned when the cfg gained its [stack] llrMode=sack override: the profile
# pins llrMode=gobackn and profile values override CLI flags, so before the
# override the tests' --llrMode=sack silently ran GBN (blanket retransmits:
# 149 drops / 314 retx). True SACK retransmits selectively (27/27) and halves
# completion (106.9 -> 15.5 us).
# Re-pinned (cut-through accounting fix: serialization-only egress
# busy-until; the 200-ns pipeline overlaps the serializer). The 192-KiB
# incast now drains ~30% faster, the VOQ tail-drop pattern is smaller
# (27 -> 11), and the max RTT fell 5849 -> 1164 ns.
BASELINE = {
    "simTimeNs": 10880,
    "switchVoqDrops": 11,
    "retransmittedPackets": 11,
    "permanentLosses": 0,
}

# Observed deterministic values with --rttcc=1 --rttccTargetRttNs=1000
# (RngRun=1; post cut-through accounting fix). The non-default 1us target is
# REQUIRED to engage the loop: selective SACK retransmit recovers each VOQ
# drop with one retransmit, and the incast's max RTT (1164 ns post-fix;
# 5849 ns pre-fix) never exceeds the 10us default target.
# Re-pinned with BASELINE (cut-through accounting fix). Target moved
# 3000 -> 1000 ns: the fix dropped the incast's max RTT to 1164 ns, so the
# old 3-us target sat above the observed RTT and the controller never
# throttled. 1000 ns engages the loop (rate 400 -> 354.9).
RTTCC_ON = {
    "simTimeNs": 10881,
    "switchVoqDrops": 11,
    "retransmittedPackets": 11,
    "permanentLosses": 0,
    "rttccMinRateGbps": 354.9,
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
        "--dataSize=196608",
        "--RngRun=1",
    ]
    if rttcc:
        args.append("--rttcc=1")
        # Below the incast's observed max RTT (5849ns): the 10us default never
        # throttles under SACK (see RTTCC_ON comment).
        args.append("--rttccTargetRttNs=1000")
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
