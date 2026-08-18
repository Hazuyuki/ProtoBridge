# Calibration

All calibration is against **NVIDIA H200 NVLink4** hardware. There is no A800
calibration in this release.

## Hardware reference

| Parameter | Value |
|-----------|-------|
| GPU | NVIDIA H200 |
| NVLink generation | NVLink4 |
| Links per GPU | 18 |
| Peak aggregate bandwidth (per direction) | 900 GB/s (7200 Gbps) |
| Link latency | 400 ns |
| FEC | RS(544, 514, 15) |
| Protocol profile | `configs/protocol_profiles/h200-ll128.profile` |

## Calibration data (`surrogate/calibration/`)

| File | Contents |
|------|----------|
| `h200_ring4_ar_baseline.json` | 27 raw ns-3 ring-allreduce points, 4 GPU, sizes 64 KiB – 1 GiB, 3 seeds each (`dataSize`, `numGpus`, `algorithm`, `simTimeUs`, …) |

## Surrogate accuracy

| Surrogate | Calibration set | Metric | Value |
|-----------|-----------------|--------|-------|
| Theory-derived (`surrogate/theory/`) | none (physical derivation) | vs ns-3 | 0.6–0.75× (lower bound; omits firmware overhead) |

## Generate fresh ns-3 baselines

To regenerate `h200_ring4_ar_baseline.json` (or any baseline) from the
simulator:

```bash
# 4-GPU ring allreduce, one size + seed at a time:
for sz in 65536 262144 1048576 4194304 16777216 67108864 \
          134217728 268435456 1073741824; do
  for seed in 1 2 3; do
    ./ns3 run "gpu-cluster-sim --numGpus=4 --topology=ring \
      --algorithm=ring --dataSize=$sz --rngRun=$seed \
      --fecN=544 --fecK=514 --fecT=15"
  done
done
```

Each run emits `RESULT_START … simTimeUs=… RESULT_END`; collect the
`simTimeUs` values into the baseline JSON. The default H200 profile applies
the link bandwidth, latency, and FEC parameters above.
