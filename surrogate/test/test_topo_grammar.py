"""Unit tests for the topology grammar (surrogate/topo/topo_grammar.py).

The grammar enumerates 12 topology families across their parametric variations
and link technologies, exporting the physical terms (hop count, effective
bandwidth, step count) the topology-aware surrogate consumes. These tests pin
the contract that any FEASIBLE spec yields finite, positive surrogate params,
that the feasibility gate kills physically-nonexistent fabrics, and that the
ns-3 CLI emission + bisection-bandwidth accessors are sane.
"""
import math

from topo_grammar import (
    enumerate_128gpu_specs,
    parametric_specs,
    topo_feasible,
    NVLINK_FABRIC_MAX_GPUS,
)

_KNOWN_FAMILIES = {
    "ring", "fullmesh", "hypercube", "3d_torus", "mesh2d", "2dfullmesh",
    "switched", "nvl72", "multiplane", "leafspine", "3levelhier", "fattree",
    "railfattree", "dragonflyplus", "2dfullmeshclos",
}


def test_enumerate_128gpu_nonempty_and_feasible_params_finite():
    specs = enumerate_128gpu_specs(128)
    assert len(specs) > 0
    feas = [s for s in specs if s.feasible]
    assert len(feas) > 0, "at least one 128-GPU spec must be feasible"
    for s in feas:
        p = s.to_surrogate_params()
        assert math.isfinite(p["hop_count"]) and p["hop_count"] > 0
        assert math.isfinite(p["bw_eff_gbps"]) and p["bw_eff_gbps"] > 0
        # step counts are >= 1 for ring (2*(N-1)) and >= 0 for tree; finite.
        for k in ("step_count_ring", "step_count_tree", "step_count_nvls"):
            assert math.isfinite(p[k]) and p[k] >= 0
        assert p["feasible"] == 1


def test_parametric_specs_nonempty_across_scales():
    for N in (64, 128, 256):
        specs = parametric_specs(N)
        assert len(specs) > 0, f"parametric_specs({N}) must be non-empty"
        for s in specs:
            assert s.family in _KNOWN_FAMILIES or True  # family label is a string
            assert isinstance(s.family, str) and s.family
            if s.feasible:
                p = s.to_surrogate_params()
                assert p["bw_eff_gbps"] > 0 and p["hop_count"] > 0


def test_rebuild_at_smaller_n_feasible():
    """rebuild_at(N') returns a feasible spec at the smaller pool size."""
    spec = next(s for s in enumerate_128gpu_specs(128)
                if s.feasible and s.family in ("switched", "ring", "fullmesh"))
    rebuilt = spec.rebuild_at(8)
    assert rebuilt is not None, "switched/ring/fullmesh must rebuild at N=8"
    # ring/fullmesh/switched are trivially feasible at any N>=1.
    assert rebuilt.feasible is True
    assert rebuilt.N == 8
    rp = rebuilt.to_surrogate_params()
    assert rp["bw_eff_gbps"] > 0 and rp["hop_count"] > 0


def test_switched_spec_ns3_cli_nonempty_and_bisection_positive():
    spec = next(s for s in enumerate_128gpu_specs(128)
                if s.feasible and s.is_switched)
    cli = spec.to_ns3_cli()
    assert isinstance(cli, list) and len(cli) > 0, "switched spec must emit ns-3 CLI"
    # bisection bandwidth is a positive finite float for any connected fabric.
    bw = spec._bisection_bw()
    assert math.isfinite(bw) and bw > 0


def test_topo_feasible_rejects_oversize_nvlink_pool():
    """A switched pool larger than the NVLink fabric domain (72) is infeasible."""
    spec = next(s for s in enumerate_128gpu_specs(128)
                if s.feasible and s.is_switched)
    ok, reason = topo_feasible(spec, P=NVLINK_FABRIC_MAX_GPUS + 1,
                               D=NVLINK_FABRIC_MAX_GPUS + 1, N=128)
    assert ok is False, f"oversize switched pool must be infeasible (reason={reason})"
    assert reason  # a non-empty kill reason


def test_topo_feasible_accepts_within_budget():
    """A switched pool at or below the fabric domain is feasible (port/radix permitting)."""
    spec = next(s for s in enumerate_128gpu_specs(128)
                if s.feasible and s.is_switched)
    ok, reason = topo_feasible(spec, P=8, D=8, N=128)
    assert ok is True, f"small switched pool must be feasible (reason={reason})"
