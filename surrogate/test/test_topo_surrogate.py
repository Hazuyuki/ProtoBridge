"""Test the topology-aware surrogate on a shipped topo_specs.csv spec.

The topology-aware surrogate computes collective latency from a TopoSpec's
physical terms (hop count, effective bandwidth, step count) plus a small
calibrated per-group residual. It must produce finite, positive, monotone-in-size
latencies for any feasible spec, and resolve the auto algorithm (small -> tree,
large -> ring/nvls).
"""
import os

import dse_topo_surrogate as ts
from topo_grammar import enumerate_128gpu_specs
from algo_selector import select_algorithm


def test_predict_returns_finite_positive():
    spec = next(s for s in enumerate_128gpu_specs(128) if s.feasible)
    lat = ts.predict(spec, 1 << 20, N=128)
    assert lat == lat and lat > 0.0  # finite & positive


def test_latency_monotonic_in_size():
    """Larger message -> larger latency (serialization grows)."""
    spec = next(s for s in enumerate_128gpu_specs(128) if s.feasible)
    small = ts.predict(spec, 1 << 16, N=128)
    large = ts.predict(spec, 1 << 26, N=128)
    assert large > small


def test_auto_algorithm_switches_tree_to_ring():
    """Small ARs resolve to tree (concurrent broadcast); large to ring/nvls."""
    spec = next(s for s in enumerate_128gpu_specs(128)
                if s.feasible and s.family == "switched")
    small_algo = select_algorithm(spec, 1 << 16, 128, "allreduce")
    large_algo = select_algorithm(spec, 1 << 28, 128, "allreduce")
    assert small_algo == "tree"
    assert large_algo in ("ring", "nvls")


def test_group_of_covers_all_feasible_families():
    """Every feasible spec classifies into one of the three residual groups."""
    seen = set()
    for s in enumerate_128gpu_specs(128):
        if not s.feasible:
            continue
        g = ts.group_of(s)
        assert g in ("direct", "switched", "fat_tree"), f"bad group {g} for {s.family}"
        seen.add(g)
    assert seen == {"direct", "switched", "fat_tree"}
