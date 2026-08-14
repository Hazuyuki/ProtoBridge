#!/usr/bin/env python3
"""Parametric interconnect topology grammar for the DSE.

A TopoSpec is one concrete 128-GPU interconnect, described by a family (which
maps to an existing ns-3 `Build*` function) plus its parameters and link
technology. The grammar enumerates the 12 supported families across their
parametric variations and link technologies, yielding the topology axis of the
G3.5 search space.

Design choices (per the approved plan):
  * Parametric families — reuses the existing C++ `Build*` functions; no new
    generic graph builder.
  * `to_ns3_cli()` emits the exact `--topology=...` + extra params + link-tech
    flags the simulator already understands (reuses dse_config_to_ns3.
    LINK_TECH_MAP).
  * `to_cost()` reuses dse_cost_model unit costs, computed from the spec's own
    switch/link counts (so parametric variations get distinct costs, unlike the
    fixed TOPO_PARAMS table).
  * `to_surrogate_params()` exports the derived fields the topology-aware
    surrogate (surrogate/topo/dse_topo_surrogate.py) consumes: hop_count,
    bw_eff_gbps, step_count, is_switched, supports_nvls.

The ring/tree collective embedding (G3.1) is installed in C++ from the
topology name; the `embedding` column here documents which schedule each
family uses (Gray code / serpentine / snake / NodeId-sorted / identity).
"""
from __future__ import annotations

import csv
import math
import os
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Tuple

_HERE = os.path.dirname(os.path.abspath(__file__))

# Link-tech → (fabric, per-link bw_gbps, link_data_rate). Inlined from the
# ns-3 config generator so this module is self-contained (no companion file).
LINK_TECH_MAP = {
    "direct_electrical": {"fabric": "NVLink", "bw_gbps": 100, "link_data_rate": 100},
    "direct_optical":    {"fabric": "NVLink", "bw_gbps": 200, "link_data_rate": 200},
    "roce_optical":      {"fabric": "RoCE",   "bw_gbps": 400, "link_data_rate": 400},
    "ici_electrical":    {"fabric": "ICI",    "bw_gbps": 100, "link_data_rate": 100},
    "xccl_optical":      {"fabric": "NVLink", "bw_gbps": 600, "link_data_rate": 600},
}

# Unit costs (USD), inlined from the DSE cost model.
NUM_GPUS = 128
SWITCH_COST = {"nvsw": 8000, "eth": 4000, "mixed": 6000, "none": 0}
LINK_COST = {"electrical": 200, "optical": 1500}
VC_BUF_COST_PER_MB = 1.0      # SRAM
LLR_BUF_COST_PER_MB = 0.02    # DRAM


# ---------------------------------------------------------------------------
# Physical switch / fabric constraints (shipping ~2024-2026).
# Sources:
#   NVL72 = 72-GPU single non-blocking NVLink fabric (shipping max); NVL576 is
#     roadmap/not shipping -> beyond 72 GPUs no NVLink fabric, must use IB/Eth.
#     https://www.nvidia.com/en-us/data-center/gb200-nvl72/
#     https://www.nvidia.com/en-us/data-center/nvlink/
#   NVLS (NVSwitch in-network reduction) is valid ONLY within a single NVSwitch
#     domain (<=8 GPUs, single node). NCCL disables plain NVLS once the comm
#     spans >1 node; NVLSTree (<=72, NVL72) is a DIFFERENT algorithm.
#     https://docs.nvidia.com/deeplearning/nccl/user-guide/docs/env.html
#     NCCL src/graph/tuning.cc: (a==NCCL_ALGO_NVLS && comm->nNodes>1) disable=1
#   H200 per-GPU NVLink4 budget = 18 links (900 GB/s). A topology whose
#     per-GPU link demand exceeds 18 is unrealizable on H200.
#     https://www.nvidia.com/en-us/data-center/h200/
#   Single switch-ASIC radix = 64x800G (51.2T); largest shipping chassis ~576
#     ports (Arista 7800R3). A leaf/spine needing >576 ports/switch is not a
#     realizable single device.
#     https://www.broadcom.com/products/ethernet-connectivity/switching/strataxgs/bcm78900-series
#     https://www.arista.com/assets/data/pdf/Datasheets/7800R3-Data-Sheet.pdf
# ---------------------------------------------------------------------------
NVLINK_FABRIC_MAX_GPUS = 72          # NVL72 (shipping); >72 = no NVLink fabric
NVLS_DOMAIN_MAX_GPUS = 8             # single NVSwitch ASIC domain (intra-node)
NVLINKS_PER_GPU = 18                 # H200 NVLink4
SWITCH_CHASSIS_MAX_PORTS = 576       # largest shipping single-chassis radix

# Families that model a single-tier non-blocking NVLink/NVSwitch fabric (the
# only fabrics where NVLS / pool-invariant bisection apply). Beyond NVL72 these
# do not physically exist.
_NVLINK_FABRIC_FAMILIES = {"switched", "nvl72"}

# Direct-connect families where links_per_gpu is a connectivity DEMAND (not a
# free parameter) and must respect the 18-NVLink/GPU budget.
_DIRECT_NVLINK_FAMILIES = {"fullmesh", "2dfullmesh", "hypercube", "3d_torus",
                           "mesh2d", "ring", "multiplane", "switched", "nvl72"}

# Clos families subject to per-switch port-radix limits.
_CLOS_FAMILIES = {"leafspine", "3levelhier", "fattree", "railfattree", "dragonflyplus",
                  "2dfullmeshclos"}


def _rail_fattree_shape(spec, N: int) -> Tuple[int, int, int, int, int]:
    """Return leaves/rail, total leaves, spines, cores, and link multiplicity.

    A rail contains the same local GPU rank from every enclosure.  One leaf
    serves at most ``rail_nodes_per_leaf`` enclosures on one rail.  Parallel
    leaf-spine links keep 32 uplinks per leaf before the core tier appears.
    """
    rails = spec.rail_count or 8
    nodes_per_leaf = spec.rail_nodes_per_leaf or 32
    nodes = math.ceil(N / rails)
    leaves_per_rail = math.ceil(nodes / nodes_per_leaf)
    total_leaves = rails * leaves_per_rail
    default_spines, default_links, default_cores = _rail_fattree_defaults(
        N, rails, nodes_per_leaf)
    spines = spec.num_spine or default_spines
    cores = spec.rail_num_core or default_cores
    links_per_spine = spec.rail_links_per_spine or default_links
    return leaves_per_rail, total_leaves, spines, cores, links_per_spine


def _rail_fattree_defaults(N: int, rails: int = 8,
                           nodes_per_leaf: int = 32) -> Tuple[int, int, int]:
    """Choose the published rail-fat-tree switch counts at each scale."""
    nodes = math.ceil(N / rails)
    total_leaves = rails * math.ceil(nodes / nodes_per_leaf)
    if total_leaves <= 64:
        spines = next(
            candidate
            for candidate in (4, 8, 16, 32)
            if candidate >= math.ceil(total_leaves / 2)
        )
        return spines, 32 // spines, 0
    return total_leaves, 1, total_leaves // 2


def _clos_max_ports_per_switch(spec, N: int) -> int:
    """Worst-case per-switch port count for a Clos-family spec at N GPUs.
    A leaf carries N/num_leaf GPU downlinks + its uplinks to the upper tier."""
    fam = spec.family
    if fam == "leafspine":
        return (N // max(1, spec.num_leaf)) + spec.num_spine
    if fam == "3levelhier":
        return (N // max(1, spec.num_leaf)) + spec.num_mid + spec.num_spine
    if fam == "fattree":
        return spec.radix or 8                 # k-ary fat-tree: each switch radix k
    if fam == "railfattree":
        _, total_leaves, spines, cores, links_per_spine = _rail_fattree_shape(
            spec, N)
        if cores:
            leaf_ports = (spec.rail_nodes_per_leaf or 32) + 32
            spine_ports = 32 + 32
            core_ports = (spines * 32) // cores
            return max(leaf_ports, spine_ports, core_ports)
        leaf_ports = (spec.rail_nodes_per_leaf or 32) + spines * links_per_spine
        spine_ports = total_leaves * links_per_spine
        return max(leaf_ports, spine_ports)
    if fam == "dragonflyplus":
        total_leaf = (spec.num_leaf or 8) * (spec.dfly_routers_per_group or 1)
        return (N // max(1, total_leaf)) + (spec.num_leaf or 8)
    if fam == "2dfullmeshclos":
        return (N // max(1, spec.num_racks)) + (spec.rack_rows or 4)
    return 0


def topo_feasible(spec, P: int, D: int, N: int) -> Tuple[bool, str]:
    """Physical feasibility of a PD-split config on topology `spec`.

    A config is INFEASIBLE when it demands a physically-nonexistent fabric or
    exceeds a hardware port budget. The PD-split pools (P prefill, D decode)
    are the binding constraint: each pool runs its own collective on the spec's
    fabric, so a pool larger than the fabric domain is impossible regardless of
    the cluster size N. Returns (ok, reason).

    NOTE: does NOT model the algorithm-validity of NVLS at pool > 8 (that is
    enforced in algo_selector.select_algorithm); this gate is only the hard
    fabric/port-budget kill.
    """
    fam = spec.family
    # 1. NVLink fabric domain: switched/nvl72 pools must fit in one NVLink
    #    fabric (<=NVL72=72). A 1024-GPU "switched" pool has no physical fabric.
    if fam in _NVLINK_FABRIC_FAMILIES:
        for pool, tag in ((P, "P"), (D, "D")):
            if pool > NVLINK_FABRIC_MAX_GPUS:
                return False, f"{fam} {tag}-pool {pool}>{NVLINK_FABRIC_MAX_GPUS} (no NVLink fabric)"
    # 2. Per-GPU NVLink budget (18 on H200). Connectivity-demand families whose
    #    links_per_gpu exceeds 18 are unrealizable (fullmesh@N>=19, multiplane-mp8).
    if fam in _DIRECT_NVLINK_FAMILIES and spec.links_per_gpu > NVLINKS_PER_GPU:
        return False, f"{fam} links_per_gpu {spec.links_per_gpu}>{NVLINKS_PER_GPU}"
    # 3. Clos per-switch port radix (chassis ceiling 576).
    if fam in _CLOS_FAMILIES:
        ports = _clos_max_ports_per_switch(spec, N)
        if ports > SWITCH_CHASSIS_MAX_PORTS:
            return False, f"{fam} per-switch ports {ports}>{SWITCH_CHASSIS_MAX_PORTS}"
    if fam == "railfattree" and N % (spec.rail_count or 8):
        return False, f"railfattree requires N divisible by {spec.rail_count or 8} rails"
    if fam == "railfattree":
        _, leaves, spines, cores, links = _rail_fattree_shape(spec, N)
        if cores and (leaves % 64 or spines != leaves or cores * 2 != leaves or links != 1):
            return False, "railfattree core tier requires L=S=2C, L divisible by 64, and one leaf-spine link"
    return True, ""


# ---------------------------------------------------------------------------
# Family table: 12 families, each reusing an existing ns-3 Build* function.
# Columns: (ns3_topology, embedding_schedule, is_switched, supports_nvls,
#           feasible_at_128, default_link_techs, hop_count_fn)
# hop_count = representative per-step hop count for the EMBEDDED collective
# (ring follows Gray-code/serpentine/NodeId-sorted order so consecutive ranks
# are physically local; tree follows a locality-bisected BST).
# ---------------------------------------------------------------------------
def _direct_hop(spec): return 1            # direct-connect: 1 hop to neighbor
def _switched_hop(spec): return 1          # NVSwitch: 1 hop end-to-end
def _leaftier_hop(spec):
    # leafspine/hier3/fattree/dragonfly: ring is NodeId-sorted so same-leaf
    # ranks are contiguous (1 hop); one 2-hop jump per leaf transition.
    leaf = spec.num_leaf or 1
    N = spec.N
    steps = N - 1
    if steps <= 0: return 1
    inter = leaf                       # one inter-leaf jump per leaf
    avg = ((steps - inter) * 1 + inter * 2) / steps
    return max(1.0, avg)


def _railfattree_hop(spec):
    # Count switch hops along the rail-major collective order.  A transfer
    # crosses one switch within a leaf, two within a leaf/spine group, and
    # three when it traverses the core tier.
    rails = spec.rail_count or 8
    enclosures = spec.N // rails
    order = [node * rails + rail
             for rail in range(rails)
             for node in range(enclosures)]
    leaves_per_rail, _, _, cores, _ = _rail_fattree_shape(spec, spec.N)

    def leaf(rank):
        node, rail = divmod(rank, rails)
        return rail * leaves_per_rail + node // (spec.rail_nodes_per_leaf or 32)

    hops = []
    for source, destination in zip(order, order[1:]):
        source_leaf = leaf(source)
        destination_leaf = leaf(destination)
        if source_leaf == destination_leaf:
            hops.append(1.0)
        elif cores and source_leaf // 32 != destination_leaf // 32:
            hops.append(3.0)
        else:
            hops.append(2.0)
    return sum(hops) / len(hops) if hops else 1.0

FAMILIES: Dict[str, Tuple] = {
    # name: (ns3_topology, embedding, is_switched, supports_nvls, feasible, link_techs, hop_fn)
    "ring":            ("ring",              "identity",      False, False, True,  ["direct_electrical"],            _direct_hop),
    "fullmesh":        ("fullmesh",          "identity",      False, False, True,  ["direct_electrical"],            _direct_hop),
    "hypercube":       ("hypercube",         "gray_code",     False, False, True,  ["direct_electrical", "direct_optical"], _direct_hop),
    "3d_torus":        ("torus",             "serpentine",    False, False, False, ["direct_electrical", "direct_optical"], _direct_hop),
    "mesh2d":          ("mesh",              "serpentine",    False, False, True,  ["direct_electrical", "direct_optical"], _direct_hop),
    "2dfullmesh":      ("2dfullmesh",        "snake",         False, False, True,  ["direct_electrical", "direct_optical"], _direct_hop),
    "switched":        ("switched",          "nodeid_sorted", True,  True,  True,  ["xccl_optical"],                  _switched_hop),
    "nvl72":           ("nvl72",             "nodeid_sorted", True,  False, True,  ["xccl_optical"],                  _switched_hop),
    "multiplane":      ("multiplane",        "nodeid_sorted", True,  False, True,  ["xccl_optical"],                  _switched_hop),
    "leafspine":       ("leafspine",         "nodeid_sorted", False, False, True,  ["xccl_optical", "roce_optical"],  _leaftier_hop),
    "3levelhier":      ("3levelhierarchical","nodeid_sorted", False, False, True,  ["xccl_optical", "roce_optical"],  _leaftier_hop),
    "fattree":         ("fattree",           "nodeid_sorted", False, False, True,  ["xccl_optical", "roce_optical"],  _leaftier_hop),
    "railfattree":     ("railfattree",       "rail_major",    False, False, True,  ["xccl_optical", "roce_optical"],  _railfattree_hop),
    "dragonflyplus":   ("dragonflyplus",     "nodeid_sorted", False, False, True,  ["xccl_optical", "roce_optical"],  _leaftier_hop),
    "2dfullmeshclos":  ("2dfullmeshclos",    "nodeid_sorted", False, False, True,  ["xccl_optical", "roce_optical"],  _leaftier_hop),
}
# Note: 14 rows above; the "12 families" in the plan collapses ring+fullmesh as
# trivial direct baselines. We keep all 14 for completeness.


@dataclass
class TopoSpec:
    """One concrete interconnect configuration at N GPUs."""
    spec_id: str
    family: str            # grammar family key (FAMILIES)
    ns3_topology: str = ""      # --topology= value (set by finalize)
    link_tech: str = ""
    N: int = 128
    # Builder params (only the ones relevant to this family are set):
    dims: Tuple[int, ...] = ()            # torus/mesh/2dfullmesh (rows,cols,...)
    hypercube_dims: int = 0
    radix: int = 0                        # fattree
    num_leaf: int = 0                     # leafspine / hier3 / dragonfly groups
    num_spine: int = 0
    num_mid: int = 0                      # hier3 mid-tier
    num_planes: int = 0                   # multiplane / nvl72 switch planes
    num_racks: int = 0                    # 2dfullmeshclos
    rack_rows: int = 0
    rack_cols: int = 0
    links_per_gpu: int = 0
    dfly_routers_per_group: int = 0
    nvl72_gpus_per_group: int = 0
    rail_count: int = 0                       # local GPU ranks / rails
    rail_nodes_per_leaf: int = 0              # enclosures served by one rail leaf
    rail_links_per_spine: int = 0             # parallel links per leaf-spine pair
    rail_num_core: int = 0                    # core switches; zero for two tiers
    # Derived (filled by finalize):
    is_switched: bool = False
    supports_nvls: bool = False
    feasible: bool = True
    embedding: str = ""
    switch_count: int = 0
    bisection_bw_gbps: float = 0.0
    hop_count: float = 1.0
    bw_eff_gbps: float = 0.0

    def finalize(self):
        """Compute derived fields after all builder params are set."""
        f = FAMILIES[self.family]
        self.ns3_topology = f[0]
        self.embedding = f[1]
        self.is_switched = f[2]
        self.supports_nvls = f[3]
        self.feasible = f[4]
        self.switch_count = self._switch_count()
        self.links_per_gpu = self.links_per_gpu or self._default_links_per_gpu()
        self.bisection_bw_gbps = self._bisection_bw()
        self.hop_count = float(f[6](self))
        # Effective per-GPU usable bandwidth (GB/s) = min(links_per_gpu * per-link,
        # bisection cap). per-link BW from link_tech.
        per_link_gbps = LINK_TECH_MAP[self.link_tech]["bw_gbps"]
        self.bw_eff_gbps = min(self.links_per_gpu * per_link_gbps,
                               max(self.bisection_bw_gbps, per_link_gbps))
        return self

    # ---- builder-param derivation ------------------------------------------
    def _switch_count(self) -> int:
        fam = self.family
        if fam in ("ring", "fullmesh", "hypercube", "3d_torus", "mesh2d", "2dfullmesh"):
            return 0  # direct-connect, no switch ASIC
        if fam == "switched":
            return 18  # NVL72-style: 18 NVSwitch ASICs for full non-blocking 128-GPU
        if fam == "nvl72":
            return self.num_planes or 18
        if fam == "multiplane":
            return self.num_planes or 4
        if fam == "leafspine":
            return (self.num_leaf or 8) + (self.num_spine or 8)
        if fam == "3levelhier":
            return (self.num_leaf or 8) + (self.num_mid or 4) + (self.num_spine or 8)
        if fam == "fattree":
            # k-ary fat tree: 5k^2/4 switches for k-ary... approximate by radix.
            k = self.radix or 8
            return max(1, (5 * k * k) // 4)
        if fam == "railfattree":
            _, total_leaves, spines, cores, _ = _rail_fattree_shape(self, self.N)
            return total_leaves + spines + cores
        if fam == "dragonflyplus":
            return (self.num_leaf or 8) + (self.num_leaf or 8) * 4  # leaf + global
        if fam == "2dfullmeshclos":
            return (self.num_racks or 8) + (self.rack_rows or 4)
        return 0

    def _default_links_per_gpu(self) -> int:
        fam = self.family
        if fam == "hypercube": return self.hypercube_dims or 7
        if fam == "3d_torus": return 6           # 3 dims * 2 directions
        if fam == "mesh2d": return 4             # 2 dims * 2 directions (no wrap)
        if fam == "2dfullmesh": return 4
        if fam == "ring": return 2
        if fam == "fullmesh": return self.N - 1  # full mesh: N-1 links/GPU
        if fam == "switched": return 18
        if fam == "nvl72": return self.num_planes or 18
        if fam == "multiplane": return (self.num_planes or 4) * 4
        if fam == "leafspine": return 8
        if fam == "3levelhier": return 8
        if fam == "fattree": return self.radix or 8
        if fam == "railfattree": return 1
        if fam == "dragonflyplus": return 8
        if fam == "2dfullmeshclos": return 8
        return 1

    def _bisection_bw(self) -> float:
        """Bisection bandwidth (GB/s) — used for cost-goodput sanity, not the
        per-step surrogate (which uses bw_eff_gbps + hop_count)."""
        per_link = LINK_TECH_MAP[self.link_tech]["bw_gbps"]
        lpg = self.links_per_gpu
        fam = self.family
        if fam in ("fullmesh", "switched", "nvl72", "railfattree"):
            return self.N * lpg * per_link / 2  # full bisection
        if fam == "hypercube":
            return self.N * per_link / 2         # 1 dim cut
        if fam in ("3d_torus", "mesh2d", "2dfullmesh"):
            # cut across the smallest dimension
            dims = [d for d in self.dims if d > 0] or [1]
            smallest = min(dims)
            cut_gpus = self.N // smallest
            return cut_gpus * per_link / 2
        if fam == "ring":
            return 2 * per_link                  # cut = 2 links
        # leafspine/hier/fattree/dragonfly/clos: bisection ~ inter-tier links
        return self.N * lpg * per_link / 8

    # ---- export ------------------------------------------------------------
    def to_ns3_cli(self, num_gpus: Optional[int] = None) -> List[str]:
        """Emit the ns-3 CLI args for this spec (topology + params + link tech).

        Returns just the topology-specific args; callers add collective/size/
        protocol/reliability flags. Mirrors dse_config_to_ns3.config_to_cli.
        """
        N = num_gpus or self.N
        lt = LINK_TECH_MAP[self.link_tech]
        args = [f"--numGpus={N}", f"--topology={self.ns3_topology}",
                f"--fabricType={lt['fabric']}", f"--bandwidth={lt['bw_gbps']}",
                f"--linkDataRate={lt['link_data_rate']}"]
        if self.links_per_gpu and self.ns3_topology in (
                "switched", "leafspine", "nvl72", "multiplane",
                "3levelhierarchical", "dragonflyplus"):
            args.append(f"--linksPerGpu={self.links_per_gpu}")
        if self.family == "hypercube":
            args.append(f"--hypercubeDims={self.hypercube_dims or 7}")
        elif self.family == "3d_torus":
            d = self.dims
            args += [f"--torusDimX={d[0]}", f"--torusDimY={d[1]}", f"--torusDimZ={d[2]}"]
        elif self.family == "mesh2d":
            args += [f"--meshRows={self.dims[0]}", f"--meshCols={self.dims[1]}"]
        elif self.family == "2dfullmesh":
            args += [f"--rackRows={self.dims[0]}", f"--rackCols={self.dims[1]}"]
        elif self.family == "fattree":
            args.append(f"--fattreeRadix={self.radix}")
        elif self.family == "railfattree":
            args += [f"--railCount={self.rail_count or 8}",
                     f"--railNodesPerLeaf={self.rail_nodes_per_leaf or 32}",
                     f"--railSpineSwitches={self.num_spine}",
                     f"--railCoreSwitches={self.rail_num_core}",
                     f"--railLinksPerSpine={self.rail_links_per_spine}"]
        elif self.family == "leafspine":
            args += [f"--leafSwitches={self.num_leaf}", f"--spineSwitches={self.num_spine}"]
        elif self.family == "3levelhier":
            args += [f"--leafSwitches={self.num_leaf}",
                     f"--hier3MidSwitches={self.num_mid}",
                     f"--spineSwitches={self.num_spine}"]
        elif self.family == "nvl72":
            args += [f"--nvl72SwitchPlanes={self.num_planes}",
                     f"--nvl72GpusPerGroup={self.nvl72_gpus_per_group}"]
        elif self.family == "multiplane":
            args.append(f"--multiPlaneCount={self.num_planes}")
        elif self.family == "dragonflyplus":
            args += [f"--dflyGroups={self.num_leaf}",
                     f"--dflyRoutersPerGroup={self.dfly_routers_per_group}"]
        elif self.family == "2dfullmeshclos":
            args += [f"--numRacks={self.num_racks}",
                     f"--rackRows={self.rack_rows}", f"--rackCols={self.rack_cols}"]
        # ring/fullmesh need no extra params.
        return args

    def to_cost(self, vc_buf_kb: int = 1024, llr_buf_mb: int = 256,
                rel_policy: str = "fec_llr_retry", pd_pct: int = 0,
                bufferless: bool = True) -> Dict[str, float]:
        """Marginal hardware cost reusing dse_cost_model unit costs.

        Computed from the spec's own switch/link counts (so parametric
        variations differ), not the fixed TOPO_PARAMS table.
        """
        sw_type = "nvsw" if self.is_switched else (
            "eth" if self.switch_count > 0 else "none")
        sw_cost = self.switch_count * SWITCH_COST[sw_type]
        medium = "optical" if "optical" in self.link_tech else "electrical"
        if self.family == "railfattree":
            _, total_leaves, spines, cores, links_per_spine = _rail_fattree_shape(
                self, self.N)
            total_links = self.N + total_leaves * spines * links_per_spine
            if cores:
                total_links = self.N + total_leaves * 32 + spines * 32
        else:
            total_links = self.N * self.links_per_gpu // 2  # undirected
        link_cost = total_links * LINK_COST[medium]
        vc_cost = self.N * (vc_buf_kb / 1024.0) * VC_BUF_COST_PER_MB
        if bufferless or "retry" not in rel_policy:
            llr_cost = 0.0
        else:
            llr_cost = self.switch_count * llr_buf_mb * LLR_BUF_COST_PER_MB
        pd_cost = (pd_pct / 100.0) * 2500.0
        total = sw_cost + link_cost + vc_cost + llr_cost + pd_cost
        return {"switch_cost": sw_cost, "link_cost": link_cost,
                "vc_buf_cost": vc_cost, "llr_buf_cost": llr_cost,
                "pd_mem_cost": pd_cost, "total": total}

    def to_surrogate_params(self) -> Dict[str, float]:
        """Derived fields consumed by the topology-aware surrogate (G3.4)."""
        from algo_selector import step_count
        return {
            "spec_id": self.spec_id, "family": self.family,
            "link_tech": self.link_tech, "N": self.N,
            "hop_count": self.hop_count,
            "bw_eff_gbps": self.bw_eff_gbps,
            "bisection_bw_gbps": self.bisection_bw_gbps,
            "switch_count": self.switch_count,
            "links_per_gpu": self.links_per_gpu,
            "is_switched": int(self.is_switched),
            "supports_nvls": int(self.supports_nvls),
            "feasible": int(self.feasible),
            "embedding": self.embedding,
            "step_count_ring": step_count("ring", self.N),
            "step_count_tree": step_count("tree", self.N),
            "step_count_nvls": step_count("nvls", self.N),
        }

    def rebuild_at(self, N):
        """Return a NEW TopoSpec of this spec's family at N GPUs (params
        recomputed for N), or None if the family cannot build at N.

        Used by the PD-split sweep runner to run a pool's collective at the
        pool size (N=P or N=D) on the same interconnect family. Mirrors the
        per-family params fns in dse_config_to_ns3.py. Returns None for
        infeasible (family, N) combos (e.g. hypercube at non-power-of-2);
        the caller skips + logs those.
        """
        import math as _m
        fam = self.family
        kw = {}
        # Trivial families (no params): always feasible.
        if fam in ("ring", "fullmesh"):
            pass
        elif fam == "hypercube":
            if N & (N - 1) != 0:  # not a power of 2
                return None
            kw["hypercube_dims"] = max(1, int(_m.log2(N)))
        elif fam in ("3d_torus", "mesh2d", "2dfullmesh"):
            pairs = _factor_pairs(N)
            if fam == "3d_torus":
                # Use a balanced three-dimensional factorization when one
                # exists.  Small communicator sizes may not have three
                # factors greater than one; unit dimensions still describe a
                # valid lower-dimensional torus and keep the node count exact.
                triples = _factor_triples(N)
                if triples:
                    kw["dims"] = min(
                        triples,
                        key=lambda values: (
                            max(values) - min(values),
                            max(values),
                        ),
                    )
                elif pairs:
                    r, c = min(
                        pairs,
                        key=lambda values: (values[1] - values[0], values[1]),
                    )
                    kw["dims"] = (1, r, c)
                else:
                    kw["dims"] = (1, 1, N)
            elif pairs:
                r, c = min(
                    pairs,
                    key=lambda values: (values[1] - values[0], values[1]),
                )
                kw["dims"] = (r, c)
            else:
                kw["dims"] = (1, N)
        elif fam == "switched":
            kw["links_per_gpu"] = self.links_per_gpu or 18
        elif fam == "nvl72":
            # group structure must divide N; keep planes, shrink group
            planes = self.num_planes or 18
            if N < 8:
                gpus_per_group = N
            elif N % 8 == 0:
                gpus_per_group = 8
            elif N % planes == 0:
                gpus_per_group = max(1, N // planes)
            else:
                return None
            kw["num_planes"] = planes
            kw["nvl72_gpus_per_group"] = gpus_per_group
            kw["links_per_gpu"] = planes
        elif fam == "multiplane":
            mp = self.num_planes or 4
            kw["num_planes"] = mp
            # links_per_gpu = mp planes x 4 links/plane; per-GPU port count is
            # constant across N (NOT topology-size-dependent). The old formula
            # (mp*4*N)//(self.N*mp) scaled it down to 4 at the base scale, violating
            # links_per_gpu >= num_planes -> NS_ABORT in BuildMultiPlane.
            kw["links_per_gpu"] = mp * 4
        elif fam == "leafspine":
            # Keep the leaf count a divisor of the communicator.  The
            # 128-GPU variants use eight leaves; smaller communicators use the
            # largest available divisor up to eight.
            leaf_limit = min(8, N)
            leaf = max(
                value for value in range(1, leaf_limit + 1) if N % value == 0
            )
            kw.update(
                num_leaf=leaf,
                num_spine=max(1, self.num_spine or 8),
                links_per_gpu=self.links_per_gpu or 8,
            )
        elif fam == "3levelhier":
            leaf_limit = min(8, N)
            leaf = max(
                value for value in range(1, leaf_limit + 1) if N % value == 0
            )
            mid_limit = min(leaf, self.num_mid or 4)
            mid = max(
                value for value in range(1, mid_limit + 1) if leaf % value == 0
            )
            kw.update(
                num_leaf=leaf,
                num_mid=mid,
                num_spine=max(1, self.num_spine or 8),
                links_per_gpu=self.links_per_gpu or 8,
            )
        elif fam == "fattree":
            kw["radix"] = self.radix or 8
        elif fam == "railfattree":
            rail_limit = min(8, N)
            rails = max(
                value for value in range(1, rail_limit + 1) if N % value == 0
            )
            nodes_per_leaf = self.rail_nodes_per_leaf or 32
            spines, links_per_spine, cores = _rail_fattree_defaults(
                N, rails, nodes_per_leaf)
            kw.update(rail_count=rails,
                      rail_nodes_per_leaf=nodes_per_leaf,
                      num_spine=spines,
                      rail_num_core=cores,
                      rail_links_per_spine=links_per_spine,
                      links_per_gpu=1)
        elif fam == "dragonflyplus":
            routers_per_group = self.dfly_routers_per_group or 1
            if N % routers_per_group:
                # A smaller communicator may not be divisible by the
                # source topology's routers-per-group.  One router per group
                # is the exact executable reduction in that case.
                routers_per_group = 1
            group_limit = max(1, N // routers_per_group)
            groups = max(
                value
                for value in range(1, group_limit + 1)
                if N % (value * routers_per_group) == 0
            )
            kw.update(
                num_leaf=groups,
                dfly_routers_per_group=routers_per_group,
                links_per_gpu=self.links_per_gpu or 8,
            )
        elif fam == "2dfullmeshclos":
            # A communicator smaller than one modeled rack is represented as
            # one rack with an exact factorization.
            rack_capacity = (self.rack_rows or 1) * (self.rack_cols or 1)
            if N % rack_capacity == 0:
                kw.update(
                    num_racks=N // rack_capacity,
                    rack_rows=self.rack_rows or 1,
                    rack_cols=self.rack_cols or 1,
                )
            else:
                pairs = _factor_pairs(N)
                if pairs:
                    rows, cols = min(
                        pairs,
                        key=lambda values: (values[1] - values[0], values[1]),
                    )
                else:
                    rows, cols = 1, N
                kw.update(num_racks=1, rack_rows=rows, rack_cols=cols)
            kw["links_per_gpu"] = self.links_per_gpu or 8
        else:
            return None
        s = TopoSpec(spec_id=f"{self.spec_id}_n{N}", family=fam,
                     link_tech=self.link_tech, N=N, **kw)
        s.finalize()
        return s


# ---------------------------------------------------------------------------
# Enumeration of the 128-GPU spec space.
# ---------------------------------------------------------------------------
def _factor_pairs(n):
    """Distinct (a,b) factor pairs of n with a<=b, a>=2."""
    return [(a, n // a) for a in range(2, n + 1) if n % a == 0 and a <= n // a]


def _factor_triples(n):
    """Distinct sorted triples (a<=b<=c, each>=2) with a*b*c==n."""
    out = []
    for a in range(2, int(n ** (1 / 3)) + 2):
        if n % a: continue
        rem = n // a
        for b in range(a, int(math.isqrt(rem)) + 2):
            if rem % b: continue
            c = rem // b
            if b <= c:
                out.append((a, b, c))
    return out


def enumerate_128gpu_specs(N: int = 128) -> List[TopoSpec]:
    """Enumerate all TopoSpecs at N GPUs across the 14 families."""
    specs: List[TopoSpec] = []
    sid = 0

    def add(family, link_tech, **kw):
        nonlocal sid
        s = TopoSpec(spec_id=f"topo{sid:03d}", family=family,
                     link_tech=link_tech, N=N, **kw)
        s.finalize()
        specs.append(s)
        sid += 1

    # 1. ring
    add("ring", "direct_electrical")
    # 2. fullmesh
    add("fullmesh", "direct_electrical")
    # 3. hypercube (2^7=128)
    for lt in ("direct_electrical", "direct_optical"):
        add("hypercube", lt, hypercube_dims=7)
    # 4. 3d_torus (factor triples of 128, each dim>=2)
    for d in _factor_triples(N):
        for lt in ("direct_electrical", "direct_optical"):
            add("3d_torus", lt, dims=d)
    # 5. mesh2d (factor pairs)
    for r, c in _factor_pairs(N):
        for lt in ("direct_electrical", "direct_optical"):
            add("mesh2d", lt, dims=(r, c))
    # 6. 2dfullmesh (factor pairs)
    for r, c in _factor_pairs(N):
        for lt in ("direct_electrical", "direct_optical"):
            add("2dfullmesh", lt, dims=(r, c))
    # 7. switched (NVSwitch, link counts)
    for lpg in (8, 16, 18):
        add("switched", "xccl_optical", links_per_gpu=lpg)
    # 8. nvl72
    for planes, gpg in [(18, 8), (18, 16), (6, 16)]:
        add("nvl72", "xccl_optical", num_planes=planes, nvl72_gpus_per_group=gpg,
            links_per_gpu=planes)
    # 9. multiplane
    for mp in (2, 4, 8):
        add("multiplane", "xccl_optical", num_planes=mp, links_per_gpu=mp * 4)
    # 10. leafspine
    for leaf, spine in [(8, 8), (4, 16), (16, 4)]:
        for lt in ("xccl_optical", "roce_optical"):
            add("leafspine", lt, num_leaf=leaf, num_spine=spine, links_per_gpu=8)
    # 11. 3levelhierarchical
    for leaf, mid, spine in [(8, 4, 8), (4, 4, 16), (8, 8, 4)]:
        for lt in ("xccl_optical", "roce_optical"):
            add("3levelhier", lt, num_leaf=leaf, num_mid=mid, num_spine=spine,
                links_per_gpu=8)
    # 12. fattree (radix 8 -> 128 endpoints; also 16)
    for rad in (8, 16):
        for lt in ("xccl_optical", "roce_optical"):
            add("fattree", lt, radix=rad, links_per_gpu=rad)
    # 13. dragonflyplus
    for grp, rpg in [(8, 1), (4, 2), (16, 1)]:
        for lt in ("xccl_optical", "roce_optical"):
            add("dragonflyplus", lt, num_leaf=grp, dfly_routers_per_group=rpg,
                links_per_gpu=8)
    # 14. 2dfullmeshclos
    for racks, (rr, rc) in [(8, (4, 4)), (4, (2, 2)), (16, (4, 4))]:
        for lt in ("xccl_optical", "roce_optical"):
            add("2dfullmeshclos", lt, num_racks=racks, rack_rows=rr, rack_cols=rc,
                links_per_gpu=8)
    # 15. Rail-optimized fat tree. Keep this last so the IDs of all previously
    # enumerated 128-GPU configurations remain stable.
    if N % 8 == 0:
        rail_spines, rail_parallel, rail_cores = _rail_fattree_defaults(N)
        add("railfattree", "xccl_optical", rail_count=8,
            rail_nodes_per_leaf=32, num_spine=rail_spines,
            rail_num_core=rail_cores,
            rail_links_per_spine=rail_parallel, links_per_gpu=1)
    return specs


def parametric_specs(N: int) -> List[TopoSpec]:
    """Scale-aware MULTI-VARIANT enumeration: several buildable parametric
    variants per family at N GPUs (vs enumerate_128gpu_specs which is N=128-
    hardcoded, vs rebuild_at which collapses each family to ONE variant).

    Each variant differs in a physical lever (mesh aspect ratio, switch radix,
    leaf/spine counts, plane count, links-per-gpu) -> distinct bisection/hop/
    cost -> DENSER Pareto fronts. Non-buildable variants are filtered by the
    runner's smoke-build step (returned rc!=ok), so a wrong guess here is a
    wasted cheap smoke run, not bogus data.
    """
    specs: List[TopoSpec] = []
    sid = 0

    def add(family, link_tech, **kw):
        nonlocal sid
        s = TopoSpec(spec_id=f"par{sid:03d}", family=family,
                     link_tech=link_tech, N=N, **kw)
        s.finalize()
        specs.append(s)
        sid += 1

    # 1. ring (trivial, 1 variant)
    add("ring", "direct_electrical")
    # 2. fullmesh (trivial, 1 variant; runner skips at N>=512)
    add("fullmesh", "direct_electrical")
    # 3. hypercube (power-of-2 only; dims=log2 N fixed by connectivity)
    if N & (N - 1) == 0:
        add("hypercube", "direct_electrical", hypercube_dims=int(math.log2(N)))
    # 4. mesh2d: ALL factor pairs (aspect ratio -> bisection spread)
    for r, c in _factor_pairs(N):
        add("mesh2d", "direct_electrical", dims=(r, c))
    # 5. 2dfullmesh: ALL factor pairs (many time out -> stuck filter, but the
    #    square-ish ones complete and give real points)
    for r, c in _factor_pairs(N):
        add("2dfullmesh", "direct_electrical", dims=(r, c))
    # 6. A single NVSwitch fabric is not a scale-out topology.  Keep it in the
    # hardware-calibration grammar, but never enumerate it for superpod scale.
    if N <= NVLINK_FABRIC_MAX_GPUS:
        for lpg in (8, 16, 18):
            add("switched", "xccl_optical", links_per_gpu=lpg)
    # Rail-optimized fat tree.  Eight local GPU ranks form eight rails; each
    # leaf serves at most 32 enclosures and retains 32 spine-facing links.
    if N % 8 == 0:
        rail_spines, rail_parallel, rail_cores = _rail_fattree_defaults(N)
        add("railfattree", "xccl_optical", rail_count=8,
            rail_nodes_per_leaf=32, num_spine=rail_spines,
            rail_num_core=rail_cores,
            rail_links_per_spine=rail_parallel, links_per_gpu=1)
    # 7. nvl72: switch-plane count sweep (gpg derived per rebuild_at rule)
    for planes in (6, 18):
        if N % planes and N % 8:
            continue   # rebuild_at infeasibility rule
        gpg = N // planes if N % planes == 0 else 8
        add("nvl72", "xccl_optical", num_planes=planes,
            nvl72_gpus_per_group=gpg, links_per_gpu=planes)
    # 8. multiplane: plane-count sweep (lpg = mp*4 constant)
    for mp in (2, 4, 8):
        add("multiplane", "xccl_optical", num_planes=mp, links_per_gpu=mp * 4)
    # 9. fattree: radix sweep (overprovisioned radix builds fine; capacity
    #    mis-sized ones filtered by smoke)
    for rad in (8, 16, 32):
        add("fattree", "xccl_optical", radix=rad, links_per_gpu=rad)
    # 10. leafspine: num_leaf divisor of N (lpg=num_leaf keeps lpg>=num_leaf and
    #     lpg%num_leaf==0 per the leafspine builder constraint); num_spine sweep
    #     gives cost-axis spread (switch_count = num_leaf+num_spine).
    for leaf in [d for d in (8, 16, 32) if N % d == 0]:
        for spine in (4, 8, 16, 32):
            add("leafspine", "xccl_optical", num_leaf=leaf, num_spine=spine,
                links_per_gpu=leaf)
    # 11. 3levelhier: num_leaf divisor; mid/spine sweep (builds reliably at
    #     1024 -> densifies the scale-sparsest figure)
    for leaf in [d for d in (8, 16, 32) if N % d == 0]:
        for mid in (4, 8):
            for spine in (8, 16, 32):
                add("3levelhier", "xccl_optical", num_leaf=leaf, num_mid=mid,
                    num_spine=spine, links_per_gpu=leaf)
    # 12. dragonflyplus: groups divisor of N, rpg sweep (lpg>=totalLeaf=groups*rpg)
    for grp in [d for d in (8, 16, 32) if N % d == 0]:
        for rpg in (1, 2):
            tot = grp * rpg
            add("dragonflyplus", "xccl_optical", num_leaf=grp,
                dfly_routers_per_group=rpg, links_per_gpu=max(8, tot))
    # 13. 2dfullmeshclos: rack-grid sweep (N must divide rack grid product)
    for (rr, rc) in [(4, 4), (8, 8), (2, 8), (4, 8)]:
        g = rr * rc
        if N % g:
            continue
        add("2dfullmeshclos", "xccl_optical", num_racks=N // g,
            rack_rows=rr, rack_cols=rc, links_per_gpu=8)
    return specs


def write_csv(path: str, specs: List[TopoSpec]):
    rows = [s.to_surrogate_params() | {"cost_total": s.to_cost()["total"]}
            for s in specs]
    if not rows:
        return
    cols = list(rows[0].keys())
    with open(path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=cols)
        w.writeheader()
        for r in rows:
            w.writerow(r)


if __name__ == "__main__":
    specs = enumerate_128gpu_specs(128)
    print(f"Enumerated {len(specs)} TopoSpecs at N=128 across {len(FAMILIES)} families:")
    from collections import Counter
    c = Counter(s.family for s in specs)
    for fam, n in sorted(c.items()):
        feasible = sum(1 for s in specs if s.family == fam and s.feasible)
        print(f"  {fam:18s} {n:3d} specs ({feasible} feasible)")
    print(f"\nFeasible total: {sum(1 for s in specs if s.feasible)}/{len(specs)}")
    out = os.path.join(_HERE, "..", "..", "configs", "dse", "topo_specs.csv")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    write_csv(out, specs)
    print(f"Wrote {out}")
    # Smoke: print one spec's CLI + cost
    s = specs[0]
    print("\nSample spec:", s.spec_id, s.family, s.link_tech)
    print("  CLI:", " ".join(s.to_ns3_cli()))
    print("  cost:", s.to_cost()["total"])
    print("  surrogate:", {k: s.to_surrogate_params()[k] for k in
          ("hop_count", "bw_eff_gbps", "switch_count", "embedding")})
