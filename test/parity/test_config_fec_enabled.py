#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Config mode honors the CLI ``--fecEnabled=0`` override (codex round 29, F4).

The config-primary path reassigns the endpoint FEC model to the profile
bundle's model (``gpu-cluster-sim.cc``: ``if (pbundle.fec) { fecModel =
pbundle.fec; }``). The profile key set has no ``fecEnabled`` (RESERVED only
covers fecN/fecK/fecT/fecEncodeLatencyNs/fecDecodeLatencyNs) and
``FecModel`` defaults ``m_enabled=true`` — so the unconditional swap silently
re-enabled FEC after the CLI's ``--fecEnabled=0`` had disabled the
CLI-constructed model. Config mode then ran with FEC encode/decode latency
and BER protection the user had explicitly turned off, and the counters were
indistinguishable from ``--fecEnabled=1``.

The fix applies the CLI flag to the bundle's model after the swap, so the
explicit CLI value always wins over the model default. This test gates that:
a config-mode run with ``--fecEnabled=0`` must show ZERO FEC activity
(``fecNoError=0``) while the same run with ``--fecEnabled=1`` counts decodes,
and the two differ in sim time (the disabled model adds no coding latency).

Run:
    python3 -m pytest test/parity/test_config_fec_enabled.py -v
    python3 test/parity/test_config_fec_enabled.py
"""

import glob
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CFG = os.path.join(ROOT, "configs", "protocol_configs", "p2p-single.cfg")


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


def run_sim(args, timeout=600):
    binary = find_sim_binary()
    if binary:
        cmd = [binary] + args
        cwd = None
    else:
        cmd = ["./ns3", "run", "gpu-cluster-sim " + " ".join(args)]
        cwd = ROOT
    proc = subprocess.run(cmd, cwd=cwd or ROOT, capture_output=True,
                          text=True, timeout=timeout)
    out = proc.stdout + proc.stderr
    fec = re.search(r"^fecNoError=(\d+)", out, re.MULTILINE)
    status = re.search(r"^status=(\S+)", out, re.MULTILINE)
    sim_ns = re.search(r"^simTimeNs=(\d+)", out, re.MULTILINE)
    return (int(fec.group(1)) if fec else None,
            status.group(1) if status else None,
            int(sim_ns.group(1)) if sim_ns else None, out)


def test_config_fec_enabled_override():
    common = ["--protocolConfig=" + CFG, "--numGpus=2",
              "--dataSize=65536", "--topology=switched"]
    fec_on, st_on, ns_on, out_on = run_sim(common + ["--fecEnabled=1"])
    assert st_on == "complete", "enabled run failed:\n" + out_on[-400:]
    assert (fec_on or 0) > 0, (
        "the enabled run must exercise the FEC decode path (fecNoError>0); "
        "if this fires, the probe scenario stopped exercising FEC")

    fec_off, st_off, ns_off, out_off = run_sim(common + ["--fecEnabled=0"])
    assert st_off == "complete", "disabled run failed:\n" + out_off[-400:]
    assert fec_off == 0, (
        "config mode with --fecEnabled=0 must show zero FEC activity "
        "(fecNoError={}; pre-fix: the profile model's m_enabled=true default "
        "silently re-enabled FEC after the CLI disabled it)".format(fec_off))
    assert ns_off < ns_on, (
        "the disabled run must not pay FEC coding latency "
        "(disabled simTimeNs={} >= enabled {})".format(ns_off, ns_on))


if __name__ == "__main__":
    test_config_fec_enabled_override()
    print("test_config_fec_enabled: PASS")
