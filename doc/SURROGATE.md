# Surrogate model

ProtoBridge ships a latency surrogate so broad design spaces can be screened
without running the full ns-3 sweep. It is grounded in the H200 NVLink4
reference (see [CALIBRATION.md](CALIBRATION.md)).

> **New here?** [`surrogate/theory/USER_GUIDE.md`](../surrogate/theory/USER_GUIDE.md)
> is the human-friendly getting-started guide (30-second quickstart, full input
> reference, copy-paste recipes). This page is a short overview + parameter
> table; the full derivation is in
> [`surrogate/theory/WHITEBOX_SURROGATE.md`](../surrogate/theory/WHITEBOX_SURROGATE.md).

## Theory-derived surrogate — `surrogate/theory/`

`whitebox_surrogate_v2.py` is a first-principles physical model. The full
derivation lives in [`surrogate/theory/WHITEBOX_SURROGATE.md`](../surrogate/theory/WHITEBOX_SURROGATE.md);
the core design rule keeps fixed latency separate from serialized packet
work:

```
L_hat = S_protocol                              # protocol startup (LL/LL128 ~15us, SIMPLE ~65us)
        + steps * T_fixed                        # per-step fixed (hop × link latency + sw overhead)
        + steps * T_serial * Phi_credit          # serialized bytes / effective_bw
                         * Phi_fec               #   × FEC bandwidth overhead (N/K)
                         * Phi_share             #   × contention share (residual capacity, 1/(1−ρ))
        + T_retransmission                       # GBN/SACK retransmission tail
        + T_reload                               # LLR buffer reload
        + T_fec_latency                         # FEC encode + decode latency
```

H200 factory functions: `make_h200_ring_theory()`, `make_h200_tree_theory()`,
`make_h200_nvls_theory()`, `make_1024_optical_fattree_theory()`. For a new
platform where only the per-link wire rate is known, the **recommended** entry
is the single-wire-rate factory `make_surrogate_from_wire(b_link, num_lanes,
algo)`, which derives every schedule bandwidth from one input (see §11 of
`WHITEBOX_SURROGATE.md`). Pass an optional `topology=` descriptor — a
`Topology` dataclass built via `make_topology(bisection_gbps=, hop_count=)` or
`make_topology_from_family(family, N, per_link_gbps=...)` — to model a real
fabric's bisection cap and hop count; omit it for the default ideal NVSwitch
fabric (byte-identical to the pre-topology model).

```python
from surrogate.theory import whitebox_surrogate_v2 as wb
s = wb.make_h200_ring_theory()
lat_us = s.predict(data_bytes=1<<20, num_gpus=4, algo="ring", credits=32, ber=0)
```

Because it is a physical derivation (no firmware/arbiter/VOQ overhead), it is a
**lower bound** on the ns-3 measurement — the theory/ns-3 ratio is
credit-sensitive (see [CALIBRATION.md](CALIBRATION.md) for the pinned range).
Use it for ordering and trend analysis, not absolute prediction.

## Defining custom hardware

The factories bake in calibrated H200 (or 1024-GPU optical) values. To model
different hardware, pass `**overrides` — any `__init__` keyword replaces the
anchor value (the full list, with units and the H200 value, is in the
`TheoryDerivedSurrogate` class docstring):

```python
from surrogate.theory import whitebox_surrogate_v2 as wb
# H200 ring profile, but with 8 NVLink lanes and a 7us protocol startup:
s = wb.make_h200_ring_theory(num_lanes=8, startup_us=7)
# or construct from scratch for a non-H200 platform:
s = wb.TheoryDerivedSurrogate(ring_bw_bytes_per_us=120000, link_latency_us=0.3,
                              credit_bdp_ring_packets=60, startup_us=10.0,
                              num_lanes=8)
lat_us = s.predict(1<<20, 4, "ring", credits=32, ber=0)
```

The parameters that move the clean (ber=0) mean latency:

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
under link degradation (ber>0) or with FEC/LLR enabled.

> **Note:** the bandwidth parameters are *effective* transfer rates already
> incorporating protocol efficiency and spray — not raw per-link × lanes
> products — and the H200 values are the calibrated anchor. Custom values yield
> a physically-plausible-but-uncalibrated lower bound.

## Tests

```bash
python3 -m pytest surrogate/test/ -v
```

- `test_theory.py` — the theory model returns finite, monotone-in-size
  latencies within a 2× physical band of the ns-3 baseline (generous, since
  it is an uncalibrated lower bound).
