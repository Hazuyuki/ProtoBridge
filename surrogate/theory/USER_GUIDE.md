# ProtoBridge Latency Surrogate — User Guide

This guide shows how to **use** the latency surrogate to estimate collective
communication latency in microseconds. It is the fast screening model that
filters the broad design space before selected candidates are simulated in
full ns-3. For the full mathematical derivation, see
[`WHITEBOX_SURROGATE.md`](WHITEBOX_SURROGATE.md); for how the H200 anchor numbers
were measured, see [`../../doc/CALIBRATION.md`](../../doc/CALIBRATION.md).

The model predicts the latency of one collective operation (AllReduce /
AllGather / AlltoAll, scheduled as ring / tree / NVLS). It is a
first-principles physical derivation, so it is a **lower bound** on ns-3 — use
it for **ordering and trend analysis across a design space**, not for
absolute single-point prediction.

---

## 1. 30-second quickstart

```python
from surrogate.theory.whitebox_surrogate_v2 import make_surrogate_from_wire

# Build a surrogate for H200 NVLink4: 25 GB/s per link, 18 links per GPU.
s = make_surrogate_from_wire(b_link_bytes_per_us=25000, num_lanes=18, algo="ring")

# Predict mean latency of a 256 MiB ring AllReduce on 8 GPUs.
lat_us = s.predict(256 * 1024 * 1024, 8, "ring", credits=128, ber=0)
# lat_us == 1325.5  (the H200 reference point)
```

That is the whole core usage: **build** a surrogate, then **query** it.
Everything else on this page is optional.

---

## 2. The input model — three stages

| Stage | What you give | Required? |
|---|---|---|
| **① Build** a surrogate | per-link wire rate, lane count, algorithm | yes (or use an H200 preset) |
| **② Query** a latency | data size, GPU count, algorithm, credits | yes (4 inputs) |
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
                         eta_ring=0.40, eta_tree=1.05, eta_nvls=1.20, **overrides)
```

Derives every schedule bandwidth from **one** physical input — the per-GPU
NVLink aggregate `B_agg = num_lanes * b_link` — plus three N-independent
efficiency factors. No per-bandwidth calibration needed.

| Input | Meaning | H200 NVLink4 |
|---|---|---|
| `b_link_bytes_per_us` | single NVLink unidirectional wire rate | `25000` (25 GB/s) |
| `num_lanes` | NVLink links per GPU | `18` |
| `algo` | `ring` / `tree` / `nvls` | — |
| `eta_ring` / `eta_tree` / `eta_nvls` | efficiency factors (defaulted) | 0.40 / 1.05 / 1.20 |

Derived bandwidths land within ~2% of the calibrated H200 values (see §11 of
`WHITEBOX_SURROGATE.md`). This is the entry point for any new platform where
only the per-link wire rate is known.

### H200 measured presets

```python
from surrogate.theory.whitebox_surrogate_v2 import make_h200_ring_theory, make_h200_tree_theory, make_h200_nvls_theory
s = make_h200_ring_theory()      # ships the exact calibrated H200 ring numbers
```

Use these when you want the measured H200 values rather than the wire-rate
derivation. They accept `**overrides` too (any `__init__` keyword).

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
| `tree_bw_per_level_bytes_per_us` | bytes/µs | tree serialization | 26250 |
| `nvls_bw_bytes_per_us` | bytes/µs | NVLS serialization | 535500 |
| `link_latency_us` | µs (400 ns = 0.4) | ring/nvls propagation | 0.4 |
| `t_min_switch_us` | µs | tree fixed (NVSwitch fill) | 1.1 |
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
s.predict(data_bytes, num_gpus, algo, credits, ber=0, ..., topology=None)
```

### Required (4)

| Input | Meaning |
|---|---|
| `data_bytes` | operation data size in bytes |
| `num_gpus` | participating GPU count N |
| `algo` | `ring` / `tree` / `nvls` |
| `credits` | flow-control credit count |

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
four required inputs. The rest engage physical mechanisms only when set.

### Tail latency — `predict_tail()`

Same signature plus `percentile=99` (drops the `include_*` ablation switches):

```python
p99 = s.predict_tail(256 * 1024 * 1024, 8, "ring", credits=128, ber=0, percentile=99)
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
s.predict(256 * 1024 * 1024, 8, "ring", credits=128, ber=0, topology=t)   # ~4718 us
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

## 6. Recipes

### A. H200 ring AllReduce, default (ideal) fabric

```python
s = make_surrogate_from_wire(25000, 18, "ring")
s.predict(256 * 1024 * 1024, 8, "ring", credits=128, ber=0)        # 1325.5 us
```

### B. Same ring algorithm on a real 8-GPU ring fabric (topology bites)

```python
from surrogate.theory.whitebox_surrogate_v2 import make_topology_from_family
t = make_topology_from_family("ring", N=8, per_link_gbps=25)
s.predict(256 * 1024 * 1024, 8, "ring", credits=128, ber=0, topology=t)   # ~4718 us
```

### C. Non-blocking NVSwitch fabric (== default, no per_link needed)

```python
t = make_topology_from_family("switched", N=8)
s.predict(256 * 1024 * 1024, 8, "ring", credits=128, ber=0, topology=t)   # == topology=None
```

### D. Link degradation — BER + FEC

```python
# Pre-FEC BER 1e-3 (residual post-FEC error > 0), RS(544,514,15) FEC + LLR
# Go-Back-N retransmission: the ~370us tail is dominated by GBN retransmission
# (at ber<=1e-4 or without llr_enabled, retransmission is 0).
s.predict(64 * 1024 * 1024, 8, "ring", credits=128, ber=1e-3,
         llr_enabled=True, fec_enabled=True, fec_n=544, fec_k=514, fec_t=15,
         retry_mode="gobackn")
```

### E. Contention from a concurrent flow

```python
# A background flow offering ~10% of the ring schedule bandwidth (177 GB/s),
# i.e. ~17.7 GB/s, on the shared bottleneck. competing_bw is in GB/s.
s.predict(16 * 1024 * 1024, 8, "ring", credits=128, ber=0, competing_bw=17.7)
```

### F. Custom (non-H200) platform from one wire rate

```python
# A hypothetical 8-link fabric at 50 GB/s/link unidirectional.
s = make_surrogate_from_wire(b_link_bytes_per_us=50000, num_lanes=8, algo="ring")
s.predict(256 * 1024 * 1024, 8, "ring", credits=128, ber=0)
```

### G. Tail (p99) latency

```python
s.predict_tail(256 * 1024 * 1024, 8, "ring", credits=128, ber=0, percentile=99)
```

---

## 7. What it does and does not model

**Does:**
- Ideal-NVSwitch default fabric (byte-identical to the pre-topology model).
- Real-fabric bisection cap + hop propagation (when `topology=` is given).
- Credit pressure, FEC coding overhead, GBN/SACK retransmission, contention,
  optical link protection, LLR buffer reload — each enabled by setting the
  corresponding optional input.
- N-independent efficiency factors; N enters through the explicit collective
  topology (`(N-1)/N` ring steps, `log2(N)` tree levels, NVLS N-cancellation).

**Does not (by design):**
- Model firmware / arbiter / VOQ drain overhead → it is a **lower bound**
  (a lower bound on ns-3; the theory/ns-3 ratio is credit-sensitive — see
  [CALIBRATION.md](../../doc/CALIBRATION.md)). Use it for ordering/trends, not
  absolutes.
- The `ber > 0` retransmission path keeps the **construction** bandwidth
  (topology's effect on retransmission is second-order); only the `ber = 0`
  mean path is fully topology-aware. See §12 of `WHITEBOX_SURROGATE.md`.
- Replace packet simulation near a design frontier — surrogate winners are
  re-verified in ns-3.

---

## 8. Tests

```bash
python3 -m pytest surrogate/test/ -q          # 22 passed
```

`test_theory.py` checks the model is finite, monotone in size, within a 2×
physical band of the ns-3 H200 baseline, and exercises the topology kwarg
(byte-identity, bisection cap, hop latency, the name→Topology factory, and the
error paths).

---

## 9. Going deeper

- [`WHITEBOX_SURROGATE.md`](WHITEBOX_SURROGATE.md) — full derivation, the design
  rule, and the per-family topology formulas (§12, §13).
- [`../../doc/CALIBRATION.md`](../../doc/CALIBRATION.md) — how the H200 anchor
  numbers were measured.
