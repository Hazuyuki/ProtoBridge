# Calibration

All calibration is against **NVIDIA H200 NVLink4** hardware. There is no A800
calibration in this release.

## Hardware reference

| Parameter | Value |
|-----------|-------|
| GPU | NVIDIA H200 |
| NVLink generation | NVLink4 |
| Links per GPU | 18 |
| Peak aggregate bandwidth | 900 GB/s bidirectional (450 GB/s per direction; 7200 Gbps) |
| Link latency | 400 ns |
| FEC | RS(544, 514, 15) |
| Per-TypeId startup delay | LL 15000 / LL128 25000 / SIMPLE 46000 / NVLS 23000 ns |
| Protocol profile | `configs/protocol_profiles/h200-ll128.profile` |

## Calibration data (`surrogate/calibration/`)

| File | Contents |
|------|----------|
| `h200_ring4_ar_baseline.json` | 27 raw ns-3 ring-allreduce points, 4 GPU, sizes 64 KiB – 1 GiB, 3 seeds each (`dataSize`, `numGpus`, `algorithm`, `simTimeUs`, …) |

## Surrogate accuracy

| Surrogate | Calibration set | Metric | Value |
|-----------|-----------------|--------|-------|
| Theory-derived (`surrogate/theory/`) | none (physical derivation) | vs ns-3 | lower bound; credit-sensitive — ≈0.20–0.57× at the profile per-VC credit (vcCredits=64; the baseline JSON records no credit value) |

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
