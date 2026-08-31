#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Automated gate for the plain-Ethernet lossy NACK -> LLR retransmit loop.

The unit test ``EthernetSwitchDropTest`` (gpu-cluster-test.cc) asserts only the
DROP DECISION (OnPacketEnqueue return false/true); it explicitly does NOT drive
the NACK egress, the ``SetInterNodeSwitchLlrEnabled`` switch-side wiring, or the
``OnRetryTimeout`` / ``--llrRetryLimit`` permanent-loss fallback -- those were
covered only by the manual ``p2p-eth-incast.cfg`` smoke. This test converts that
smoke into a machine-checked gate by running the incast scenario through ns-3
and asserting the full event-driven recovery loop.

Two cases, mirroring the two ``--llrRetryLimit`` regimes documented in the
``p2p-eth-incast.cfg`` header:

* ``test_ethernet_incast_recovers`` -- ``--llrRetryLimit=64`` (high): under the
  2-flow incast the dest-port VOQ overflows (``switchVoqDrops > 0``), the
  switch emits NACKs, the sender retransmits (``retransmittedPackets > 0``),
  and every dropped packet is eventually recovered (``permanentLosses == 0``)
  so the run COMPLETES. This is the end-to-end proof that EmitNackBack +
  SetInterNodeSwitchLlrEnabled + ProcessNackPacket are wired and functional --
  a regression in any of them stalls at the 3600s ceiling with
  retransmittedPackets == 0.

* ``test_ethernet_permanent_loss_fallback`` -- ``--llrRetryLimit=8`` (low, the
  documented ``p2p-eth-incast.cfg`` contrast): retries exhaust and the
  permanent-loss fallback fires (``permanentLosses > 0``) yet the run still
  COMPLETES (the graceful degraded completion, NOT a stall). This is the
  faithful severe-congestion lossy-Ethernet degradation (the thing RoCE's PFC
  exists to prevent) -- a safety-critical path with no other automated coverage.
  (``--llrRetryLimit=1`` is too aggressive -- it stalls at the 3600s ceiling
  rather than degrading gracefully -- so 8 is the documented contrast point.)

Run:
    python3 -m pytest test/parity/test_ethernet_lossy_recovery.py -v
    python3 test/parity/test_ethernet_lossy_recovery.py
"""

import glob
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
INCAST_CFG = "configs/protocol_configs/p2p-eth-incast.cfg"


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


def run_incast(llr_retry_limit, timeout=600):
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
        "--llrRetryLimit={}".format(llr_retry_limit),
        "--ber=0",
        "--packetLossRate=0",
        "--protocolConfig=" + INCAST_CFG,
        "--dataSize=262144",
        "--RngRun=1",
    ]
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

    for k in ("status", "simTimeUs", "switchVoqDrops",
              "retransmittedPackets", "permanentLosses", "retryBufferPeak"):
        grab(k, (int if k != "status" and k != "simTimeUs" else
                 (float if k == "simTimeUs" else str)))
    return fields, out


def test_ethernet_incast_recovers():
    fields, out = run_incast(64)
    assert fields.get("status") == "complete", (
        "incast rlim=64 did not complete (retransmit loop broken?):\n"
        + out[-500:])
    assert fields.get("switchVoqDrops", 0) > 0, (
        "incast rlim=64: switchVoqDrops={} (lossy tail-drop path did not "
        "fire)".format(fields.get("switchVoqDrops")))
    assert fields.get("retransmittedPackets", 0) > 0, (
        "incast rlim=64: retransmittedPackets={} (NACK->retransmit recovery "
        "did not fire -- EmitNackBack or SetInterNodeSwitchLlrEnabled "
        "regressed?)".format(fields.get("retransmittedPackets")))
    assert fields.get("permanentLosses", -1) == 0, (
        "incast rlim=64: permanentLosses={} (every dropped packet should be "
        "recovered at retry limit 64)".format(fields.get("permanentLosses")))


def test_ethernet_permanent_loss_fallback():
    # rlim=8 is the documented p2p-eth-incast.cfg contrast: retries exhaust on
    # some packets (permanentLosses > 0) yet the run still COMPLETES -- the
    # graceful degraded completion, not a stall. (rlim=1 is too aggressive and
    # stalls at the 3600s Simulator::Stop ceiling, so 8 is the contrast point.)
    fields, out = run_incast(8)
    assert fields.get("status") == "complete", (
        "incast rlim=8 did not complete (permanent-loss fallback should still "
        "complete the run gracefully):\n" + out[-500:])
    assert fields.get("permanentLosses", 0) > 0, (
        "incast rlim=8: permanentLosses={} (the permanent-loss fallback should "
        "fire once retries exhaust -- the lossy-Ethernet degradation)"
        .format(fields.get("permanentLosses")))


if __name__ == "__main__":
    ok = True
    for name, fn in (("incast_recovers", test_ethernet_incast_recovers),
                     ("permanent_loss_fallback", test_ethernet_permanent_loss_fallback)):
        try:
            fn()
            print("PASS  {}".format(name))
        except AssertionError as e:
            ok = False
            print("FAIL  {}  {}".format(name, e))
    sys.exit(0 if ok else 1)
