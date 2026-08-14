#!/usr/bin/env python3
"""Adaptive collective algorithm selector (Python mirror of the C++
ResolveAutoAlgorithm in scratch/gpu-cluster-sim.cc).

Picks the collective algorithm from (topology, message size, N). The rules are
NCCL-like: small/latency-bound messages favor tree (2*log2(N) steps); large /
bandwidth-bound favor ring (2*(N-1) pipelined); NVSwitch fabrics can use NVLS /
SHARP (switch-assisted, ~2 steps, but with a higher startup cost so only worth
it for larger messages).

The C++ side resolves --algorithm=auto per-phase inside the simulator; this
module is used by the DSE / runner scripts for any pre-sim resolution (e.g.
labelling the surrogate search space, or choosing the algorithm column for a
config-to-CLI translator when the user wants the selector's answer up front).
Keep the thresholds in sync with the C++ constants.
"""

from __future__ import annotations
from dataclasses import dataclass

# ---------------------------------------------------------------------------
# Thresholds (must match scratch/gpu-cluster-sim.cc ResolveAutoAlgorithm).
# ---------------------------------------------------------------------------
MEDIUM = 1 << 26        # 64 MiB: below this -> tree (latency-bound)
NVLS_THRESH = 1 << 23   # 8 MiB: NVSwitch-assisted only worth it at/above this

# Plain NVLS (NVSwitch in-network reduction) is valid ONLY within a single
# NVSwitch domain = one NVSwitch ASIC = <=8 GPUs (single node). NCCL disables
# plain NVLS the moment the communicator spans >1 node
# (NCCL src/graph/tuning.cc: `a==NCCL_ALGO_NVLS && comm->nNodes>1 -> disable`).
# NVLSTree (a DIFFERENT algorithm, NCCL 2.18+, MNNVL/NVL72 <=72) extends the
# idea across nodes but is NOT modeled here; switched pools of 8<N<=72 fall
# through to tree (NVLSTree-conservative, pool-variant). Pools >72 have no
# NVLink fabric at all (topo_grammar.topo_feasible kills them).
#   https://docs.nvidia.com/deeplearning/nccl/user-guide/docs/env.html
NVLS_DOMAIN_MAX_GPUS = 8

# The SHARP/NVLS injector only runs on the single-NVSwitch `switched` fabric
# (BuildFullyConnected). nvl72/multiplane carry NVSwitch ASICs but their
# multi-plane routing breaks the injector's single-switch assumption, so the
# selector must NOT advertise nvls for them (auto would otherwise abort).
NVSWITCH_TOPOLOGIES = {"switched"}


@dataclass
class TopoSpec:
    """Minimal topology descriptor used by the selector.

    Only the fields the selector inspects are required; the full TopoSpec in
    scripts/topo_grammar.py carries far more and is compatible by superset.
    """
    family: str
    is_switched: bool = False
    supports_nvls: bool = False


def supports_nvls(topo) -> bool:
    """True if the NVLS/SHARP injector runs on this topology.

    Only the single-NVSwitch `switched` fabric is supported today; nvl72/
    multiplane carry NVSwitch ASICs but the injector's single-switch routing
    assumption breaks on multi-plane fabrics.
    """
    if topo is None:
        return False
    # Duck-type: a TopoSpec-like object exposes `supports_nvls`.
    if hasattr(topo, "supports_nvls"):
        return bool(getattr(topo, "supports_nvls"))
    # Bare string: family name.
    return topo in NVSWITCH_TOPOLOGIES


def select_algorithm(topo_spec, msg_bytes: int, N: int,
                     collective: str, trace_mode: bool = False) -> str:
    """Resolve the algorithm for one collective.

    Args:
        topo_spec: a TopoSpec, a family string, or None.
        msg_bytes: per-collective message size in bytes.
        N: number of GPUs.
        collective: allreduce / allgather / reducescatter / alltoall / etc.
        trace_mode: if True, the C++ trace factory will resolve `auto` itself
            per-phase; the Python side then only needs a label, and we return
            "auto" so the simulator does the per-phase split. Set False for
            single-shot / surrogate use where one concrete algo is needed.

    Returns:
        One of {auto, ring, tree, nvls, sharp, fullmesh}.
    """
    if trace_mode:
        return "auto"

    coll = (collective or "").lower()
    has_nvls = supports_nvls(topo_spec)
    size = int(msg_bytes)
    # Plain NVLS is valid only within a single NVSwitch domain (<=8 GPUs). For
    # switched pools of 8<N<=72 the realistic algorithm is NVLSTree (a tree of
    # NVLS multicast steps, pool-variant); we approximate it with `tree` here
    # so the latency grows with pool size instead of staying pool-invariant
    # (the prior unbounded-NVLS routing gave an unphysical NVLS floor for
    # 16/48/...-GPU switched pools, collapsing the inter-vs-tgs Pareto front).
    nvls_ok = has_nvls and N <= NVLS_DOMAIN_MAX_GPUS

    if coll in ("allgather", "reducescatter"):
        if nvls_ok and size >= NVLS_THRESH:
            return "nvls"
        return "ring"

    if coll == "allreduce":
        if nvls_ok and size >= NVLS_THRESH:
            return "nvls"
        # Tree wins for latency-bound small messages (far fewer steps).
        if size < MEDIUM:
            return "tree"
        return "ring"

    # alltoall / broadcast / reduce: ring is the only general injector.
    return "ring"


def step_count(algo: str, N: int, collective: str = "allreduce") -> int:
    """Logical communication step count for an algorithm at size N.

    Used by the topology-aware surrogate (surrogate/topo/dse_topo_surrogate.py). This is
    the algorithmic step count; physical hop latency is applied per-step by the
    surrogate from the TopoSpec.
    """
    a = (algo or "ring").lower()
    if a in ("sharp", "nvls", "fullmesh"):
        return 2 if a != "fullmesh" else 1
    if a == "tree":
        # 2 * ceil(log2 N); reduce-scatter + all-gather halves of allreduce.
        import math
        return 2 * max(1, math.ceil(math.log2(max(N, 2))))
    # ring
    return 2 * (N - 1)


if __name__ == "__main__":
    import argparse
    ap = argparse.ArgumentParser(description="Adaptive collective algorithm selector")
    ap.add_argument("--topo", default="switched", help="topology family")
    ap.add_argument("--size", type=int, required=True, help="message size in bytes")
    ap.add_argument("--N", type=int, default=128, help="GPU count")
    ap.add_argument("--collective", default="allreduce")
    ap.add_argument("--trace-mode", action="store_true")
    args = ap.parse_args()
    print(select_algorithm(args.topo, args.size, args.N,
                           args.collective, args.trace_mode))
