# Data-size and injection-rate sweeps (DCQCN incast + random traffic)

Five single-knob sweeps on the ns3-rdma-matched fabric — single shared-leaf
`tor` topology, 40 Gbps RoCE inter-node links, 1 µs latency, 1000-B bulk
chunks, `--soTxWindow=64`. This is the same matched fabric as
[doc/PFC_NS3RDMA_ALIGNMENT.md](../../../doc/PFC_NS3RDMA_ALIGNMENT.md) and
[doc/SCALEOUT_ALIGNMENT.md](../../../doc/SCALEOUT_ALIGNMENT.md) (the
ns3-rdma head-to-head notes), so the incast panels here are directly
comparable with those alignment tables; the ns3-rdma comparison itself is
documented in those two notes — this example is the ProtoBridge-side
characterization on the same fabric.

## Sweeps

| CSV | What it sweeps | Fixed |
|---|---|---|
| `datasize.csv` | DCQCN incast data size, 1 MB → 200 MB (7 points) | 3 nodes |
| `injection_rate.csv` | Per-source injection rate 0.1 → 1.0 × line rate (12 points) | 3 nodes, 4-KiB messages, 2 ms warmup + 4 ms window |
| `rate_msgsize.csv` | Injection rate (0.5/0.7/0.9) × message size (1/16/128 KiB) | 3 nodes |
| `nodes.csv` | Node count 2/4/8 (per-sender 50 MB) | DCQCN incast |
| `bulkchunk.csv` | Bulk chunk size 1000 → 8192 B | 3 nodes, 50 MB, DCQCN |

Random-traffic sweeps use `--randTraffic` (uniform-random cross-node
messages through the SoHeader/ECN/DCQCN bulk path — the same data path the
collectives use); offered/delivered bytes are counted inside the measurement
window only, with `RngRun=1` reproducibility.

## Run

```bash
./ns3 build                       # from the repo root
python3 run_sweeps.py             # 32 ns-3 runs -> the five CSVs (~15 min)
python3 plot_sweeps.py            # -> sweep_summary.{pdf,png}
```

The committed CSVs and figure come from commit `01e97a8`; a re-run should
reproduce them exactly (deterministic seeds).

## Reading the figure (`sweep_summary.png`)

1. **Data size (DCQCN incast)** — completion time is strictly linear in
   bytes (proportional fit R² = 1.000; aggregate throughput constant at
   ≈96 Gbps for 2 senders × 40 Gbps links). ECN marks grow linearly
   (~2 marks/KB) — DCQCN reacts per packet, so the marking rate scales
   with packet count, not with queue pressure.
2. **Injection rate (random traffic)** — delivered goodput tracks offered
   exactly up to rate 0.8, then saturates at ~96.8 Gbps with **zero packet
   loss** (all drop counters 0; the excess is absorbed by DCQCN rate
   shaping, not drops). The saturation point is the uniform-random
   destination-collision limit (each of 3 nodes receives ~1/3 of the
   traffic → ~32 Gbps < 40 Gbps per receiving NIC). Verified stable
   across 4/10/20-ms measurement windows.
3. **Rate × message size** — 128-KiB messages lose goodput earlier and
   harder (73 Gbps at rate 0.9 vs 94 for 1-KiB messages): a long message
   serializes as a burst of 1000-B chunks that fills the VOQ deeper
   between ECN reactions, so the rate machine's response granularity
   coarsens.
4. **Node scaling (DCQCN incast)** — per-sender throughput is flat at
   ~38.5 Gbps (near the 40 Gbps link cap) from 2 to 8 nodes; aggregate
   scales linearly. 2 nodes (single flow, no contention) produces 0 ECN
   marks; 4+ nodes mark but throughput is unaffected.
5. **Bulk chunk size (50 MB DCQCN incast)** — larger chunks → fewer ECN
   marks (proportional to packet count) and slightly faster completion
   (20.80 ms at 1000 B → 20.10 ms at 8192 B; the fixed per-packet
   pipeline overhead amortizes).
6. **Link utilization** — delivered / 120 Gbps aggregate line rate vs
   injection rate; the saturation knee leaves utilization at ~80.7 %.

## Relationship to the ns3-rdma alignment notes

The fabric parameters are deliberately the matched ones from the alignment
notes so the numbers can be cross-read (e.g. the 2-node row in
`nodes.csv` — 10.4 ms for 50 MB — is the same operating point as the PB
"tor, PFC off" 2:1 row in `doc/SCALEOUT_ALIGNMENT.md` at 21.90 ms for 2 ×
50 MB). The ns3-rdma head-to-head itself (FCT ratios, PFC modes, headroom
sizing) lives in those notes, not here — this example characterizes
ProtoBridge's DCQCN behavior on the same fabric across data volume,
offered load, message size, node count, and chunk size.

## Files

- `run_sweeps.py` — regenerates the five CSVs (32 ns-3 runs)
- `plot_sweeps.py` — renders `sweep_summary.{pdf,png}` from the CSVs
  (matplotlib, Agg backend; self-checks legend/label placement before
  exporting)
- `*.csv` — committed sweep results
- `sweep_summary.{pdf,png}` — committed six-panel figure
