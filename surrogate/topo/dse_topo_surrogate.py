#!/usr/bin/env python3
"""Topology-aware surrogate for collective latency.

Decomposes the per-group `t_step` constant into physical terms so the model
generalizes to NEW (topology, algorithm, message size) combinations without a
per-topology fit.

Model (per collective):
  ring (pipelined):
    latency = startup + step_count * (hop*LINK_LAT + chunk/(bw_eff*proto_eff) + sw_ring)
    step_count = 2*(N-1); chunk = size/N
  tree / nvls / sharp / fullmesh (concurrent broadcast/reduce):
    latency = startup + hop*LINK_LAT + size/(bw_eff*proto_eff) + sw_flat
    The critical path is O(1) switch traversals, NOT step_count sequential
    hops — ns-3 tree completes small-msg ARs in ~1us comm after startup, far
    below step_count*hop, so a sequential model overestimates ~14x. sw_flat is
    a per-collective (not per-step) overhead.
  proto_eff: LL 0.5, LL128 0.9375, SIMPLE 1.0.

Only sw_ring (per-step, ring) and sw_flat (per-collective, tree/nvls) are
fit residuals; hop_count, bw_eff, step_count are COMPUTED from the TopoSpec /
algorithm. The residuals were calibrated against an H200 128-GPU ns-3 end-to-end
sweep (DEFAULT_FIT below, MAPE 0.47% over 18 ring points) and are embedded so
this module runs without that sweep data.

Usage:
  python3 dse_topo_surrogate.py            # predict latency across sizes for the
                                           #   first feasible spec in configs/dse/topo_specs.csv
  python3 dse_topo_surrogate.py --all      # predict one size across all feasible specs
"""
import argparse
import csv
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, _HERE)

from topo_grammar import enumerate_128gpu_specs, TopoSpec
from algo_selector import step_count, select_algorithm

LINK_LATENCY_NS = 400          # --latency=400
LINK_LATENCY_US = LINK_LATENCY_NS / 1000.0
PROTO_EFF = {"LL": 0.5, "LL128": 0.9375, "SIMPLE": 1.0}
PROTO_STARTUP_US = {"LL": 15.0, "LL128": 15.0, "SIMPLE": 65.0}
# The ns-3 runners force protocol LL128 (--forceProtocol=1).
FORCE_PROTO = "LL128"

# Calibrated per-group residuals (us), fit on an H200 128-GPU ns-3 e2e sweep.
#   sw_ring  : per-step overhead for the RING algorithm (pipelined).
#   sw_flat  : per-collective overhead for tree/nvls/sharp/fullmesh.
# MAPE(ring) = 0.47% over 18 points; ring sweep sizes 64KiB–1GiB.
DEFAULT_FIT = {
    "direct":   {"sw_ring_us": 0.8309, "sw_flat_us": 0.1817},
    "switched": {"sw_ring_us": 0.9155, "sw_flat_us": 0.9240},
    "fat_tree": {"sw_ring_us": 0.9142, "sw_flat_us": 0.9349},
}

# Family → residual group. The e2e calibration defined three groups by fabric
# shape; this map generalizes the grouping to every grammar family.
_FAT_TREE_FAMILIES = {"fattree", "railfattree", "2dfullmeshclos"}
_SWITCHED_FAMILIES = {"switched", "nvl72", "multiplane", "leafspine",
                      "3levelhier", "dragonflyplus"}
# Everything else (ring, fullmesh, hypercube, torus, mesh, 2dfullmesh) is direct.


def group_of(spec) -> str:
    """Classify a TopoSpec into the residual group (direct/switched/fat_tree)."""
    fam = getattr(spec, "family", spec) if not isinstance(spec, str) else spec
    if fam in _FAT_TREE_FAMILIES:
        return "fat_tree"
    if fam in _SWITCHED_FAMILIES:
        return "switched"
    return "direct"


def chunk_per_step(algo, size, N):
    if algo == "ring":
        return size / N
    if algo == "tree":
        return size / 2          # halves each tree level (approx)
    return size                  # nvls / sharp / fullmesh


def per_step_latency_us(spec, chunk_bytes, proto, sw_overhead_us):
    hop = spec.hop_count
    bw_GBs = spec.bw_eff_gbps if spec.bw_eff_gbps > 0 else 1.0
    proto_eff = PROTO_EFF[proto]
    ser_us = chunk_bytes / (bw_GBs * 1000.0 * proto_eff)
    return hop * LINK_LATENCY_US + ser_us + sw_overhead_us


def collective_latency_us(spec, algo, size_bytes, N, proto, sw_ring_us,
                          sw_flat_us=0.0):
    """Per-collective latency.

    Ring is pipelined: step_count sequential steps, each carrying chunk=size/N.
    Tree / nvls / sharp / fullmesh are concurrent broadcast/reduce: the critical
    path is O(1) switch traversals, so model as startup + full-size serialization
    + a flat per-collective overhead (sw_flat). (The ns-3 tree injector completes
    small-message ARs in ~1us of comm after startup — far below step_count*hop,
    confirming concurrency; a sequential 14-step model overestimates ~14x.)
    """
    startup = PROTO_STARTUP_US[proto]
    bw_GBs = spec.bw_eff_gbps if spec.bw_eff_gbps > 0 else 1.0
    proto_eff = PROTO_EFF[proto]
    if algo == "ring":
        sc = step_count("ring", N)
        chunk = chunk_per_step("ring", size_bytes, N)
        ser_us = chunk / (bw_GBs * 1000.0 * proto_eff)
        return startup + sc * (spec.hop_count * LINK_LATENCY_US + ser_us + sw_ring_us)
    # tree / nvls / sharp / fullmesh: concurrent broadcast.
    ser_us = size_bytes / (bw_GBs * 1000.0 * proto_eff)
    return startup + spec.hop_count * LINK_LATENCY_US + ser_us + sw_flat_us


def predict(spec, size_bytes, N=128, algo="auto", proto=FORCE_PROTO,
            collective="allreduce", fit=DEFAULT_FIT):
    """Predict one collective's latency (us) for a TopoSpec + size.

    algo="auto" resolves per-collective via algo_selector (small -> tree,
    large -> ring, nvls on switched). Returns the latency in microseconds.
    """
    if algo == "auto":
        algo = select_algorithm(spec, size_bytes, N, collective)
    g = group_of(spec)
    f = fit.get(g, fit["direct"])
    return collective_latency_us(spec, algo, size_bytes, N, proto,
                                 f["sw_ring_us"], f["sw_flat_us"])


def trace_e2e_us(spec, sizes, compute_us, algo_mode, sw_ring_us, sw_flat_us=0.0,
                 N=128):
    """Sum collective latencies + compute over a trace.

    algo_mode: "ring" (force ring) or "auto" (resolve per-collective via
    algo_selector — small -> tree, large -> ring, nvls on switched).
    """
    proto = FORCE_PROTO
    total = 0.0
    for s in sizes:
        if algo_mode == "auto":
            a = select_algorithm(spec, s, N, "allreduce") if spec is not None else "ring"
        else:
            a = "ring"
        total += collective_latency_us(spec, a, s, N, proto, sw_ring_us, sw_flat_us)
    return total + compute_us


def _load_specs_csv(path):
    """Load the shipped topo_specs.csv into a list of TopoSpec objects."""
    specs = []
    for row in csv.DictReader(open(path)):
        for s in enumerate_128gpu_specs(int(row.get("num_gpus", 128))):
            if s.spec_id == row.get("spec_id", ""):
                specs.append(s)
                break
    return specs


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--all", action="store_true",
                    help="predict one size across all feasible specs "
                         "(default: a size sweep for the first feasible spec)")
    ap.add_argument("--size", type=lambda x: int(x, 0), default=1 << 20,
                    help="message size in bytes (default 1MiB), used with --all")
    ap.add_argument("--num-gpus", type=int, default=128)
    a = ap.parse_args()

    specs = [s for s in enumerate_128gpu_specs(a.num_gpus) if s.feasible]
    if not specs:
        print("No feasible TopoSpecs at N={} — check the grammar.".format(a.num_gpus))
        return 1

    if a.all:
        print("=== Predicted allreduce latency @ {} bytes, N={} ===".format(
            a.size, a.num_gpus))
        print("{:18s} {:14s} {:>8s} {:>8s} {:>10s} {:>10s}".format(
            "spec_id", "family", "hop", "bw_GBs", "algo", "lat_us"))
        for s in specs:
            lat = predict(s, a.size, N=a.num_gpus)
            algo = select_algorithm(s, a.size, a.num_gpus, "allreduce")
            print("{:18s} {:14s} {:8.1f} {:8.1f} {:>10s} {:10.2f}".format(
                s.spec_id, s.family, s.hop_count, s.bw_eff_gbps, algo, lat))
    else:
        s = specs[0]
        sizes = [1 << 16, 1 << 18, 1 << 20, 1 << 22, 1 << 24, 1 << 26,
                 1 << 28, 1 << 30]
        print("=== Predicted allreduce latency sweep, spec={} ({} / {}) N={} ===".format(
            s.spec_id, s.family, s.link_tech, a.num_gpus))
        print("group={}".format(group_of(s)))
        print("{:>12s} {:>10s} {:>10s}".format("size_B", "algo", "lat_us"))
        for sz in sizes:
            lat = predict(s, sz, N=a.num_gpus)
            algo = select_algorithm(s, sz, a.num_gpus, "allreduce")
            print("{:12d} {:>10s} {:10.2f}".format(sz, algo, lat))
    return 0


if __name__ == "__main__":
    sys.exit(main())
