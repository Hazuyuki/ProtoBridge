# Hierarchical collectives vs So topology (16 nodes x 8 GPUs, DCQCN)

Deterministic-traffic counterpart of
[`random_traffic_dcqcn/`](../random_traffic_dcqcn/): compares the 8
scale-out (`--interNodeTopology`) topologies under the calibrated
hierarchical collectives instead of a synthetic random load.

## Setup

- 16 nodes x 8 GPUs (128 ranks), RoCE 400 Gbps So links, 1 us link latency;
  Su fabric is the calibrated NVLink/NVSwitch model.
- `--collective=allreduce|alltoall --algorithm=hierarchical`, data size
  1..256 MiB per GPU (x4 steps), `--dcqcn=1`, `RngRun=1`.
- Bus bandwidth uses the NCCL convention with N = 128 ranks:
  allreduce `2(N-1)/N * S/t`, alltoall `(N-1)/N * S/t`.
- DCQCN on an inline collective requires the So payload-builder seam:
  `--dcqcn=1` on a RoCE fabric mounts a dedicated `RocePayloadBuilder` for
  inter-node sends (SoHeader + `RoceProtocol::SIMPLE`, which the switch
  ECN-mark and receiver CNP-echo guards key on) while intra-node (Su)
  traffic keeps the NCCL builder's framing. With `--dcqcn=0` (default) the
  seam is unmounted and runs are byte-identical to before.

## Run

```bash
./ns3 build                       # from the repo root
python3 run_sweep.py              # 80 runs (2 collectives x 8 topologies x 5 sizes) -> results.csv
python3 plot_collectives.py       # -> collective_vs_topology.{pdf,png}
```

## Reading the figure

The dashed line is the **analytic So wire bound for iso-hardware "one 400G
NIC per GPU"** — the completion-time floor (busbw ceiling) of an ideal
contention-free non-blocking fabric at the switched shapes' NIC count,
counting only the inter-node bytes each GPU's NIC must move: hierarchical
allreduce is rail-parallel, `2(n-1)/(g·n)·S ≈ 0.23S` per GPU → busbw bound
`(N-1)/(n-1)·B ≈ 3387 Gbps`; alltoall moves the `(N-g)/N ≈ 0.94S`
inter-node fraction → bound `(N-1)/(N-g)·B ≈ 423 Gbps`.

- **fullmesh** wins both collectives, but it is NOT an iso-cost ideal: the
  builder gives every node pair one link per rail (same-local-rank GPUs
  directly connected), i.e. 15 dedicated 400G links per GPU — ~15× the
  switched shapes' NIC bandwidth. On alltoall it crosses *below* the
  iso-hardware wire bound from 16 MiB on (~694 vs 423 Gbps busbw at
  256 MiB): the margin is extra hardware, not better switching. No switch
  in the So path -> ECN never marks.
- **allreduce is not So-limited at this scale**: the wire bound sits ~1
  decade below every curve (panel a), and raising the So bandwidth 10×
  moves tor only from 11.8 to 9.4 ms — completion is dominated by the
  intra-node phases plus per-phase startup/ramp. That is why the switched
  shapes cluster tightly (~340–360 Gbps) and even fullmesh only reaches
  ~463: topology differences show up muted, through the inter-node phases'
  share of a mostly intra-bound total. `tor` and `rail` are bit-identical
  for allreduce (same effective So paths) but diverge slightly on alltoall.
- **alltoall is So-limited**, so topology separates strongly there: the
  switched shapes reach ~38–50% of their wire bound (~160–210 Gbps; the
  gap is 15-to-1 incast at each destination NIC plus switch-hop
  serialization), with DCQCN active (ECN/CNP counters in `results.csv`).
- **ring** is worst at 128 ranks (allreduce ~123 Gbps, alltoall ~21 Gbps):
  all cross-node traffic of a node funnels through GPU 0's two ring links
  and multi-hop forwarding grows with node count. **torus2d / dragonfly**
  sit in between, ordered by gateway path diversity. The gateway shapes
  have no EthernetSwitch in the So path, so ECN is structurally zero
  there (same finding as the random-traffic sweep).
- As in the random-traffic experiment, DCQCN in this lossless fabric does
  not change completion time (a saturated bottleneck port stays busy either
  way; DCQCN relocates queueing to the senders) -- its effect shows as the
  ECN/CNP activity counters, not as a latency delta.

Smoke gate: `python3 test/parity/test_collective_dcqcn.py` (run directly)
pins the seam's invariants: `--dcqcn=0` stays dormant (ecn=0, unchanged
completion), `--dcqcn=1` lights the ECN->CNP chain on an oversubscribed
leafspine allreduce.

## ncclring: real-NCCL global pipelined ring (allreduce)

```bash
python3 run_ncclring_sweep.py     # 40 runs (8 topologies x 5 sizes, --algorithm=ncclring) -> results_ncclring.csv
python3 plot_ncclring.py          # -> ncclring_vs_hierarchical.{pdf,png} (solid=hierarchical, dashed=ncclring)
```

`--algorithm=ncclring` replaces the 4-phase barrier-separated hierarchical
schedule with what NCCL's ring algorithm actually does: 8 channels
(= gpusPerNode), each a global ring over all 128 ranks, rail-rotated so
channel c enters each node at local GPU c and exits at local GPU (c+7)%8 —
one NIC in / one NIC out per ring per node, all 8 rails crossing
concurrently. Each channel moves S/8 in 2(N-1)=254 chunk steps
(chunk = S/1024; 256 MiB -> 256 KiB, NCCL SIMPLE regime), fully
event-driven with no barriers, so intra- and inter-node hops pipeline.

Findings (vs the hierarchical rows in `results.csv`):

- **Large sizes: pipelining converts sum-of-phases into
  max-of-bottleneck.** At 256 MiB, tor/leafspine reach **741 Gbps busbw
  vs hierarchical's 362 (2.05x)** — the So NIC, intra-node hops, and
  reduction all overlap instead of waiting on phase barriers.
- **tor/leafspine BEAT fullmesh under ncclring** (741 vs 655 Gbps) —
  inverted from hierarchical. Ring crossings are cross-rail
  ((n, c−1) -> (n+1, c)): a switch reaches any NIC in one hop, while
  fullmesh/rail only wire same-local-rank pairs and must PXN-proxy the
  crossing through the rail peer plus an intra-node hop. This is the real
  NCCL PXN situation, reproduced faithfully.
- **Small sizes are latency-bound**: 254 steps put a ~810 us floor under
  every topology at 1 MiB (vs hierarchical's ~270 us) — precisely why real
  NCCL switches to tree at small sizes; the crossover here is ~16-32 MiB.
- **Gateway shapes (ring/torus2d/dragonfly) converge under ncclring**
  (~377 Gbps at 256 MiB, vs 123-215 hierarchical): with only one crossing
  per node per channel, the GPU-0 funnel serializes 8 channels but no
  longer multiplies with hop count the way phase-synchronized bulk
  transfers did.
- **`host` is the one topology where DCQCN fires under ncclring** (~32k
  ECN marks at >=16 MiB): a single central switch sees all 128 NICs'
  concurrent chunk streams, enough incast depth to mark; every other
  shape's per-hop load stays below the marking threshold.

Gate: `python3 test/parity/test_ncclring.py` (run directly) pins
2n x 8g leafspine allreduce 16 MiB at simTimeNs=444189 and
ncclring < hierarchical on the same config.
