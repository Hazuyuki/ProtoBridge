# ProtoBridge Analytical Latency Surrogate

This document specifies the surrogate implemented in
`whitebox_surrogate_v2.py`. The model predicts the mean latency of one
communication operation. It is used to screen broad design spaces before
selected candidates are evaluated with ProtoBridge.

## Quick Start — Recommended Entry

The recommended way to construct a surrogate for any platform is the
single-wire-rate factory `make_surrogate_from_wire`. It derives every
schedule bandwidth from one physical input — the per-GPU NVLink
aggregate `B_agg = num_lanes * b_link` — plus three N-independent
efficiency factors, so no per-bandwidth calibration is required:

```python
from whitebox_surrogate_v2 import make_surrogate_from_wire

# H200 NVLink4: 25 GB/s per link, 18 links per GPU
s = make_surrogate_from_wire(b_link_bytes_per_us=25000, num_lanes=18, algo="ring")

# Mean latency of a 256 MiB ring AllReduce on 8 GPUs
s.predict(256 * 1024 * 1024, 8, "ring", credits=128, ber=0)
```

| Input | Meaning | H200 |
|---|---|---|
| `b_link_bytes_per_us` | single NVLink unidirectional wire rate | 25000 |
| `num_lanes` | NVLink links per GPU | 18 |
| `algo` | ring / tree / nvls | — |
| `eta_ring`, `eta_tree`, `eta_nvls` (optional, defaulted) | efficiency factors | 0.40 / 1.05 / 1.20 |

Derived bandwidths: `ring_bw = packet_bw = eta_ring * B_agg`,
`tree_bw = (B_agg / num_lanes) * eta_tree`, `nvls_bw = eta_nvls * B_agg`
(all within 2% of the calibrated H200 values; the efficiency factors are
N-independent — N enters only the collective topology in `predict()`, see
Section 3 and Section 11).

The `make_h200_ring_theory` / `make_h200_tree_theory` /
`make_h200_nvls_theory` factories are preset profiles that ship the exact
calibrated H200 numbers — use them when you want the measured values rather
than the wire-rate derivation. `predict()` then takes the operation inputs
(`data_bytes`, `num_gpus`, `algo`, `credits`, ...) described in Section 2.

## 1. Design Rule

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
This change reduced full-dataset MAPE from 22.0% to 11.8% without adding a
mechanism-specific or message-size-specific fitted coefficient.

## 2. Inputs

### Operation and protocol inputs

| Input | Meaning |
|---|---|
| `D` | Operation data size in bytes |
| `N` | Participating GPU or NPU count |
| `algorithm` | Ring, tree, or NVLS schedule |
| `S_protocol` | Protocol startup latency |
| `steps` | Number of communication steps |
| `B_step` | Data transmitted in one step |
| `B_chunk` | Packetization or spray-chunk size |

### Fabric and mechanism inputs

| Input | Meaning |
|---|---|
| `BW_eff` | Effective schedule bandwidth |
| `T_fixed` | Link or switch-pipeline latency per step |
| `C` | Available credits |
| `C_bdp` | Credits needed to keep the schedule path busy |
| `N_fec`, `K_fec`, `T_fec` | Reed-Solomon code parameters |
| `f_optical` | Fraction of transfer work on protected optical links |
| `BER` | Pre-FEC bit-error rate |
| `retry_mode` | Go-Back-N or Selective Acknowledgment |
| `retry_buffer_bytes` | Retained retransmission data |
| `rho_shared` | Competing load at the shared bottleneck |

The H200 profile uses three schedule-throughput quantities and two
schedule-specific credit-window quantities. The previous implementation used
one link throughput, one switch-efficiency factor, one credit-batching value,
and two saturation quantities. The revised profile therefore does not
increase the calibration-parameter budget.

## 3. Transfer Work

The collective schedule determines the number of steps and data per step.
Serialized time is

```text
T_serial = B_step / BW_eff
```

The current H200 profile uses:

| Schedule | Effective transfer quantity |
|---|---:|
| Ring | 177,000 bytes/us |
| Tree | 26,250 bytes/us per active level |
| NVLS | 535,500 bytes/us aggregate work bandwidth |

Ring uses link propagation for `T_fixed`. Tree uses the switched-path pipeline
delay. NVLS uses its multicast schedule and startup profile.

Ring's 177,000 B/us = 0.40 * B_agg (B_agg = num_lanes * b_link), corrected
from 94,000; see section 11. The MAPE figures in section 9 are a
pre-correction exp3 snapshot run in the parent repo and are not re-derived
here.

## 4. Credit Pressure

With `C` credits and `B_chunk` bytes per credit, the sender can keep
`C * B_chunk` bytes in flight. The schedule requires

```text
C_bdp = BW_path * T_credit_return / B_chunk
Phi_credit = max(1, C_bdp / C)
```

The ratio `C / C_bdp` is the fraction of path bandwidth sustainable by the
credit window. The current H200 profile uses 91.5 packets for ring and 35.5
packets for tree. NVLS does not apply the endpoint credit factor in the
evaluated profile.

## 5. FEC

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

## 6. Link-Level Retransmission

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

## 7. Contention

`rho_shared` is the fraction of the selected link or switch-output capacity
consumed by concurrent traffic using that bottleneck. Residual capacity is
`BW_eff * (1 - rho_shared)`, so

```text
Phi_share = 1 / (1 - rho_shared)
```

Disjoint routes use `rho_shared = 0`. Source injection rate is not substituted
for bottleneck utilization.

## 8. Isolated Formula Validation

The controlled sweep enables one mechanism at a time for 1, 4, 16, 64, and
256 MiB transfers.

| Mechanism | Full formula MAPE | Term removed MAPE |
|---|---:|---:|
| Credit pressure | 0.62% | 73.91% |
| FEC coding | 0.00% | 5.53% |
| High-BER SACK retransmission | 5.22% | 8.47% |
| Output contention | 1.06% | 9.03% |

The high-BER test uses pre-FEC BER `1e-3` and RS(550,514,18). Its stronger
code keeps the controlled sweep in a completed-operation regime while still
exposing retransmission delay. The contention test uses a smooth background
flow offered at 10% of the shared output-link rate.

Artifacts:

- `exp3_surrogate/parsed/exp3_mechanism_formula_validation.csv`
- `exp3_surrogate/parsed/exp3_mechanism_ablation.csv`
- `exp3_surrogate/parsed/exp3_contention_surrogate_audit.csv`
- `exp3_surrogate/figures/exp3_mechanism_formula_validation.pdf`
- `exp3_surrogate/figures/exp3_mechanism_ablation.pdf`

## 9. Full-Dataset Evaluation

The dataset contains 1,898 completed ProtoBridge runs and 640
seed-independent configurations. Configuration-level five-fold evaluation
produces:

| Model | MAPE | P95 error | Pairwise ranking accuracy |
|---|---:|---:|---:|
| Latency-bandwidth | 55.7% | 212.1% | 0.676 |
| Factorized surrogate | **11.8%** | **31.5%** | **0.878** |
| Random forest | 41.8% | 190.3% | 0.518 |
| XGBoost | 81.9% | 406.3% | 0.449 |
| MLP | 47.1% | 143.3% | 0.553 |

Leave-one-group-out MAPE is 10.6% for ring, 15.1% for tree, 11.1% for
8-GPU configurations, 12.5% for 16-GPU configurations, and 9.5% for the
high-BER regime. MLP reaches 46.5% on the 8-GPU holdout and 156.6% on the
high-BER holdout. Leave-one-size-out MAPE for the
surrogate remains at or below 17.9%.

The surrogate retains every packet-simulated winner after evaluating 33.4% of
each candidate set on average. The latency-bandwidth model requires 68.5%.
One surrogate query takes 20.5 us on the evaluation host.

Primary artifacts:

- `exp3_surrogate/parsed/exp3_2_model_eval.csv`
- `exp3_surrogate/parsed/exp3_2_extrapolation.csv`
- `exp3_surrogate/parsed/exp3_3_generalization.csv`
- `exp3_surrogate/parsed/exp3_design_selection_summary.json`
- `exp3_surrogate/parsed/exp3_runtime.csv`

## 10. Scope

The H200 profile is not a protocol-independent hardware law. A new platform or
protocol family needs its own fixed profile and ProtoBridge validation. The
surrogate is used for broad screening. Packet simulation remains the source of
timing for selected candidates and for decisions near a frontier boundary.

## 11. Single-Parameter Derivation

`make_surrogate_from_wire(b_link, num_lanes, algo)` derives every schedule
bandwidth from one NVLink wire-rate input plus three physically-motivated
efficiency factors, with no per-bandwidth calibration:

```text
B_agg = num_lanes * b_link                       (per-GPU NVLink aggregate)
ring_bw = packet_bw = eta_ring * B_agg           ~0.40 (AllReduce algorithm bw)
tree_bw = (B_agg / num_lanes) * eta_tree         ~1.05 (one link + NCCL/FEC, ~= n/k)
nvls_bw = eta_nvls * B_agg                       ~1.20 (NVSwitch multicast gain)
```

For H200 (b_link = 25,000 B/us, num_lanes = 18, B_agg = 450,000), the
defaults eta_ring=0.40, eta_tree=1.05, eta_nvls=1.20 yield ring/packet =
180,000 (calibrated 177,000, +1.7%), tree = 26,250 (exact), nvls = 540,000
(calibrated 535,500, +0.8%) -- all within 2%. The ring-bus bandwidth
2(N-1)/N * alBw = 0.69 * B_agg falls in NCCL's typical 60-80% utilization
band, anchoring eta_ring in physics rather than as a fit coefficient. The
calibrated `make_h200_*` factories still ship the exact measured values; this
is the single-parameter entry point for a new platform where only the
per-link wire rate is known.

## 12. Topology (optional)

By default the model assumes an ideal non-blocking NVSwitch fabric: the
schedule bandwidths (`ring_bw`/`tree_bw`/`nvls_bw` = `eta * B_agg`) are the
per-GPU injection rate on a fabric with effectively infinite bisection, and
the per-step fixed latency is one switch traversal. Pass an optional
`topology` descriptor to `predict()` to model how a real interconnect topology
influences those two parameters:

```python
from whitebox_surrogate_v2 import make_surrogate_from_wire, make_topology

s = make_surrogate_from_wire(b_link_bytes_per_us=25000, num_lanes=18, algo="ring")

# Ideal NVSwitch fabric (the default): 256 MiB ring AllReduce on 8 GPUs.
ideal = s.predict(256 * 1024 * 1024, 8, "ring", credits=128, ber=0)            # ~1325 us

# Same ring algorithm on an actual 8-GPU ring topology: bisection = 2 links
# x 25 GB/s = 50 GB/s, which caps the schedule rate down.
ring_topo = make_topology(bisection_gbps=50, hop_count=1)
slow = s.predict(256 * 1024 * 1024, 8, "ring", credits=128, ber=0,
                 topology=ring_topo)                                        # ~4718 us
```

`Topology` carries the two physical quantities by which topology influences
latency:

| Field | Effect | Default |
|---|---|---|
| `bisection_bw_bytes_per_us` | caps the schedule's serialization rate via `min(<algo_bw>, bisection)` | `inf` (no cap) |
| `hop_count` | adds `(hop_count - 1) * link_latency` of per-step propagation | `1.0` (one switch) |

A non-blocking NVSwitch fabric leaves `bisection` infinite (no cap, `hop=1`);
a ring / 2-D torus has a tight bisection that caps the per-GPU usable
bandwidth; a k-tier fat-tree / leaf-spine sets `hop_count` to its depth.
`make_topology(bisection_gbps=…, hop_count=…)` accepts the bisection in GB/s
(1 GB/s = 1000 B/µs) and converts; the raw `Topology` field is in B/µs to
match the bandwidth parameters.

Omitting `topology` (or passing `Topology()` with defaults) is byte-identical
to the ideal-fabric model, so the calibrated H200 numbers in Section 11 are
unchanged. The bisection cap applies to the mean serialization term
(`T_serial`); the `ber > 0` retransmission path keeps the construction
bandwidth, so for the common `ber = 0` (calibration) regime the mean path is
fully topology-aware. The two physical numbers can be read from a fuller
topology module (e.g. `TopoSpec.to_surrogate_params()["bisection_bw_gbps"]` in
the DSE layer's `topo_grammar`) or supplied by hand; this module ships no
per-family bisection table.
