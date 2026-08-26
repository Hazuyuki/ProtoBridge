# Calibration

All calibration is against **NVIDIA H200 NVLink4** hardware. There is no A800
calibration in this release.

## Hardware reference

H200 NVLink4 is the calibration reference; the fabric-hardware + protocol
values live in
[`configs/protocol_profiles/h200-ll128.profile`](../configs/protocol_profiles/h200-ll128.profile)
(annotated in [`doc/CONFIG_GUIDE.md`](CONFIG_GUIDE.md) §3). The 4-GPU baseline
recipe below carries the exact runtime values it uses.

## Calibration data (`surrogate/calibration/`)

| File | Contents |
|------|----------|
| `h200_ring4_ar_baseline.json` | 27 raw ns-3 ring-allreduce points, 4 GPU, sizes 64 KiB – 1 GiB, 3 seeds each (`dataSize`, `numGpus`, `algorithm`, `simTimeUs`, …) |

## Surrogate accuracy

| Surrogate | Calibration set | Metric | Value |
|-----------|-----------------|--------|-------|
| Theory-derived (`surrogate/theory/`) | calibrated — ring_bw=177000 B/µs back-fit to the H200 HW point (first-principles wire derivation = 180000; 177000 sits 1.7% below), not a pure physical derivation | vs ns-3 | lower bound vs the 4-GPU ring baseline (≈0.20–0.57× at vcCredits=64; the baseline JSON records no credit value); ring (8-GPU/256MB) credit-sensitive — cr=128 reproduces HW 1347.65 µs (−0.003%), cr=64 → +42.3%; nvls credit-insensitive (≈0.566× vs 8-GPU ns-3) |

## Generate fresh ns-3 baselines

To regenerate `h200_ring4_ar_baseline.json` (or any baseline) from the
simulator:

```bash
# 4-GPU ring allreduce, one size at a time. The sim is deterministic, so one
# run per size reproduces the baseline byte-for-byte (seeds 1/2/3 are
# identical). ber=0 => FEC/retry inactive, so no --fecN/K/T or --llr* needed.
for sz in 65536 262144 1048576 4194304 16777216 67108864 \
          134217728 268435456 1073741824; do
  ./ns3 run "gpu-cluster-sim --numGpus=4 --topology=ring --algorithm=ring \
    --dataSize=$sz --bandwidth=400 --latency=400 \
    --startupLL=15000 --startupLL128=25000 --startupSIMPLE=46000"
done
```

Each run emits `RESULT_START … simTimeUs=… RESULT_END`; collect the
`simTimeUs` values into the baseline JSON. The baseline uses `--bandwidth=400`
(the 8-GPU calibrated configs use the profile's `bandwidthGbps=170`; the 4-GPU
ring baseline uses the `400` override) and `ber=0`, so FEC/retry are inactive.
Reference points: 1 MiB → 61.0 µs, 4 MiB → 182.6 µs, 256 MiB → 8101.5 µs.

## RoCE v2 P2P (scale-out) — perftest-anchored, soft external calibration

The scale-out (So) RoCEv2 fabric has **no real-HW anchor** in this release (the
A800 calibration target is single-node NVLink; it cannot produce inter-node
RoCE measurements). A third-party perftest blog
(`/share_data/zhuyu/RoCEv2_benchmark_data.md`, ConnectX-7 400G, RoCEv2 GID idx
3, MTU 4096, RC, TX depth 128) is used as a **soft, externally-anchored sanity
calibration** of the *raw-RDMA scalar constants* — it is a third-party blog,
not peer-reviewed, not the project's A800 hardware.

### Layer distinction (the key caveat)

The perftest measures **raw RDMA** (one-sided `ib_write_bw` / `ib_write_lat`)
— the layer *below* NCCL. There is no collective algorithm, no GPU kernel
launch, no NCCL persistent-kernel wakeup. It therefore calibrates the raw
wire + NIC constants, **not** the NCCL-collective timing. The ns-3
`RoceProtocolModel`'s 40 µs startup is the NCCL-launch layer (validated
separately for the Su fabric); the perftest does **not** exercise it.

### Calibration script + extracted constants

[`configs/calibration/roce_p2p_calib.py`](../configs/calibration/roce_p2p_calib.py)
encodes the blog data and fits two models; the extracted constants land in
[`configs/calibration/roce_p2p_calib.json`](../configs/calibration/roce_p2p_calib.json):

| Constant | Value | How extracted | Fit quality |
|----------|-------|----------------|-------------|
| per-QP single-flow ceiling | 49.0 Gbps | single-QP `ib_write_bw` saturation | direct |
| wire efficiency | 0.988 | 8-QP saturation 395/400 = 0.9875 | direct |
| per-message fixed latency | 1.45 µs | 2-byte `ib_write_lat` (doorbell + prop + completion) | — |
| single-write serialization rate | ~199 Gbps | 2B→64 KB `ib_write_lat` slope (JSON stores 199.26; surrogate uses the round 192 Gbps = 24 k B/µs, which fits the linear endpoints better) | — |
| NIC message-issue rate | ~8.2 Mmsg/s | 2B/4B/64B `ib_write_bw` (∝ S; mean 8.24, 64 B point 8.16) | MAPE 1.6 % |
| `ib_write_lat` curve L(S)=a+b·S^c | a=1.45, b=1.65e-3, c=0.669 | 4-point `curve_fit` | MAPE 0.11 % |
| saturation knee | 4 KB | 90 % of ceiling | — |
| line rate | 400 Gbps | ConnectX-7 spec | match |

The bandwidth ramp (MAPE 19.8 % on a smooth Hill form) is perftest-TX-depth-128
pipeline dynamics — **not** a calibration target; only the ceiling (49 Gbps) and
the message-issue rate are calibration-relevant.

### Calibrated analytical model (surrogate)

`make_connectx7_p2p_perftest_theory()` (in `surrogate/theory/whitebox_surrogate_v2.py`)
returns a `TheoryDerivedSurrogate` configured so `predict(D, 2, "ring",
credits=128, collective="sendrecv")` reproduces `ib_write_lat`: per-message
fixed latency 1.45 µs (doorbell+completion 1.05 µs + same-switch propagation
0.4 µs), single-write effective rate 192 Gbps (24 000 B/µs — the round value
nearest the ~199 Gbps measured 2B→64 KB slope; NIC-processing-limited, below
the 400 G line rate; already effective so `wire_efficiency=1.0`),
single QP (`num_lanes=1`). Fit: 2 B / 64 B / 64 KB within 2 %, the 1 KB
transition within 8 % (the single-write curve is sublinear in NIC processing;
the linear surrogate fits the endpoints), overall **MAPE 2.6 %**.

The per-QP throughput ceiling (~49 Gbps) and the 0.988 wire efficiency are
**throughput / aggregate** concepts — they are **not** expressed by this
single-transfer latency model (the surrogate's `predict()` is a latency, not a
sustained-throughput model). The 49 Gbps ceiling would be applied in ns-3 via
an opt-in per-flow rate cap (follow-up, see below).

### What this validates in the ns-3 RoCE model

- `wire_efficiency` 0.95 → **validated as slightly conservative** (8-QP
  best-case saturation 0.9875; 0.95 is a reasonable operating value). No change.
- line rate 400 Gbps → **match**. No change.
- per-QP ceiling 49 Gbps + doorbell 1.45 µs → raw-RDMA constants; the ns-3
  NCCL-collective layer uses multiple QPs (aggregate → ~400 G) and is
  NCCL-launch-dominated, so these do **not** change collective timing. They
  are documented constants for the raw-RDMA layer.

### ns-3 P2P support (status)

ns-3 **does** support P2P: `P2pTraffic` + `FabricEndpoint::SendP2p` (the raw
single-packet send, no NCCL startup) + the `.cfg` `[op]` stencil `kind = P2P`
(`ProtocolTransactionKind::P2P_TRANSFER`). A runnable example is
[`configs/protocol_configs/p2p-single.cfg`](../configs/protocol_configs/p2p-single.cfg)
(`startup = 0` → raw transfer; verified: packets sent/received, `simTimeUs`
scales with size — 2 B → 1.2 µs, 8 MiB → 836.8 µs).

**Gaps (deferred):**
- The `.cfg` SEND_P2P completion measures **dispatch time** (`latencyNs` is
  fixed at 400 ns regardless of size), not delivery latency — a perftest-style
  ns-3 latency needs delivery-complete instrumentation.
- A RoCE P2P run needs a RoCE PEX profile (`configs/protocol_profiles/`) +
  the inter-node `RoceProtocolModel` wired as the So protocol model (the
  WU1–WU3 scale-out plan). The analytical calibrated model
  (`make_connectx7_p2p_perftest_theory`) covers the RoCE P2P case in the
  meantime.
- `--collective=p2p` inline CLI is a no-op (validated but no injector
  handler); P2P is reachable via the `.cfg` stencil path.

### Applicability / limits of this data

- **Usable for:** raw-RDMA scalar constants (per-QP ceiling, wire efficiency,
  per-message doorbell latency, saturation knee).
- **NOT usable for:** collective simTime fidelity (raw P2P ≠ AR/AG/A2A);
  multi-node/multi-hop scaling (2-node same-switch only); DCQCN/PFC/ECN
  (no congestion data — the blog itself flags this, pointing to the
  SIGCOMM 2015 DCQCN paper).

