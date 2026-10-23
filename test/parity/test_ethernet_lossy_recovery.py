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

* ``test_ethernet_permanent_loss_fallback`` -- ``--llrRetryLimit=3`` (below the
  recovery threshold): retries exhaust and the permanent-loss fallback fires
  (``permanentLosses > 0``) with the loss notified end-to-end -- the receiver
  advances past the gap and the run reports the degradation (permanentLosses=1,
  incomplete at the 3600s ceiling: the deliberate stall-not-corrupt policy,
  protocol-config-runner.cc's PERMANENT_LOSS guard). This is the faithful
  severe-congestion lossy-Ethernet degradation (the thing RoCE's PFC exists to
  prevent) -- a safety-critical path with no other automated coverage.

 dataSize note: pinned at 192 KiB since the bulkChunkSize-cap fix (12 x 16 KiB
 bulk chunks per flow). The 1fb92ba linksPerGpu=18 re-anchor originally
 moved the pin from 256 KiB up to 4 MiB -- but that 4 MiB calibration ran
 when the 16 KiB --bulkChunkSize cap was silently ignored by the spray-chunk
 coalescing (32 x 128 KiB fragments per flow, drops=86). With the cap
 actually enforced, a 4 MiB burst is 256 fragments/flow against
 voqDropThreshold=2: ~18k VOQ drops burn EVERY retry budget (permLoss=354
 even at rlim=64, incomplete at rlim=8) -- a lossy-regime stress test, not
 the NACK->retransmit recovery demo this file gates. 192 KiB restores the
 intended regime at the true chunk size.

 llrMode note: re-anchored when the cfg gained its ``[stack] llrMode = sack``
 override. The referenced h200-ll128 profile pins ``llrMode = gobackn`` and
 profile values override CLI flags, so the tests' ``--llrMode=sack`` silently
 ran GBN before the override (blanket retransmits: 149 drops / 314 retx /
 permLoss=9 at rlim=8 with a "graceful" completion that was really GBN's
 blanket-retransmit byte accounting double-counting retransmitted fragments).
 True SACK retransmits selectively: 27 drops / 27 retx, permLoss=0 for every
 rlim >= 4 at 192 KiB, and a permanent loss always stalls by design (the
 executor counts delivered effective bytes; PERMANENT_LOSS events are ignored
 rather than credited -- stall-not-corrupt). The contrast point is therefore
 rlim=3 (permLoss=1, incomplete), not rlim=8.

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
        "--dataSize=196608",
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
    # rlim=3 is the contrast point under true SACK (see the module docstring):
    # below the recovery threshold (rlim>=4 recovers everything at 192 KiB),
    # retries exhaust, the permanent-loss fallback fires end-to-end
    # (NotifyPermanentLoss -> PERMANENT_LOSS packet -> receiver MarkPermanentGap)
    # and the run degrades to the deliberate stall-not-corrupt incomplete
    # state (the executor's PERMANENT_LOSS guard).
    fields, out = run_incast(3)
    assert fields.get("permanentLosses", 0) > 0, (
        "incast rlim=3: permanentLosses={} (the permanent-loss fallback should "
        "fire once retries exhaust -- the lossy-Ethernet degradation)"
        .format(fields.get("permanentLosses")))
    assert fields.get("status") == "incomplete", (
        "incast rlim=3: status={} (a permanent loss must stall the run, not "
        "complete with missing bytes -- stall-not-corrupt)".format(
            fields.get("status")))


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
