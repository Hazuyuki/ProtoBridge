# ProtoBridge Latency Surrogate — User Guide

This guide shows how to **use** the latency surrogate to estimate collective
communication latency in microseconds. It is the fast screening model that
filters the broad design space before selected candidates are simulated in
full ns-3. For the full mathematical derivation, see
[`WHITEBOX_SURROGATE.md`](WHITEBOX_SURROGATE.md); for how the H200 anchor numbers
were measured, see [`../../doc/CALIBRATION.md`](../../doc/CALIBRATION.md).

The model predicts the latency of one collective operation — AllReduce,
AllGather, AlltoAll, or point-to-point Send-Recv — scheduled as ring / tree /
NVLS / full-mesh. The **collective name and the algorithm together** select the
step model:
pass `collective=` to `predict()` (`'allreduce'` / `'allgather'` /
`'alltoall'` / `'sendrecv'`). It is a **required** argument — not inferred from
the algorithm, so the caller must name the operation explicitly. It is a
first-principles physical derivation fitted to ns-3, not a one-sided bound:
ring sits below ns-3 in the calibrated range, while tree can sit ABOVE it
(e.g. +15% at N=4/D=256 MiB — see [CALIBRATION.md](../../doc/CALIBRATION.md)).
Use it for **ordering and trend analysis across a design space**, not for
absolute single-point prediction.

---

## 1. 30-second quickstart

```python
from surrogate.theory.whitebox_surrogate_v2 import make_surrogate_from_wire

# Build a surrogate for H200 NVLink4: 25 GB/s per link, 18 links per GPU.
s = make_surrogate_from_wire(b_link_bytes_per_us=25000, num_lanes=18, algo="ring")

# Predict mean latency of a 256 MiB ring AllReduce on 8 GPUs.
lat_us = s.predict(256 * 1024 * 1024, 8, "ring", credits=128, ber=0, collective="allreduce")
# lat_us == 1325.5  (the H200 reference point)
```

That is the whole core usage: **build** a surrogate, then **query** it.
Everything else on this page is optional.

---

## 2. The input model — three stages

| Stage | What you give | Required? |
|---|---|---|
| **① Build** a surrogate | per-link wire rate, lane count, algorithm | yes (or use an H200 preset) |
| **② Query** a latency | data size, GPU count, algorithm, credits, `collective` | yes (5 inputs) |
| **③ Topology** (optional) | a fabric name + physical numbers | no — omit = ideal NVSwitch |

Omitting stage ③ gives the **default fabric**: an ideal non-blocking NVSwitch.
That default is byte-identical to the model before topology support was added,
so the H200 calibration is unchanged. You only need ③ when modeling a real
fabric whose bisection bandwidth or path length differs from ideal NVSwitch.

---

## 3. Build a surrogate

### Recommended: single-wire-rate factory

```python
make_surrogate_from_wire(b_link_bytes_per_us, num_lanes, algo,
                         eta_ring=0.40, eta_tree=0.61, eta_nvls=1.20,
                         eta_fullmesh=1.0, **overrides)
```

Derives every schedule bandwidth from **one** physical input — the per-GPU
NVLink aggregate `B_agg = num_lanes * b_link` — plus three N-independent
efficiency factors. No per-bandwidth calibration needed.

| Input | Meaning | H200 NVLink4 |
|---|---|---|
| `b_link_bytes_per_us` | single NVLink unidirectional wire rate | `25000` (25 GB/s) |
| `num_lanes` | NVLink links per GPU | `18` |
| `algo` | `ring` / `tree` / `nvls` / `fullmesh` (`fm`) | — |
| `eta_ring` / `eta_tree` / `eta_nvls` / `eta_fullmesh` | efficiency factors (defaulted) | 0.40 / 0.61 / 1.20 / 1.0 |

Derived bandwidths land within ~2% of the calibrated H200 values (see §11 of
`WHITEBOX_SURROGATE.md`). This is the entry point for any new platform where
only the per-link wire rate is known.

### H200 measured presets

```python
from surrogate.theory.whitebox_surrogate_v2 import make_h200_ring_theory, make_h200_tree_theory, make_h200_nvls_theory, make_h200_fullmesh_theory
s = make_h200_ring_theory()      # ships the exact calibrated H200 ring numbers
```

Use these when you want the measured H200 values rather than the wire-rate
derivation. They accept `**overrides` too (any `__init__` keyword). For
AlltoAll and Send-Recv use `make_h200_alltoall_theory()` (SIMPLE 46 µs
startup) and `make_h200_sendrecv_theory()` (15 µs startup); both reuse the
H200 ring anchor with a protocol-appropriate startup. For the full-mesh
algorithm use `make_h200_fullmesh_theory()` — it is **ns-3-calibrated** (not a
wire-rate derivation); see recipe M.

### Custom hardware

Pass `**overrides` to either factory to replace any anchor value, or construct
directly for a non-H200 platform:

```python
from surrogate.theory import whitebox_surrogate_v2 as wb
s = wb.make_h200_ring_theory(num_lanes=8, startup_us=7)          # H200 ring, 8 lanes, 7us startup
s = wb.TheoryDerivedSurrogate(ring_bw_bytes_per_us=120000, link_latency_us=0.3,
                              credit_bdp_ring_packets=60, startup_us=10.0, num_lanes=8)
```

The full `__init__` parameter list (21 knobs, all defaulted) is in the
`TheoryDerivedSurrogate` class docstring. The parameters that move the clean
(`ber=0`) mean latency:

| Parameter | Units | Affects | H200 ring |
|-----------|-------|---------|-----------|
| `ring_bw_bytes_per_us` | bytes/µs | ring serialization | 177000 |
| `tree_bw_per_level_bytes_per_us` | bytes/µs | tree GPU↔NVSwitch trunk (ns-3-calibrated) | 275400 |
| `nvls_bw_bytes_per_us` | bytes/µs | NVLS serialization | 535500 |
| `fullmesh_bw_bytes_per_us` | bytes/µs | full-mesh serialization (ns-3-calibrated) | 449890 |
| `link_latency_us` | µs (400 ns = 0.4) | ring/nvls/fullmesh propagation | 0.4 |
| `t_min_switch_us` | µs | tree fixed (NVSwitch pipeline cost) | 4.10 |
| `credit_bdp_ring_packets` | packets | ring credit pressure Φ_credit | 91.5 |
| `credit_bdp_tree_packets` | packets | tree credit pressure Φ_credit | 35.5 |
| `startup_us` / `nvls_startup_us` | µs | protocol startup (fixed) | 15.0 / 23.0 |

The remaining knobs (`packet_bw_bytes_per_us`, `spray_chunk_bytes`,
`bulk_chunk_bytes`, `num_lanes`, `fec_*`, `retry_source_*`) only take effect
under link degradation (ber>0) or with FEC/LLR enabled. The bandwidth
parameters are *effective* transfer rates already incorporating protocol
efficiency and spray (not raw per-link × lanes products); the H200 values are
the calibrated anchor, so custom values yield a physically-plausible but
uncalibrated lower bound.

---

## 4. Query a latency — `predict()`

```python
s.predict(data_bytes, num_gpus, algo, credits, ber=0, ..., topology=None, *, collective)
```

### Required (5)

| Input | Meaning |
|---|---|
| `data_bytes` | operation data size in bytes |
| `num_gpus` | participating GPU count N |
| `algo` | `ring` / `tree` / `nvls` / `fullmesh` |
| `credits` | flow-control credit count |
| `collective` | `'allreduce'` / `'allgather'` / `'alltoall'` / `'sendrecv'` (keyword-only; not inferred from `algo`) |

### Optional, grouped by mechanism (all defaulted)

| Group | Parameters |
|---|---|
| Reliability / retransmission | `ber`, `fec_enabled`, `llr_enabled`, `fec_n`/`fec_k`/`fec_t`, `retry_mode` (`'gobackn'`/`'sack'`), `retry_buffer_entries`/`retry_buffer_bytes`, `retry_limit`, `strict_reliability` |
| Optical links | `optical_fraction`, `optical_hops` |
| Contention | `competing_bw` (traffic on the shared bottleneck) |
| Memory | `mem_bw` |
| Startup | `startup_us` |
| Ablation switches | `include_credit`, `include_fec`, `include_retransmission`, `include_contention` |
| Topology | `topology` (see §5) |

For the common case — clean ideal fabric, `ber=0` — you usually pass only the
five required inputs (`data_bytes`, `num_gpus`, `algo`, `credits`,
`collective`). The rest engage physical mechanisms only when set.

### Collective — AlltoAll and Send-Recv

The algorithm alone no longer fixes the collective. `collective=` names the
operation and, with `algo`, fixes the step count and per-step volume. The
mapping (full formulas in §6 of `WHITEBOX_SURROGATE.md`):

| `collective` | `algo` | steps | per-step volume | per-GPU volume |
|---|---|---|---|---|
| `allreduce` | `ring` | `2(N-1)` | `D(N-1)/N / steps` | `D(N-1)/N` |
| `allreduce` | `tree` | `2⌊log₂N⌋` | `α(N)·D/steps` (α(N)=1.0/2.0/3.0 structural trunk load: single edge / root with two children / interior node) | `α(N)·D` per trunk |
| `allgather` | `nvls` | `2` | `D/N` | `D` (multicast) |
| `allreduce` | `fullmesh` | `2` | `D/N` | `D/N` per peer (N−1 concurrent) |
| `allgather` | `fullmesh` | `1` | `D/N` | `D/N` per peer (N−1 concurrent) |
| `alltoall` | `ring` / `collnetdirect` | `N-1` | `D/N` | `D(N-1)/N` |
| `alltoall` | `tree` | `⌈log₂N⌉` | `D(N-1)/N / steps` | `D(N-1)/N` |
| `sendrecv` | any | `1` | `D` | `D` (single hop) |

Full-mesh is the one algorithm where `N` does **not** scale the step volume:
the `N-1` peer sends are **concurrent** on dedicated per-pair bundles (one
phase ships `D/N` per bundle), not `2(N-1)` sequential hops like ring. AR = 2
phases (barrier-gated reduce-scatter + all-gather), AG = 1; `fullmesh_bw` is
ns-3-calibrated (recipe M) — structurally it is NVLS **without** the N-fold
serialization factor.

AlltoAll moves `D(N-1)/N` per GPU across `N-1` partner phases (ring) or
`⌈log₂N⌉` recursive-doubling rounds (tree) — the volume correction the DSE's
analytical model describes but does not apply. Its per-step serialization uses
the per-link ring rate; the cross-section **contention** (every GPU talks to
every other) is captured by the **topology bisection cap** (§5), so without a
topology AlltoAll is an optimistic lower bound. Send-Recv is one point-to-point
hop: `steps=1`, full payload, no pipeline credit pressure.

```python
from surrogate.theory.whitebox_surrogate_v2 import make_h200_alltoall_theory
s = make_h200_alltoall_theory()                         # 46 µs SIMPLE startup
# 256 MiB AlltoAll on 8 GPUs: 7 partner phases, D/N per phase.
s.predict(256 * 1024 * 1024, 8, "ring", credits=128, ber=0, collective="alltoall")

# Send-Recv: one hop, latency = startup + D/ring_bw + link_lat.
s.predict(256 * 1024 * 1024, 8, "ring", credits=128, ber=0, collective="sendrecv")
```

`collective` is required — omitting it raises `TypeError`, and an unknown
name raises `ValueError`. It is never inferred from `algo`.

### Tail latency — `predict_tail()`

Same signature plus `percentile=99` (drops the `include_*` ablation switches):

```python
p99 = s.predict_tail(256 * 1024 * 1024, 8, "ring", credits=128, ber=0, collective="allreduce", percentile=99)
```

---

## 5. Topology (optional) — model a real fabric

By default the model assumes ideal non-blocking NVSwitch (infinite bisection,
1 hop). Pass a `topology=` descriptor to model how a **real** fabric influences
the two parameters topology actually affects: a **bisection-bandwidth cap** on
the schedule serialization rate (`min(algo_bw, bisection)`) and **hop-count
propagation** (`(hop-1) * link_latency` added per step). Three ways to supply
it, simplest first:

### ① Name + physical numbers (recommended)

```python
from surrogate.theory.whitebox_surrogate_v2 import make_topology_from_family

# 8-GPU ring fabric: bisection = 2 links x 25 GB/s = 50 GB/s, hop = 1.
t = make_topology_from_family("ring", N=8, per_link_gbps=25)
s.predict(256 * 1024 * 1024, 8, "ring", credits=128, ber=0, collective="allreduce", topology=t)   # ~4718 us
```

`make_topology_from_family(family, N, links_per_gpu=None, per_link_gbps=None, **params)`
applies the per-family bisection + hop formulas copied from ProtoBridge's DSE
topology grammar, so a **name** + physical numbers yield a `Topology`. Supports
15 families: `ring`, `fullmesh`, `hypercube`, `3d_torus`, `mesh2d`,
`2dfullmesh`, `switched`, `nvl72`, `multiplane`, `leafspine`, `3levelhier`,
`fattree`, `railfattree`, `dragonflyplus`, `2dfullmeshclos`. Family-specific
builder params (`dims`, `radix`, `num_leaf`, `num_planes`, `rail_*`, ...) are
optional kwargs.

> **Unit convention — important.** `per_link_gbps` is the per-link
> **unidirectional** wire rate in GB/s, i.e. `b_link_bytes_per_us / 1000`
> (25 for H200 NVLink4). It must match the `b_link` you used to build the
> surrogate, because the bisection caps the surrogate's own `b_link`-derived
> bandwidth. Non-blocking NVSwitch families (`switched` / `nvl72` /
> `multiplane`) never cap the rate, so `per_link_gbps` may be omitted for them
> (the result equals the ideal fabric).

### ② Hand-computed two numbers

```python
from surrogate.theory.whitebox_surrogate_v2 import make_topology
t = make_topology(bisection_gbps=50, hop_count=1)        # 50 GB/s bisection, 1 hop
```

### ③ Raw dataclass

```python
from surrogate.theory.whitebox_surrogate_v2 import Topology
t = Topology(bisection_bw_bytes_per_us=50000.0, hop_count=1.0)   # raw B/us
```

### What each fabric does

| Fabric | Bisection | Hop | Effect |
|---|---|---|---|
| `switched` / `nvl72` / `multiplane` (NVSwitch) | effectively infinite | 1 | no cap → **ideal** (== default) |
| `ring` | `2 * per_link` (tight) | 1 | caps the rate → much slower |
| `fullmesh` | `N * (N-1) * per_link / 2` (large) | 1 | usually uncapped |
| `3d_torus` / `mesh2d` / `2dfullmesh` | `(N/min_dim) * per_link / 2` | 1 | may cap |
| `leafspine` / `fattree` / `dragonflyplus` / ... | `N * lpg * per_link / 8` | `_leaftier_hop` > 1 | cap + extra hops |

Full per-family formulas: §13 of `WHITEBOX_SURROGATE.md`.

---

## 6. Scale-out (So) fabric + hierarchical (Su→So) collectives

When a job spans more than one node, NCCL runs the collective **hierarchically**:
each node does it locally over NVLink (Su, scale-up), one representative per node
does it across nodes over Ethernet RoCEv2 (So, scale-out), then the result is
redistributed locally (Su). The `HierarchicalSurrogate` class composes **any** Su
sub-surrogate with **any** So sub-surrogate under a So topology. It is
**model-agnostic** — H200 is just one preset; A800 / B200 / MI300 / any NVLink
plus any Ethernet fabric work the same way by building `su` and `so` from your
own hardware numbers.

The So term is an **uncalibrated lower bound** anchored to ns-3 (spec-derived
RoCEv2 params — 400GbE wire rate, 40 µs RDMA startup, 0.95 wire efficiency — not
real-HW calibration). The analytical model omits per-packet credit flow, VOQ
arbitration, PFC/MTU overhead, and per-phase barrier syncs that ns-3 models, so
the hierarchical latency sits below the ns-3 packet-level simulation.

### Entry point — the class, not a factory

```python
from surrogate.theory.whitebox_surrogate_v2 import HierarchicalSurrogate

h = HierarchicalSurrogate(su, so, gpus_per_node=8, num_nodes=4, so_topology=so_topo)
lat_us = h.predict(data_bytes, "ring", credits=128, collective="allreduce")
```

`su` and `so` are **any** `TheoryDerivedSurrogate` instances (§3). The convenience
factory `make_h200_hierarchical_theory(gpus_per_node, num_nodes,
so_topology_family, ...)` is one H200 preset for this; for any other platform,
build `su`/`so` yourself and compose — the class is the general entry point.

### Build `su` (NVLink) and `so` (Ethernet RoCEv2) — three levels, most general first

| Level | How | When |
|---|---|---|
| **1 full custom** | `TheoryDerivedSurrogate(ring_bw_bytes_per_us=..., startup_us=..., ...)` | every knob explicit, any vendor |
| **2 wire-derived** | `make_surrogate_from_wire(b_link_bytes_per_us, num_lanes, algo, **overrides)` | only the per-link wire rate known (preferred for new platforms) |
| **3 preset** | `make_h200_ring_theory()` / `make_1024_optical_fattree_theory()` / `make_connectx7_p2p_perftest_theory()` | reuse a shipped anchor |

The So sub-surrogate is a `TheoryDerivedSurrogate` with **Ethernet-flavored**
values — no H200 lock-in. The knobs that distinguish a So (RoCEv2) stage from a
Su (NVLink) stage:

| Knob | Su (NVLink) | So (Ethernet RoCEv2) | Why |
|---|---|---|---|
| `ring_bw_bytes_per_us` | NVLink schedule bw | Ethernet schedule bw (e.g. 160000 = 400GbE × 8 NIC) | per-fabric wire rate |
| `num_lanes` | NVLink links/GPU (18) | NICs/node (8) | So counts NICs, not NVLink lanes |
| `link_latency_us` | 0.4 | ~1.0 | per-hop propagation |
| `t_min_switch_us` | 4.10 (NVSwitch) | ~1.5 (Ethernet pipeline) | switch fill |
| `startup_us` | 15 (NCCL) | ~40 (RDMA) | software launch |
| `wire_efficiency` | 1.0 (no L3/L4 header) | 0.95 (Eth+IP+UDP+IB) | header overhead on T_serial |
| `tree_bw` / `nvls_bw` | used on Su | inherited, unused on So | So stage runs the ring path |

**Plain (non-RoCE) lossy Ethernet So fabric.** `make_h200_ethernet_theory()`
is the lossy counterpart to `make_h200_roce_theory()`: the same 400GbE L2 wire
(`ring_bw=160000`), but a **store-and-forward** switch pipeline
(`link_latency_us=3.0`, `t_min_switch_us=3.0` vs RoCE's cut-through 1.0/1.5)
and `wire_efficiency=1.0` (plain L2 data carries no RoCE IP/UDP/IB headers —
mirroring the ns-3 `EthernetProtocolModel`, which locks `WireEfficiency=1.0`).
The lossy drop/NACK/LLR-retransmit behavior is **ns-3-only** — the surrogate
is the uncongested mean (the same split as RoCE/DCQCN). To use it as the So
stage of a hierarchical collective, pass it via the `so_factory` seam:
`make_h200_hierarchical_theory(so_factory=make_h200_ethernet_theory)` (the
default is `make_h200_roce_theory`). See recipe N.

### So topology — `so_topology`

```python
from surrogate.theory.whitebox_surrogate_v2 import make_topology_from_family
so_topo = make_topology_from_family("so_spineleaf", N=num_nodes,
                                     links_per_gpu=8, per_link_gbps=50.0)
```

Three So families (`N` = node count, `links_per_gpu` = NICs/node,
`per_link_gbps` = unidirectional Ethernet GB/s, 50 = 400GbE):

| Family | Hop | Bisection | Use |
|---|---|---|---|
| `so_fullmesh` | 1 | non-blocking | **lower bound** (default); per-node NIC is the bottleneck |
| `so_spineleaf` | 2 | leaf uplink / spine downlink | oversubscribed, 1 inter-switch hop |
| `so_fattree` | 3 | tier-capped | most oversubscribed, 2 inter-switch hops |

For a fully custom So fabric use `make_topology(bisection_gbps=..., hop_count=...)` (§5 ②).

### `predict()` — `collective=` is required, selects the phase decomposition

`collective` is a **required keyword-only** argument (same rule as §4); it selects
how the Su and So stages compose:

| `collective` | Decomposition | So-stage volume |
|---|---|---|
| `allreduce` | Su(rs) + So(rs) + So(ag) + Su(ag) — 4 phases | `D/n_su` (per-node reduced chunk) |
| `allgather` | Su(ag) + So(ag) — 2 phases (ns-3 `Complete()` fires at end of inter-node AG; no third local broadcast) | full `D` (each GPU exchanges the gathered node buffer `D/n_so` with its `n_so−1` peers: `D·(n_so−1)/n_so` per GPU) |
| `alltoall` | Su + So — 2 phases | full `D` (each GPU sends its `D/(n_su·n_so)` chunk to every GPU on the other nodes: `D·(n_so−1)/n_so` per GPU; the Su stage moves `D/n_so` total — the local-peer share `D·(n_su−1)/(n_su·n_so)` per GPU) |
| `sendrecv` | So only — 1 phase | no Su stage (single inter-node hop) |

Worked examples (non-H200) are recipes J/K/L below.

---

## 7. Recipes

### A. H200 ring AllReduce, default (ideal) fabric

```python
s = make_surrogate_from_wire(25000, 18, "ring")
s.predict(256 * 1024 * 1024, 8, "ring", credits=128, ber=0, collective="allreduce")        # 1325.5 us
```

### B. Same ring algorithm on a real 8-GPU ring fabric (topology bites)

```python
from surrogate.theory.whitebox_surrogate_v2 import make_topology_from_family
t = make_topology_from_family("ring", N=8, per_link_gbps=25)
s.predict(256 * 1024 * 1024, 8, "ring", credits=128, ber=0, collective="allreduce", topology=t)   # ~4718 us
```

### C. Non-blocking NVSwitch fabric (== default, no per_link needed)

```python
t = make_topology_from_family("switched", N=8)
s.predict(256 * 1024 * 1024, 8, "ring", credits=128, ber=0, collective="allreduce", topology=t)   # == topology=None
```

### D. Link degradation — BER + FEC

```python
# Pre-FEC BER 1e-3 (residual post-FEC error > 0), RS(544,514,15) FEC + LLR
# Go-Back-N retransmission: the ~370us tail is dominated by GBN retransmission
# (at ber<=1e-4 or without llr_enabled, retransmission is 0).
s.predict(64 * 1024 * 1024, 8, "ring", credits=128, ber=1e-3,
         llr_enabled=True, fec_enabled=True, fec_n=544, fec_k=514, fec_t=15,
         retry_mode="gobackn", collective="allreduce")
```

### E. Contention from a concurrent flow

```python
# A background flow offering ~10% of the ring schedule bandwidth (177 GB/s),
# i.e. ~17.7 GB/s, on the shared bottleneck. competing_bw is in GB/s.
s.predict(16 * 1024 * 1024, 8, "ring", credits=128, ber=0, competing_bw=17.7, collective="allreduce")
```

### F. Custom (non-H200) platform from one wire rate

```python
# A hypothetical 8-link fabric at 50 GB/s/link unidirectional.
s = make_surrogate_from_wire(b_link_bytes_per_us=50000, num_lanes=8, algo="ring")
s.predict(256 * 1024 * 1024, 8, "ring", credits=128, ber=0, collective="allreduce")
```

### G. Tail (p99) latency

```python
s.predict_tail(256 * 1024 * 1024, 8, "ring", credits=128, ber=0, collective="allreduce", percentile=99)
```

### H. AlltoAll — and why a topology matters for it

```python
from surrogate.theory.whitebox_surrogate_v2 import (
    make_h200_alltoall_theory, make_topology_from_family)
s = make_h200_alltoall_theory()
# Ideal fabric: 7 partner phases, D/N each — a lower bound (no cross-section cap).
s.predict(256 * 1024 * 1024, 8, "ring", credits=128, ber=0, collective="alltoall")
# Real 8-GPU ring fabric: the tight bisection caps the rate -> much slower.
t = make_topology_from_family("ring", N=8, per_link_gbps=25)
s.predict(256 * 1024 * 1024, 8, "ring", credits=128, ber=0,
          collective="alltoall", topology=t)
```

### I. Send-Recv (point-to-point)

```python
from surrogate.theory.whitebox_surrogate_v2 import make_h200_sendrecv_theory
s = make_h200_sendrecv_theory()
# One hop: latency = startup(15us) + D/ring_bw + link_lat. No credit, no FEC.
s.predict(256 * 1024 * 1024, 8, "ring", credits=128, ber=0, collective="sendrecv")
```

### J. Scale-out + hierarchical — wire-derived stages (non-H200)

```python
from surrogate.theory.whitebox_surrogate_v2 import (
    make_surrogate_from_wire, make_topology_from_family, HierarchicalSurrogate)
D = 1 << 24  # 16 MiB

# Su: a non-H200 NVLink platform — 20 GB/s/link, 6 lanes, 10us startup
su = make_surrogate_from_wire(b_link_bytes_per_us=20000, num_lanes=6, algo="ring",
        startup_us=10.0, link_latency_us=0.5)
# So: 200GbE RoCE — 25 GB/s/link, 4 NICs, 40us RDMA startup, 0.95 wire-eff
so = make_surrogate_from_wire(b_link_bytes_per_us=25000, num_lanes=4, algo="ring",
        startup_us=40.0, link_latency_us=1.0, wire_efficiency=0.95, t_min_switch_us=1.5)
so_topo = make_topology_from_family("so_spineleaf", N=4, links_per_gpu=4, per_link_gbps=25.0)
h = HierarchicalSurrogate(su, so, gpus_per_node=6, num_nodes=4, so_topology=so_topo)
h.predict(D, "ring", 128, collective="allreduce")     # ~809.92 us
```

### K. Hierarchical — full-custom stages (every knob explicit)

```python
from surrogate.theory.whitebox_surrogate_v2 import (
    TheoryDerivedSurrogate, make_topology_from_family, HierarchicalSurrogate)
D = 1 << 24

su = TheoryDerivedSurrogate(packet_bw_bytes_per_us=120000, ring_bw_bytes_per_us=120000,
        startup_us=8.0, num_lanes=6, link_latency_us=0.5)
so = TheoryDerivedSurrogate(packet_bw_bytes_per_us=40000, ring_bw_bytes_per_us=40000,
        startup_us=40.0, num_lanes=4, link_latency_us=1.0,
        wire_efficiency=0.95, t_min_switch_us=1.5)
so_topo = make_topology_from_family("so_fullmesh", N=4, links_per_gpu=4, per_link_gbps=25.0)
h = HierarchicalSurrogate(su, so, gpus_per_node=6, num_nodes=4, so_topology=so_topo)
h.predict(D, "ring", 128, collective="allreduce")     # ~450.39 us
h.predict(D, "ring", 128, collective="allgather")     # ~413.76 us
```

### L. Hierarchical — reuse a non-H200 preset as the Su stage

```python
from surrogate.theory.whitebox_surrogate_v2 import (
    make_1024_optical_fattree_theory, make_surrogate_from_wire,
    make_topology_from_family, HierarchicalSurrogate)
D = 1 << 24

su = make_1024_optical_fattree_theory()              # 1024-GPU 600G optical study profile
so = make_surrogate_from_wire(b_link_bytes_per_us=25000, num_lanes=4, algo="ring",
        startup_us=40.0, link_latency_us=1.0, wire_efficiency=0.95, t_min_switch_us=1.5)
so_topo = make_topology_from_family("so_spineleaf", N=4, links_per_gpu=4, per_link_gbps=25.0)
h = HierarchicalSurrogate(su, so, gpus_per_node=8, num_nodes=4, so_topology=so_topo)
h.predict(D, "ring", 128, collective="allreduce")     # ~701.85 us
```

### M. Full-mesh — ns-3-calibrated (the 4th algorithm shape)

The full-mesh algorithm ships its own preset, `make_h200_fullmesh_theory()`,
whose `fullmesh_bw=449890` is back-fit to the ns-3 `FullMeshAllReduce` /
`FullMeshAllGather` sweep (η=0.9998 vs the `18×25000` dedicated-bundle
physics, MAPE 0.10%). Unlike ring/tree/NVLS (wire-rate derivations with
fitted constants — ring runs below ns-3 in the calibrated range, tree can
run above it), this one is **ground-truth-calibrated to ns-3** — a
least-squares back-fit that straddles ns-3 within ~0.1% (above at some
points, below at others). It still omits
per-packet credit flow, VOQ arbitration and the inter-phase barrier ns-3
models; the residual is the ~0.1% gap in either direction.

```python
from surrogate.theory.whitebox_surrogate_v2 import make_h200_fullmesh_theory
s = make_h200_fullmesh_theory()                       # ns-3-calibrated preset
# 256 MiB full-mesh AllReduce on 8 GPUs: 2 phases, D/N per phase.
s.predict(256 * 1024 * 1024, 8, "fullmesh", credits=128, ber=0, collective="allreduce")   # ~165 us
# AllGather: 1 phase -> ~90 us (T_AR ~ 2*T_AG - startup).
s.predict(256 * 1024 * 1024, 8, "fullmesh", credits=128, ber=0, collective="allgather")   # ~90 us
```

The wire-derived entry (a non-H200 full-mesh platform) uses the same η=1.0
default — `make_surrogate_from_wire(25000, 18, "fullmesh")` derives
`fullmesh_bw = 1.0 × 25000 × 18 = 450000`, matching the ns-3 fit to 0.02%:

```python
s = make_surrogate_from_wire(25000, 18, "fullmesh")  # fullmesh_bw=450000
s.predict(256 * 1024 * 1024, 8, "fullmesh", credits=128, ber=0, collective="allreduce")   # ~165 us
```

Full-mesh needs no topology: the per-phase bundle rate is far below the
full-mesh bisection, so the bisection cap never binds and `topology=None` is
byte-identical. Calibration details + the ns-3 sweep are in
[`../../doc/CALIBRATION.md`](../../doc/CALIBRATION.md) §“Full-mesh”.

### N. Plain (non-RoCE) Ethernet So fabric — the lossy RoCE counterpart

`make_h200_ethernet_theory()` is the lossy counterpart to
`make_h200_roce_theory()`: the same 400GbE L2 wire (`ring_bw=160000`) but a
**store-and-forward** switch pipeline instead of RoCE's cut-through RDMA path,
and `wire_efficiency=1.0` (plain L2 data carries no RoCE IP/UDP/IB headers —
mirroring the ns-3 `EthernetProtocolModel`, which locks `WireEfficiency=1.0`).

```python
from surrogate.theory.whitebox_surrogate_v2 import make_h200_ethernet_theory, make_h200_hierarchical_theory
# Plain-Ethernet So stage in a hierarchical 2-node allreduce (the lossy
# counterpart to the RoCE So stage). The lossy drop/NACK/LLR-retransmit
# behavior is ns-3-only; the surrogate is the uncongested mean (same split
# as RoCE/DCQCN).
h = make_h200_hierarchical_theory(gpus_per_node=8, num_nodes=2,
                                  so_factory=make_h200_ethernet_theory)
h.predict(256 * 1024 * 1024, "ring", credits=128, collective="allreduce")
```

The store-and-forward latency (`link_latency_us=3.0`, `t_min_switch_us=3.0`
vs RoCE's 1.0/1.5) dominates at small messages; at large messages the serial
term dominates and plain Ethernet is marginally faster (no RoCE header
overhead). The profile is a spec-derived **lower bound anchored to ns-3** (no
perftest/blog ground truth for plain Ethernet, unlike RoCE's `ib_write_lat`);
the parity test asserts surrogate ≤ ns-3 with a sanity floor. Calibration +
the ns-3 lossy model (VOQ tail-drop → switch NACK → LLR/SACK retransmit) are
in [`../../doc/CALIBRATION.md`](../../doc/CALIBRATION.md) §“Plain Ethernet”.

---

## 8. What it does and does not model

**Does:**
- Ideal-NVSwitch default fabric (byte-identical to the pre-topology model).
- Real-fabric bisection cap + hop propagation (when `topology=` is given).
- Credit pressure, FEC coding overhead, GBN/SACK retransmission, contention,
  optical link protection, LLR buffer reload — each enabled by setting the
  corresponding optional input.
- N-independent efficiency factors; N enters through the explicit collective
  topology (`(N-1)/N` ring steps, `log2(N)` tree levels, NVLS N-cancellation).

**Does not (by design):**
- Model firmware / arbiter / VOQ drain overhead → the fitted constants absorb
  them on average, so per-point error is two-sided (ring mostly below ns-3,
  tree can be above; the theory/ns-3 ratio is credit-sensitive — see
  [CALIBRATION.md](../../doc/CALIBRATION.md)). Use it for ordering/trends, not
  absolutes.
- The `ber > 0` retransmission path keeps the **construction** bandwidth
  (topology's effect on retransmission is second-order); only the `ber = 0`
  mean path is fully topology-aware. See §12 of `WHITEBOX_SURROGATE.md`.
- Replace packet simulation near a design frontier — surrogate winners are
  re-verified in ns-3.

---

## 9. Tests

```bash
python3 -m pytest surrogate/test/ -q          # 61 passed
python3 test/parity/test_surrogate_vs_ns3_fullmesh.py   # fullmesh vs ns-3 (6 pts)
```

`test_theory.py` checks the model is finite, monotone in size, within a 2×
physical band of the ns-3 H200 baseline, and exercises the topology and
collective kwargs: `collective` is required (omitting it raises `TypeError`)
and validated (a bad name raises `ValueError`), an explicit `collective`
reproduces the calibrated ring/tree/nvls configs, the full-mesh algorithm
(steps/per-step/credit, ns-3 reproduction at 256 MiB, `T_AR ≈ 2·T_AG`,
faster-than-ring), the ber>0 GBN-retransmission
path runs under an explicit collective, plus AlltoAll step/volume for ring+tree,
Send-Recv single-hop decomposition, the AlltoAll bisection-cap contention, the
hierarchical (Su→So) decomposition with its required `collective=` and So-family
ordering (`so_fullmesh` ≤ `so_spineleaf` ≤ `so_fattree`) + node-count
monotonicity, and the topology name→Topology factory + error paths. The
full-mesh parity test asserts the ns-3-calibrated surrogate tracks ns-3
within a tight [0.95, 1.05] band across N=8 AR+AG at {16,64,256} MiB.

---

## 10. Going deeper

- [`WHITEBOX_SURROGATE.md`](WHITEBOX_SURROGATE.md) — full derivation, the design
  rule, and the per-family topology formulas (§12, §13).
- [`../../doc/CALIBRATION.md`](../../doc/CALIBRATION.md) — how the H200 anchor
  numbers were measured.
