#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Bit-identical parity test: a `.cfg` op vs the inline CLI path.

A `.cfg` whose ``[op]`` declares ``collective = <c>`` + ``algorithm = <a>``
(topology optional) delegates to a calibrated inline injector: the simulator
sources the ``[stack]`` profile's PEX + fabric-hardware values into the same
local CLI variables the inline path consumes, then runs the injector verbatim.
Because every sourced profile key maps 1:1 to an inline CLI flag, the two paths
must be bit-identical (same code, same values, same RNG seed) -- i.e. the
simTimeNs of ``--protocolConfig=...cfg`` must equal the simTimeNs of the inline
command built from the profile's values.

This is the gate that makes the config-file form a *primary* input: a user's
``.cfg`` reproduces the calibrated inline path exactly, not approximately.

Run:
    python3 -m pytest test/parity/test_config_vs_inline_parity.py -v
    # or directly:
    python3 test/parity/test_config_vs_inline_parity.py

The test parses ``configs/protocol_profiles/h200-ll128.profile``, emits the
equivalent inline CLI flags, runs ``gpu-cluster-sim`` twice per case (config
vs inline) for a matrix of {ring, tree, nvls} x {size} x {numGpus}, and
asserts simTimeNs is exactly equal.
"""

import glob
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PROFILE = os.path.join(ROOT, "configs", "protocol_profiles", "h200-ll128.profile")

# Profile key -> inline CLI flag. This MUST mirror the sourcing block in
# scratch/gpu-cluster-sim.cc (the keys it reads via prof.Get / apply* and the
# cmd.AddValue flag names). If the C++ sources a key the test does not, or vice
# versa, the two paths diverge and this test fails -- which is the point.
PROFILE_TO_FLAG = {
    # fabric hardware
    "bandwidthGbps": "--bandwidth",
    "latencyNs": "--latency",
    "numLanes": "--numLanes",
    "linksPerGpu": "--linksPerGpu",
    "sprayChunkSize": "--sprayChunkSize",
    "switchVoqDepth": "--switchVoqDepth",
    "switchArbIntervalNs": "--switchArbInterval",
    # protocol model + startup + thresholds
    "protocolModel": "--protocolModel",
    "forceProtocol": "--forceProtocol",
    "StartupDelayLL": "--startupLL",
    "StartupDelayLL128": "--startupLL128",
    "StartupDelaySIMPLE": "--startupSIMPLE",
    "StartupNVLS": "--startupNVLS",
    "LlThreshold": "--llThreshold",
    "Ll128Threshold": "--ll128Threshold",
    # FEC
    "fecN": "--fecN",
    "fecK": "--fecK",
    "fecT": "--fecT",
    "fecEncodeLatencyNs": "--fecEncodeLatency",
    "fecDecodeLatencyNs": "--fecDecodeLatency",
    # link BER
    "berIntraNodeElectrical": "--berIntraNodeElectrical",
    "berIntraRackElectrical": "--berIntraRackElectrical",
    "berInterRackOptical": "--berInterRackOptical",
    # link-level retry
    "llrEnabled": "--llrEnabled",
    "llrMode": "--llrMode",
}
# vcCredits / vcCount / flowControl are PEX-bundle values consumed only by the
# stencil runner's ApplyBundle; a collective/algorithm op keeps the inline 1-VC
# fabric model (matching the calibrated inline path), so they are intentionally
# NOT sourced and NOT in this map.

# op (algorithm) -> (cfg file, topology, collective, algorithm) for the inline
# baseline. Mirrors the .cfg's [op] collective=/algorithm=/topology= lines.
OP_CASES = {
    "ring": ("h200-ring-allreduce.cfg", "ring", "allreduce", "ring"),
    "tree": ("h200-tree-allreduce.cfg", "switched", "allreduce", "tree"),
    "nvls": ("h200-nvls-allgather.cfg", "switched", "allgather", "nvls"),
}

# (op, dataSizeBytes, numGpus). Ring is exercised across the full
# {1MiB, 256MiB} x {4, 8} matrix (the calibrated primary path); tree and nvls
# are smoke-checked at 1MiB / 8-GPU (their switched path is known bit-identical
# and larger NVLS sizes are time-bounded in ns-3).
PARITY_MATRIX = [
    ("ring", 1 << 20, 4),
    ("ring", 1 << 20, 8),
    ("ring", 1 << 20 << 8, 4),
    ("ring", 1 << 20 << 8, 8),
    ("tree", 1 << 20, 8),
    ("nvls", 1 << 20, 8),
]


def parse_profile(path):
    """Parse a `key = value` profile (INI-style, '#' comments) into a dict."""
    kv = {}
    with open(path) as f:
        for raw in f:
            line = raw.split("#", 1)[0].strip()
            if "=" not in line:
                continue
            k, v = line.split("=", 1)
            kv[k.strip()] = v.strip()
    return kv


def inline_flags_from_profile(profile):
    """Build the inline CLI flags that reproduce the profile's sourced values."""
    flags = []
    for key, flag in PROFILE_TO_FLAG.items():
        if key in profile:
            flags.append("{}={}".format(flag, profile[key]))
    return flags


def find_sim_binary():
    """Locate the built gpu-cluster-sim binary; None => use `./ns3 run`."""
    cands = []
    for b in glob.glob(os.path.join(ROOT, "build", "scratch", "*gpu-cluster-sim*")):
        if b.endswith(".o") or not os.access(b, os.X_OK):
            continue
        cands.append(b)
    # Prefer a non-debug (release) binary if both exist; else the first match.
    rel = [c for c in cands if "debug" not in os.path.basename(c)]
    if rel:
        return rel[0]
    return cands[0] if cands else None


def run_sim(args, timeout=300):
    """Run gpu-cluster-sim with args; return (status, simTimeNs) parsed."""
    binary = find_sim_binary()
    if binary:
        cmd = [binary] + args
        cwd = None
    else:
        # Fall back to the ns3 wrapper (self-builds if needed).
        cmd = ["./ns3", "run", "gpu-cluster-sim " + " ".join(args)]
        cwd = ROOT
    proc = subprocess.run(
        cmd, cwd=cwd or ROOT, capture_output=True, text=True, timeout=timeout
    )
    out = proc.stdout + proc.stderr
    status = None
    sim_ns = None
    m = re.search(r"^status=(\S+)", out, re.MULTILINE)
    if m:
        status = m.group(1)
    m = re.search(r"^simTimeNs=(\d+)", out, re.MULTILINE)
    if m:
        sim_ns = int(m.group(1))
    return status, sim_ns, out


def run_config(op, num_gpus, data_size):
    cfg = OP_CASES[op][0]
    cfg_path = os.path.join("configs", "protocol_configs", cfg)
    args = [
        "--protocolConfig={}".format(cfg_path),
        "--numGpus={}".format(num_gpus),
        "--dataSize={}".format(data_size),
        "--RngRun=1",
    ]
    return run_sim(args)


def run_inline(op, num_gpus, data_size, profile):
    _, topology, collective, algorithm = OP_CASES[op]
    args = [
        "--topology={}".format(topology),
        "--collective={}".format(collective),
        "--algorithm={}".format(algorithm),
        "--numGpus={}".format(num_gpus),
        "--dataSize={}".format(data_size),
        "--RngRun=1",
    ] + inline_flags_from_profile(profile)
    return run_sim(args)


def check_parity(op, num_gpus, data_size, profile):
    cs, cn, _ = run_config(op, num_gpus, data_size)
    is_, in_, _ = run_inline(op, num_gpus, data_size, profile)
    label = "{} {}B {}gpu".format(op, data_size, num_gpus)
    assert cs == "complete", "{}: config status={} (expected complete)".format(label, cs)
    assert is_ == "complete", "{}: inline status={} (expected complete)".format(label, is_)
    assert cn is not None and in_ is not None, "{}: missing simTimeNs (config={}, inline={})".format(
        label, cn, in_
    )
    assert cn == in_, (
        "{}: NOT bit-identical: config simTimeNs={} != inline simTimeNs={}".format(
            label, cn, in_
        )
    )
    return cn


def test_config_vs_inline_parity():
    profile = parse_profile(PROFILE)
    assert profile, "failed to parse profile {}".format(PROFILE)
    # Sanity: the keys the C++ sources are all present in the profile.
    missing = [k for k in PROFILE_TO_FLAG if k not in profile]
    assert not missing, "profile missing sourced keys: {}".format(missing)
    for op, data_size, num_gpus in PARITY_MATRIX:
        check_parity(op, num_gpus, data_size, profile)


if __name__ == "__main__":
    # Direct (non-pytest) runner: prints a one-line verdict per case.
    profile = parse_profile(PROFILE)
    if not profile:
        print("FAIL: could not parse {}".format(PROFILE))
        sys.exit(1)
    missing = [k for k in PROFILE_TO_FLAG if k not in profile]
    if missing:
        print("FAIL: profile missing sourced keys: {}".format(missing))
        sys.exit(1)
    ok = True
    for op, data_size, num_gpus in PARITY_MATRIX:
        try:
            ns = check_parity(op, num_gpus, data_size, profile)
            print("PASS  {:6s} {:>10}B {}gpu  simTimeNs={}  (config==inline)".format(
                op, data_size, num_gpus, ns))
        except AssertionError as e:
            ok = False
            print("FAIL  {}".format(e))
    sys.exit(0 if ok else 1)
