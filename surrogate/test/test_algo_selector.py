"""Light unit tests for the algorithm selector (surrogate/topo/algo_selector.py).

select_algorithm resolves the wire algorithm for one collective from a TopoSpec
+ message size + collective type. These tests pin the contract that the return is
always a known algorithm label, that the full feasible spec set never raises,
and that the small/large size branch points (tree for latency-bound small
allreduce, ring for bandwidth-bound large) and the NVLS domain guard (N<=8)
hold.
"""
from algo_selector import select_algorithm, step_count
from topo_grammar import enumerate_128gpu_specs

_KNOWN = {"auto", "ring", "tree", "nvls", "sharp", "fullmesh"}


def test_returns_known_algorithm_for_all_collectives():
    spec = next(s for s in enumerate_128gpu_specs(128) if s.feasible)
    for coll in ("allreduce", "allgather", "reducescatter", "alltoall",
                 "broadcast", "reduce"):
        for size in (1 << 16, 1 << 23, 1 << 26, 1 << 28):
            algo = select_algorithm(spec, size, N=128, collective=coll)
            assert algo in _KNOWN, f"unknown algo {algo!r} for {coll}@{size}"


def test_no_exceptions_on_full_feasible_set():
    feas = [s for s in enumerate_128gpu_specs(128) if s.feasible]
    assert len(feas) > 0
    for s in feas:
        for coll in ("allreduce", "allgather", "alltoall"):
            select_algorithm(s, 1 << 20, N=128, collective=coll)  # must not raise


def test_small_allreduce_tree_large_ring_on_non_nvls_domain():
    """At N=128 (N>8 so NVLS is off), small allreduce -> tree, large -> ring."""
    spec = next(s for s in enumerate_128gpu_specs(128)
                if s.feasible and not s.supports_nvls)
    small = select_algorithm(spec, 1 << 16, N=128, collective="allreduce")
    large = select_algorithm(spec, 1 << 28, N=128, collective="allreduce")
    assert small == "tree", f"small allreduce should be tree, got {small}"
    assert large == "ring", f"large allreduce should be ring, got {large}"


def test_nvls_only_within_single_switch_domain():
    """NVLS is returned only when the spec supports it AND N<=8."""
    # Find a spec that supports NVLS (switched family) and rebuild at N=8.
    big = next(s for s in enumerate_128gpu_specs(128)
               if s.feasible and s.supports_nvls)
    small_n = big.rebuild_at(8)
    assert small_n is not None and small_n.supports_nvls
    # At N=8, large message on a NVLS-supporting spec -> nvls.
    assert select_algorithm(small_n, 1 << 24, N=8,
                            collective="allgather") == "nvls"
    # Same family at N=128 must NOT pick nvls (N>8 guard).
    assert select_algorithm(big, 1 << 24, N=128,
                            collective="allgather") != "nvls"


def test_step_count_finite_and_ring_scales_with_n():
    assert step_count("ring", 8) >= 1
    assert step_count("ring", 128) > step_count("ring", 8)
