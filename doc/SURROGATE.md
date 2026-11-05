# Latency surrogate

The Python surrogate estimates collective and point-to-point communication
latency in microseconds. Build a model once, then call `predict()` with the
operation, algorithm, size and GPU count. Optional inputs describe topology,
credits, errors and competing traffic.

The required `collective` argument selects `allreduce`, `allgather`, `alltoall`
or `sendrecv`; the schedule is selected separately. Calibration and accuracy
conditions are recorded in [CALIBRATION.md](CALIBRATION.md). The equations are
[below](#mechanism-equations).

---

## 1. Quickstart

```python
from surrogate.theory.whitebox_surrogate_v2 import make_surrogate_from_wire

# Build a surrogate for H200 NVLink4: 25 GB/s per link, 18 links per GPU.
s = make_surrogate_from_wire(b_link_bytes_per_us=25000, num_lanes=18, algo="ring")

# Predict mean latency of a 256 MiB ring AllReduce on 8 GPUs.
lat_us = s.predict(256 * 1024 * 1024, 8, "ring", credits=128, ber=0, collective="allreduce")
# lat_us == 1325.5  (the H200 reference point)
```

Build a surrogate once, then query it for each operation.

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
                         eta_ring=0.40, eta_tree=0.685, eta_nvls=1.20,
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
| `eta_ring` / `eta_tree` / `eta_nvls` / `eta_fullmesh` | efficiency factors (defaulted) | 0.40 / 0.685 / 1.20 / 1.0 |

The wire-rate factory derives schedule bandwidths from the per-link rate and
efficiency factors. Use it when describing a platform from its link parameters;
use the presets below to retain the shipped calibration constants.

### H200 presets

```python
from surrogate.theory.whitebox_surrogate_v2 import make_h200_ring_theory, make_h200_tree_theory, make_h200_nvls_theory, make_h200_fullmesh_theory
s = make_h200_ring_theory()      # ships the exact calibrated H200 ring numbers
```

The presets retain their recorded calibration values and accept `**overrides`
(any `__init__` keyword). Calibration sources differ by schedule. For
AlltoAll and Send-Recv use `make_h200_alltoall_theory()` (SIMPLE 46 µs
startup) and `make_h200_sendrecv_theory()` (15 µs startup); both reuse the
H200 ring anchor with a protocol-appropriate startup. For the full-mesh
algorithm use `make_h200_fullmesh_theory()`; its bandwidth is fitted to ns-3.

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
| `tree_bw_per_level_bytes_per_us` | bytes/µs | tree GPU↔NVSwitch trunk (ns-3-calibrated) | 308300 |
| `nvls_bw_bytes_per_us` | bytes/µs | NVLS serialization | 535500 |
| `fullmesh_bw_bytes_per_us` | bytes/µs | full-mesh serialization (ns-3-calibrated) | 449890 |
| `link_latency_us` | µs (400 ns = 0.4) | ring/nvls/fullmesh propagation | 0.4 |
| `t_min_switch_us` | µs | tree fixed (NVSwitch pipeline cost) | 1.06 |
| `credit_bdp_ring_packets` | packets | ring credit pressure Φ_credit | 91.5 |
| `credit_bdp_tree_packets` | packets | tree credit pressure Φ_credit | 35.5 |
| `startup_us` / `nvls_startup_us` | µs | protocol startup (fixed) | 15.0 / 23.0 |

The remaining knobs (`packet_bw_bytes_per_us`, `spray_chunk_bytes`,
`bulk_chunk_bytes`, `num_lanes`, `fec_*`, `retry_source_*`) only take effect
under link degradation (ber>0) or with FEC/LLR enabled. The bandwidth
parameters are *effective* transfer rates already incorporating protocol
efficiency and spray (not raw per-link × lanes products); the H200 values are
the calibrated anchor, so custom values yield a physically-plausible but
uncalibrated estimate.

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
| Reliability / retransmission | `ber`, `fec_enabled`, `llr_enabled`, `fec_n`/`fec_k`/`fec_t`, `retry_mode` (`'gobackn'`/`'sack'`), `retry_buffer_entries`/`retry_buffer_bytes`, `retry_limit`, `strict_reliability`. For NVLS/SHARP collectives the retry-latency term fragments at the collective spray geometry (`SendCollectiveBulk`, e.g. 128 x 128 KiB at 4 GPU / 64 MiB AllGather), not the DATA bulk path — matching the reliability model. |
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
mapping is:

| `collective` | `algo` | steps | per-step volume | per-GPU volume |
|---|---|---|---|---|
| `allreduce` | `ring` | `2(N-1)` | `D(N-1)/N / steps` | `D(N-1)/N` |
| `allgather` | `ring` | `N-1` (one pass) | `D(N-1)/N / steps` | `D(N-1)/N` |
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

Per-family formulas are implemented by `make_topology_from_name` in
[`whitebox_surrogate_v2.py`](../surrogate/theory/whitebox_surrogate_v2.py).

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
| `t_min_switch_us` | 1.06 (NVSwitch) | ~1.5 (Ethernet pipeline) | switch fill |
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

### Plain Ethernet scale-out

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
in [Calibration](CALIBRATION.md#plain-ethernet-scale-out--lossy-ns-3-anchored-lower-bound).

## Scope

The model combines collective schedules, credit pressure, FEC expansion,
retry/reload work, shared contention and optional fabric bisection/hop limits.
Omitting `topology` selects an ideal nonblocking NVSwitch fabric. Predictions
can lie above or below packet simulation; accuracy depends on the calibration,
credit window and topology. Use [CALIBRATION.md](CALIBRATION.md) for provenance
and use packet simulation to check configurations selected by the surrogate.

NIC firmware, switch queue dynamics, GPU instructions, and the stateful
RDMA/PCIe submission model are outside this analytical model. Selecting IBGDA
or TLP execution in ns-3 does not add those mechanisms to surrogate predictions.


## Tests

```bash
python3 -m pytest surrogate/test/ -q
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

## Mechanism equations

### Design Rule

The surrogate keeps fixed latency separate from serialized packet work:

```text
L_hat = S_protocol
        + steps * T_fixed
        + steps * T_serial * Phi_credit * Phi_fec * Phi_share
        + T_retransmission
        + T_reload
        + T_fec_latency
```

Credit pressure, FEC expansion, and contention scale only serialized work.
They do not multiply protocol startup, propagation, or switch-pipeline delay.

### Transfer Work

The collective schedule determines the number of steps and data per step.
Serialized time is

```text
T_serial = B_step / BW_eff
```

The current H200 profile uses:

| Schedule | Effective transfer quantity |
|---|---:|
| Ring | 177,000 bytes/us |
| Tree | 308,300 bytes/us per active level (ns-3-calibrated trunk; 0.685 × B_agg) |
| NVLS | 535,500 bytes/us aggregate work bandwidth |

Ring uses link propagation for `T_fixed`. Tree uses the switched-path pipeline
delay. NVLS uses its multicast schedule and startup profile.

The ring preset uses 177,000 B/us. The wire-rate factory gives 180,000 B/us
from `0.40 * num_lanes * b_link` at 18 lanes and 25,000 B/us per lane;
the preset retains its fitted value.

#### Collective and step model

`predict()` takes a required `collective` (`allreduce` / `allgather` /
`alltoall` / `sendrecv`); together with `algorithm` it fixes `steps` and
`B_step`. It is not inferred from `algorithm` — the caller must name it
explicitly (omitting it raises `TypeError`; an invalid name raises `ValueError`).

| `collective` | `algorithm` | `steps` | `B_step` (bytes/step) | per-GPU volume |
|---|---|---:|---|---|
| `allreduce` | `ring` | `2(N-1)` | `D(N-1)/N / steps` | `D(N-1)/N` |
| `allreduce` | `tree` | `2⌊log₂N⌋` | `α(N)·D / steps` | `α(N)·D` |
| `allgather` | `nvls` | `2` | `D/N` | `D` (multicast) |
| `alltoall` | `ring` / `collnetdirect` | `N-1` | `D/N` | `D(N-1)/N` |
| `alltoall` | `tree` | `⌈log₂N⌉` | `D(N-1)/N / steps` | `D(N-1)/N` |
| `sendrecv` | any | `1` | `D` | `D` (single hop) |

AllReduce scatters `D(N-1)/N` per GPU over its `2(N-1)` ring steps
(reduce-scatter + all-gather); the tree's per-step volume is `α(N)·D/steps`
with the STRUCTURAL trunk-load `α(N)` = 1/2/3 (N≤2 single edge / N≤4 root
with two children / N≥5 interior node — the busiest GPU↔switch trunk of the
heap-shaped chunk-pipelined tree; zero free parameters, the tree analogue of
ring's (N-1)/N); AllGather via NVLS multicast moves `D/N` per
step over 2 steps; AlltoAll permutes `D(N-1)/N` per GPU — the volume
correction the DSE analytical model (`bw_model_predict`) describes in comments
but does not apply. The AlltoAll step count is `N-1` permutation phases
(ring / `collnetdirect`) or `⌈log₂N⌉` recursive-doubling rounds (tree), not the
`2(N-1)` an AllReduce routing would imply. Send-Recv is a single
point-to-point hop: `steps=1`, the full payload, no pipeline credit pressure
(`Phi_credit = 1`, `C_bdp = ∞`).

The per-step serialization `T_serial = B_step / BW_eff` uses the per-link ring
rate for AlltoAll and Send-Recv (they serialize over the per-link wire, like
the ring schedule); AlltoAll's cross-section contention — every GPU exchanges
with every other simultaneously — is captured by the **topology bisection
cap** of Section 12 (`min(ring_bw, bisection)`), not a separate bandwidth
constant, so without a topology AlltoAll is a per-link lower bound.

### Credit Pressure

With `C` credits and `B_chunk` bytes per credit, the sender can keep
`C * B_chunk` bytes in flight. The schedule requires

```text
C_bdp = BW_path * T_credit_return / B_chunk
Phi_credit = max(1, C_bdp / C)
```

The ratio `C / C_bdp` is the fraction of path bandwidth sustainable by the
credit window. The current H200 profile uses 91.5 packets for ring and 35.5
packets for tree. NVLS does not apply the endpoint credit factor in the
evaluated profile. The scale-out (So) presets (`make_h200_roce_theory`,
`make_h200_ethernet_theory`) pin both BDPs to infinity: the ns-3 So
(inter-node) path bypasses the per-VC credit gate — scale-out traffic is
governed by ECN/DCQCN or VOQ tail-drop + LLR, not credits — so the So stage
must not inherit the NVLink credit window.

### FEC

For RS(`N_fec`, `K_fec`, `T_fec`), coding expands protected traffic by

```text
Phi_fec =
    1 + f_optical * (N_fec / K_fec - 1)
```

The codec pipeline latency is added separately as `T_fec_latency`.

For a pre-FEC BER `b`, a ten-bit symbol is erroneous with probability

```text
p_symbol = 1 - (1 - b)^10
```

If `X` is binomial with `N_fec` trials and probability `p_symbol`, a codeword
is uncorrectable with probability `Pr[X > T_fec]`. Packet failure probability
accounts for every codeword carried by that packet.

### Link-Level Retransmission

One protocol step can issue several packet fragments in parallel. The step
cannot complete until its slowest required fragment arrives. If each fragment
fails with probability `p` and the step contains `P` fragments, the expected
maximum number of attempts is

```text
E[A_max] = sum over r >= 0 of (1 - (1 - p^r)^P)
```

Selective Acknowledgment retransmits only failed fragments:

```text
T_retransmission,SACK =
    steps * T_serial * (E[A_max] - 1)
```

Go-Back-N also retransmits the issued suffix of the active window. Its mean
suffix length is `(W + 1) / 2`, where

```text
W = min(C, C_bdp, packets_per_transfer)
```

The implementation enforces that Go-Back-N cannot predict less
retransmission delay than Selective Acknowledgment. When a required packet is
absent from the configured retry buffer, `T_reload` adds the source latency
and source transfer time. The optical study uses GPU HBM as this source.

### Contention

`rho_shared` is the fraction of the selected link or switch-output capacity
consumed by concurrent traffic using that bottleneck. Residual capacity is
`BW_eff * (1 - rho_shared)`, so

```text
Phi_share = 1 / (1 - rho_shared)
```

`BW_eff` is the capacity the serial term actually serializes over — the
collective's schedule rate (ring rate for alltoall/sendrecv phases, tree/NVLS
trunk otherwise) already capped by any topology bisection/egress bottleneck.
Computing `rho_shared` against the uncapped link rate understates the penalty
exactly when the bottleneck is tightest (25 GB/s competing on a 50 GB/s-capped
link doubles the transfer; against the raw 177 GB/s rate it would barely
register).

Disjoint routes use `rho_shared = 0`. Source injection rate is not substituted
for bottleneck utilization.

## Implementation

The formulas and public factories are implemented in
[`whitebox_surrogate_v2.py`](../surrogate/theory/whitebox_surrogate_v2.py).
The [example catalog](../examples/gpu-cluster/README.md) lists independent
comparisons and sweep scripts; their recorded outputs have different scopes
from hardware validation.
