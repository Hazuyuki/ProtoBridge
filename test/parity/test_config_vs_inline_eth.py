#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Bit-identical parity: config-driven vs inline-CLI for the lossy Ethernet So fabric.

The config-primary path (``--protocolConfig=...cfg``) sources the ``[stack]``
profile's PEX + fabric-hardware values into the same local CLI variables the
inline path consumes. ``test_config_vs_inline_parity.py`` gates that bit-identity
for the scale-UP (Su, single-node) path. This test is the scale-OUT (So,
multi-node) counterpart: it proves the inter-node fabric axes --
``interNodeFabricType`` / ``interNodeTopology`` / ``interNodeBandwidthGbps`` /
``interNodeLatencyNs`` -- newly sourced from the ``.cfg``'s ``[stack]`` section,
make a config-driven lossy-Ethernet run BIT-IDENTICAL to the inline CLI command
built from the same values (same code, same RNG).

This is the gate that closes the config-primary gap the So fabric had: a single
``.cfg`` now adapts across protocols (``interNodeFabricType = ETH`` vs ``RoCE``)
without any ``--interNode*`` CLI flag, and the config form reproduces the
calibrated inline path exactly, not approximately -- the same guarantee the Su
path already has.

The test parses ``configs/protocol_configs/eth-hierarchical.cfg`` (its ``[stack]``
overrides + the referenced ``h200-ll128.profile``), emits the equivalent inline
``--interNode*`` + Su flags, runs ``gpu-cluster-sim`` twice per case (config vs
inline) for {2-node, 4-node} hierarchical allreduce at 256 MiB, and asserts
``simTimeNs`` is exactly equal.

Run:
    python3 -m pytest test/parity/test_config_vs_inline_eth.py -v
    python3 test/parity/test_config_vs_inline_eth.py
"""

import glob
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CFG = os.path.join(ROOT, "configs", "protocol_configs", "eth-hierarchical.cfg")

# Profile/cfg key -> inline CLI flag. MUST mirror the sourcing block in
# scratch/gpu-cluster-sim.cc (the keys it reads via prof.Get / apply* and the
# cmd.AddValue flag names). The first four are the inter-node (So) axes this
# test exists to cover; the rest are the Su/PEX axes already covered by
# test_config_vs_inline_parity.py, mirrored here so the inline baseline
# reproduces the SAME Su fabric the config path gets from the profile.
PROFILE_TO_FLAG = {
    # inter-node (So) fabric -- the new config-sourced axes
    "interNodeFabricType": "--interNodeFabricType",
    "interNodeTopology": "--interNodeTopology",
    "interNodeBandwidthGbps": "--interNodeBandwidth",
    "interNodeLatencyNs": "--interNodeLatency",
    # fabric hardware (Su)
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

# (numNodes, dataSizeBytes). 2-node = minimal So fabric; 4-node = multi-hop.
# 256 MiB (the WU4 e2e size). Non-blocking host topology (in the .cfg) => no
# incast => zero drops => deterministic, so bit-identity is achievable.
PARITY_MATRIX = [
    (2, 1 << 20 << 8),
    (4, 1 << 20 << 8),
]

GPUS_PER_NODE = 8


def _parse_kv(path):
    """Parse a `key = value` file (INI-style, '#' comments) into a dict."""
    kv = {}
    with open(path) as f:
        for raw in f:
            line = raw.split("#", 1)[0].strip()
            if "=" not in line:
                continue
            k, v = line.split("=", 1)
            kv[k.strip()] = v.strip()
    return kv


def cfg_stack_values(cfg_path):
    """Parse the .cfg's [stack] section keys (profile= + overrides) into a dict.

    Mirrors ProtocolConfig: `profile =` is consumed separately; every other
    [stack] key overrides the profile and is sourced into the CLI vars.
    """
    stack = {}
    in_stack = False
    with open(cfg_path) as f:
        for raw in f:
            line = raw.split("#", 1)[0].strip()
            if line.startswith("[") and line.endswith("]"):
                in_stack = (line == "[stack]")
                continue
            if not in_stack or "=" not in line:
                continue
            k, v = line.split("=", 1)
            stack[k.strip()] = v.strip()
    return stack


def sourced_values(cfg_path):
    """Build the full key->value map the config path sources: profile then
    [stack] overrides (override wins), exactly as scratch does."""
    stack = cfg_stack_values(cfg_path)
    prof_path = stack.pop("profile", None)
    vals = {}
    if prof_path:
        vals.update(_parse_kv(os.path.join(ROOT, prof_path)))
    vals.update(stack)  # [stack] overrides win
    return vals


def inline_flags(cfg_path):
    """Build the inline CLI flags that reproduce the config path's sourced
    values (inter-node + Su + PEX)."""
    vals = sourced_values(cfg_path)
    flags = []
    for key, flag in PROFILE_TO_FLAG.items():
        if key in vals:
            flags.append("{}={}".format(flag, vals[key]))
    return flags


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
    status = re.search(r"^status=(\S+)", out, re.MULTILINE)
    sim_ns = re.search(r"^simTimeNs=(\d+)", out, re.MULTILINE)
    return (status.group(1) if status else None,
            int(sim_ns.group(1)) if sim_ns else None, out)


def _case(num_nodes, data_size):
    common = ["--numNodes={}".format(num_nodes),
              "--gpusPerNode={}".format(GPUS_PER_NODE),
              "--dataSize={}".format(data_size), "--RngRun=1"]
    # Config-driven: the .cfg's [stack] selects the ETH So fabric + [op] the
    # hierarchical allreduce. No --interNode* / --collective / --algorithm CLI.
    cfg_args = common + ["--protocolConfig=" + CFG]
    # Inline: every sourced key as an explicit CLI flag + the op axes.
    inline_args = common + inline_flags(CFG) + [
        "--collective=allreduce", "--algorithm=hierarchical"]
    cfg_status, cfg_ns, cfg_out = run_sim(cfg_args)
    inl_status, inl_ns, inl_out = run_sim(inline_args)
    return (cfg_status, cfg_ns, cfg_out, inl_status, inl_ns, inl_out)


def test_config_vs_inline_eth_parity():
    for num_nodes, data_size in PARITY_MATRIX:
        cs, cn, co, is_, in_, io = _case(num_nodes, data_size)
        label = "{}-node {}B".format(num_nodes, data_size)
        assert cs == "complete", (
            "{}: config run status={} (expected complete):\n{}".format(
                label, cs, co[-400:]))
        assert is_ == "complete", (
            "{}: inline run status={} (expected complete):\n{}".format(
                label, is_, io[-400:]))
        assert cn == in_, (
            "{}: config simTimeNs={} != inline simTimeNs={} (config-driven "
            "ETH must be bit-identical to the inline CLI path)".format(
                label, cn, in_))


if __name__ == "__main__":
    ok = True
    for num_nodes, data_size in PARITY_MATRIX:
        label = "{}-node {}B".format(num_nodes, data_size)
        try:
            cs, cn, co, is_, in_, io = _case(num_nodes, data_size)
            assert cs == "complete" and is_ == "complete", (
                "status cfg={} inline={}".format(cs, is_))
            assert cn == in_, "simTimeNs cfg={} != inline={}".format(cn, in_)
            print("PASS  {}  config={} inline={} (bit-identical)".format(
                label, cn, in_))
        except AssertionError as e:
            ok = False
            print("FAIL  {}  {}".format(label, e))
    sys.exit(0 if ok else 1)
