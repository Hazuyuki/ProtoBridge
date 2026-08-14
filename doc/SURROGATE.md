# Surrogate models

ProtoBridge ships three latency surrogates so broad design spaces can be
screened without running the full ns-3 sweep. All three are calibrated against
H200 NVLink4 (see [CALIBRATION.md](CALIBRATION.md)).

## 1. Theory-derived surrogate — `surrogate/theory/`

`whitebox_surrogate_v2.py` is a first-principles physical model. The full
derivation lives in [`surrogate/theory/WHITEBOX_SURROGATE.md`](../surrogate/theory/WHITEBOX_SURROGATE.md);
the core design rule keeps fixed latency separate from serialized packet
work:

```
L_hat = S_protocol                              # protocol startup (LL/LL128 ~15us, SIMPLE ~65us)
        + steps * T_fixed                        # per-step fixed (hop × link latency + sw overhead)
        + steps * T_serial * Phi_credit          # serialized bytes / effective_bw
                         * Phi_fec               #   × FEC bandwidth overhead (K/N)
                         * Phi_share             #   × contention share (WFQ)
        + T_retransmission                       # GBN/SACK retransmission tail
        + T_reload                               # LLR buffer reload
        + T_fec_latency                         # FEC encode + decode latency
```

H200 factory functions: `make_h200_ring_theory()`, `make_h200_tree_theory()`,
`make_h200_nvls_theory()`, `make_1024_optical_fattree_theory()`.

```python
import whitebox_surrogate_v2 as wb
s = wb.make_h200_ring_theory()
lat_us = s.predict(data_bytes=1<<20, num_gpus=4, algo="ring", credits=32, ber=0)
```

Because it is a physical derivation (no firmware/arbiter/VOQ overhead), it is a
**lower bound** on the ns-3 measurement — typically 0.6–0.75× the simulated
latency. Use it for ordering and trend analysis, not absolute prediction.

## 2. Analytical surrogate — `surrogate/analytical/`

`analytical_surrogate.py` is a regression-calibrated piecewise model with a
FEC/retry amplification overlay. It splits the size axis at the LL128
threshold (2 MiB):

- **Small-size regime** (latency-bound): `L = alpha_small + beta_small * size`.
- **Large-size regime** (bandwidth-bound): `L = alpha + beta * size + gamma * size²`.
- A **FEC/retry overlay** multiplies the serialized term by a retry factor
  that grows with post-FEC BER and the retransmission mode (go-back-N vs SACK),
  capturing the effective-bandwidth collapse under link degradation.

Fit on `h200_ring4_ar_baseline.json` (27 points, 4-GPU ring allreduce):
**0.9% mean / 2.7% max APE**, 100% within 10%.

```python
import analytical_surrogate as an
s = an.AnalyticalSurrogate()
s.calibrate_from_file("../calibration/h200_ring4_ar_baseline.json")
result = s.predict_latency_us(1 << 20)   # -> {"p50": ..., "breakdown": {...}}
```

`predict_latency_us` returns p50/p99/tail percentiles plus a breakdown
(retry_factor, fec_overhead, contention_share, …). `configure()` takes a
hardware/topology/collective/optical/fec/retry dict so the same surrogate
projects optical BER scenarios (see the optical-reliability demo at the end of
`analytical_surrogate.py`).

## 3. Topology-aware surrogate — `surrogate/topo/`

`dse_topo_surrogate.py` decomposes latency into physical terms computed from a
`TopoSpec` plus a small calibrated per-group residual, so it generalizes to
**new** (topology, algorithm, size) combinations without a per-topology fit:

```
ring (pipelined):
  L = startup + step_count * (hop*LINK_LAT + chunk/(bw_eff*proto_eff) + sw_ring)
  step_count = 2*(N-1);  chunk = size/N
tree / nvls / sharp / fullmesh (concurrent broadcast/reduce):
  L = startup + hop*LINK_LAT + size/(bw_eff*proto_eff) + sw_flat
```

Only `sw_ring` (per-step, ring) and `sw_flat` (per-collective, tree/nvls) are
fit residuals; `hop_count`, `bw_eff`, and `step_count` are **computed** from
the `TopoSpec` and `algo_selector`. The residuals were fit on an H200 128-GPU
ns-3 e2e sweep (MAPE 0.47% over 18 ring points) and are embedded as
`DEFAULT_FIT` so the module runs without that sweep data.

```python
import dse_topo_surrogate as ts
from topo_grammar import enumerate_128gpu_specs
spec = next(s for s in enumerate_128gpu_specs(128) if s.feasible)
lat_us = ts.predict(spec, 1 << 20, N=128)        # algo="auto" resolves small→tree, large→ring/nvls
```

Companion modules: `topo_grammar.py` (TopoSpec + feasibility + 12 families),
`algo_selector.py` (matches `ResolveAutoAlgorithm` in the C++ simulator:
64 MiB threshold for tree↔ring, NVLS only on switched ≤8-GPU domains).

## Tests

```bash
python3 -m pytest surrogate/test/ -v
```

- `test_theory.py` — the theory model returns finite, monotone-in-size
  latencies within a 2× physical band of the ns-3 baseline (generous, since
  it is an uncalibrated lower bound).
- `test_analytical.py` — calibrate then validate: mean APE < 2%, max < 5%,
  ≥90% within 10%, plus finiteness and monotonicity.
- `test_topo_surrogate.py` — `predict()` is finite/positive/monotone on a
  shipped `topo_specs.csv` spec, auto-resolves tree→ring, and every feasible
  family classifies into a residual group.
