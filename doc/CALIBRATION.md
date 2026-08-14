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
| `h200_ring4_surrogate_calibration.json` | Pre-fit `AnalyticalSurrogate` params (avg 0.92% / max 2.68% APE) |
| `h200_nvls_ar_baseline.json` | NVLS allreduce baseline |
| `h200_surrogate_calibration.json` | Multi-collective surrogate fit |
| `reference/` | 34 real NCCL measurement files on H200 8-GPU: ring/tree/nvls × allreduce/allgather/alltoall/broadcast × LL/LL128/SIMPLE |

## Surrogate accuracy

| Surrogate | Calibration set | Metric | Value |
|-----------|-----------------|--------|-------|
| Analytical (`surrogate/analytical/`) | `h200_ring4_ar_baseline.json` (27 pts) | mean APE | **0.9%** |
| | | max APE | 2.7% |
| | | within 10% | 100% (27/27) |
| Topology-aware (`surrogate/topo/`) | H200 128-GPU ring e2e sweep (18 pts) | MAPE | **0.47%** |
| Theory-derived (`surrogate/theory/`) | none (physical derivation) | vs ns-3 | 0.6–0.75× (lower bound; omits firmware overhead) |

## Re-derive the analytical calibration

```bash
cd surrogate/analytical
python3 analytical_surrogate.py
```

This loads `../calibration/h200_ring4_ar_baseline.json`, fits the piecewise
small/large-size regression + FEC/retry amplification overlay, validates
against the same baseline, and writes
`../calibration/h200_ring4_surrogate_calibration_regenerated.json`.

Expected output:

```
Validation against H200 ring4 baseline:
  Within 10%: 100.0% (27/27)
  Avg error: 0.9%
  Max error: 2.7%
```

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

## Real-hardware reference

The 34 files in `surrogate/calibration/reference/` are real NCCL timings
measured on an 8-GPU H200 NVLink node (file names encode
`h200-<topo>-<collective>-8gpu-<protocol>.json`). They are the ground-truth
anchor for the H200 NVLink parameters above and for sanity-checking the
simulator's 8-GPU predictions; the 4-GPU ring baseline the analytical
surrogate fits against is an ns-3 sweep, not a real measurement.
