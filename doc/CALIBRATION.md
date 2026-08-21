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
