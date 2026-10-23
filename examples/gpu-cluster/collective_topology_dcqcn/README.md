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

The allreduce panels (a)/(c) show **`--algorithm=ncclring`** (from
`results_ncclring.csv`; see the ncclring section below for the full
analysis — leafspine/tor lead at 741 Gbps busbw, fullmesh/rail follow at
~655 because ring crossings must PXN-proxy there, gateway shapes converge
at ~377, and small sizes are 254-step latency-bound). The alltoall panels
(b)/(d) show the hierarchical algorithm (from `results.csv`; ncclring has
no alltoall variant — real-NCCL alltoall is direct P2P, which the
hierarchical injector already models).

For reference, the analytic **So wire bound for iso-hardware "one 400G
NIC per GPU"** (not drawn): counting only the inter-node bytes each GPU's
NIC must move, hierarchical allreduce is rail-parallel,
`2(n-1)/(g·n)·S ≈ 0.23S` per GPU → busbw bound
`(N-1)/(n-1)·B ≈ 3387 Gbps`; alltoall moves the `(N-g)/N ≈ 0.94S`
inter-node fraction → bound `(N-1)/(N-g)·B ≈ 423 Gbps`.

Hierarchical findings (`results.csv`; the hierarchical-allreduce rows are
no longer plotted, the alltoall rows are panels b/d):

- **fullmesh** wins both collectives, but it is NOT an iso-cost ideal: the
  builder gives every node pair one link per rail (same-local-rank GPUs
  directly connected), i.e. 15 dedicated 400G links per GPU — ~15× the
  switched shapes' NIC bandwidth. On alltoall it crosses *below* the
  iso-hardware wire bound from 16 MiB on (~694 vs 423 Gbps busbw at
  256 MiB): the margin is extra hardware, not better switching. No switch
  in the So path -> ECN never marks.
- **hierarchical allreduce is not So-limited at this scale**: the wire
  bound sits ~1 decade below every hierarchical curve, and raising the So
  bandwidth 10×
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

## DCQCN parameter sensitivity

```bash
python3 run_dcqcn_param_sweep.py     # 46 runs: 5x3x3 grid + dcqcn=0 baseline, host ncclring 256 MiB -> results_dcqcn_params.csv
python3 run_dcqcn_rand_sweep.py      # 46 runs: same grid on randTraffic 4:1 leafspine -> results_dcqcn_rand.csv
python3 plot_dcqcn_params.py         # -> dcqcn_params.{pdf,png}
```

The 7 rate-machine knobs are CLI-exposed (0 = TypeId default):
`--dcqcnTFastNs/--dcqcnFastCycles/--dcqcnTAiNs/--dcqcnRai/--dcqcnGInit/--dcqcnGDecay/--dcqcnAlphaFloor`,
plus `--ecnThreshold` on the switch side. Sweep axes: marking threshold
{1,4,8,16,32} x GInit {0.25,0.5,0.75} x TFast {10,50,100 µs}.

Findings — **the alpha parameters are outcome-neutral in this lossless
model**, across both regimes:

- **Collective regime** (host + ncclring 256 MiB, the only collective
  scenario where ECN fires, ~32k marks): all 45 grid points AND the dcqcn=0
  baseline complete at exactly 11702.3 µs — nanosecond-identical.
  Structural cause: ncclring's flowId = 1 + channel·steps + step is
  **one-shot** (each flow carries a single 2-3-packet burst dispatched
  back-to-back), so the CNP round trip — ≥1 RTT plus the queue wait that
  earned the mark — always returns after the flow has finished sending:
  Alpha rises on dead keys. `--soTxWindow=64 --llrEnabled=1` (ACK-clocked
  dispatch) is also invariant: each endpoint holds ≤8 in-flight chunks
  (< the window) and each flow still sends only once.
- **Sustained-flow regime** (randTraffic on 4:1-oversubscribed leafspine,
  0.8 injection; the random-traffic flowId is the source rank, so Alpha CAN
  throttle the flow's later messages, and CNP feedback does arrive):
  delivered 5136–5171 Gbps across the extreme corners vs 5140.6 at
  dcqcn=0 (±0.4 %). A soft per-chunk deferral (≤1× the chunk's
  serialization time, rateFactor ≥ 0.5) is absorbed by lossless deep-VOQ
  bottleneck queueing: it relocates the wait from the switch VOQ to the
  sender, it does not reduce the bottleneck's served rate. Contrast: the
  hard pacers DO move outcomes — RTTCC (`--rttcc`) pins 68664→73700 ns on
  the incast scenario, and `--perQpCeilingGbps` caps sustained rate.
- **What does respond is the marking threshold** — the switch-side knob:
  ECN-marked packets fall −2 % (collective) / −11 % (randTraffic) over
  threshold 1→32, and delivered CNPs fall 1052→496. Marking is live
  whenever the So payload builder is mounted (it needs no `--dcqcn`), so
  the counters are a congestion *signal*, not a control loop.

Implication for calibration: GInit/TFast/TAi/RAI/GDecay cannot be tuned
against any throughput or completion-time observable in the lossless
fabric — the ECN→CNP→DCQCN chain is a counters-observable there. It would
become outcome-relevant only under lossy/finite queues (the plain-ETH
tail-drop path) or hard pacing, which is why the RoCE fabric here pairs
DCQCN (soft) with the opt-in per-QP pacer when a rate cap is wanted.

## Lossy-RoCE DCQCN sensitivity (where the knobs finally bite)

```bash
python3 run_lossy_roce_sweep.py   # 47 runs: lossy-RoCE OAT + loss-severity, hier AR 2n×8g -> results_lossy_roce.csv
python3 run_lossy_rand_sweep.py   # 10 runs: same lossy regime on saturated randTraffic -> results_lossy_rand.csv
python3 plot_lossy_roce.py        # -> lossy_roce_dcqcn.{pdf,png}
```

The lossless sweeps above left the alpha knobs outcome-neutral. Opting the
RoCE fabric into the lossy (no-PFC) regime — an **explicit**
`--voqDropThreshold=N` (N>0; the switch default is 0 = lossless, and the
scratch CLI default 8 only applies to plain Ethernet) with
`--ecnThreshold < N` for mark-before-drop — gives the chain a real job:
keep the VOQ below the drop threshold, because each tail-drop costs a
NACK→LLR retransmission stall. LLR is force-enabled with the opt-in.

Findings (2n×8g hierarchical allreduce 256 MiB, drop=8, ecn=4):

- **Drops are expensive for completion time**: lossless 15.25 ms → lossy
  no-CC 20.20 ms (+32 %, 816 drops / 892 retransmissions). Default DCQCN
  recovers to 18.35 ms (37 % of the excess); the best corner
  (GDecay=0.125) reaches 17.93 ms (46 %).
- **The step-size knobs bite**: one-at-a-time swings vs the default run —
  GInit 7.9 % (U-shaped: 0.0625 under-backs-off → 19.44 ms; 0.125–0.25
  → 18.01 ms; 0.5–1.0 → 18.35 ms), GDecay 8.4 % (0.125 best, 1.0 →
  19.44 ms), AlphaFloor 6.4 % (0.25 best). The recovery timers are flat:
  TFast 0 %, FastCycles 0 %, TAi ≤1.8 %, RAI ≤7.9 % only at 0.1.
- **The benefit is threshold-severity-gated**: at drop=16 the 344 drops
  cost 0.4 % and all CC arms are identical; at drop=32 nothing drops. The
  GBN retransmission amplification is what turns a deep-drop regime into
  a +32 % completion tax.
- **Saturation contrast** (randTraffic 4:1 leafspine, same lossy regime):
  delivered 644–647 Gbps vs 642 lossless (±0.7 %) with ~4.4 k drops and
  ~300 k retransmissions per run — a saturated bottleneck serves
  retransmissions instead of new data at the same rate, so goodput is
  drop-free while completion time is not. DCQCN choice is irrelevant
  under saturation; it matters exactly where a collective's completion
  deadline is the metric.
- Part of the dcqcn=1 delta is framing, not CC: `--dcqcn` also mounts the
  RoCE SoHeader payload builder (+10 B/packet), which shifts timing even
  with zero CNPs (21.15 → 19.01 ms at ecn=8=drop). The alpha-path
  contribution is measured with marks live (ecn=4 < drop=8).

### Same-form grid sweep (the lossless figure's layout, lossy data)

```bash
python3 run_lossy_roce_grid.py   # 47 runs: 5(ecn) x 3(GInit) x 3(TFast) grid + dcqcn0/lossless refs, drop=8 -> results_lossy_grid.csv
python3 plot_lossy_roce_grid.py  # -> lossy_roce_grid.{pdf,png}
```

The lossless figure (`dcqcn_params.png`) had 45 uniform heatmap cells
(11702.3 µs everywhere). The same 3x3 layout under the lossy opt-in
(ecn thresholds clipped to {1,2,4,6,8} — mark-before-drop requires
ecn <= drop=8; GInit around the OAT optimum) actually varies:

- **Early marks win**: best cells sit at thr 1–2 (best 16675.5 µs at
  ecn=2 — recovers **71 %** of the drop tax); thr 4 matches the OAT
  defaults (18.0–18.3 ms).
- **Late marks backfire**: thr 6 (marks at depth 6–7, just before the
  drop) is the WORST column (~19.3 ms mean) — worse than thr 8 (no
  marks possible, 18.9 ms, pure framing). The CNP round trip from a
  depth-6 mark arrives after the incast burst already reshaped the
  queue: the backoff lands on the recovery phase and throttles the
  retransmissions themselves.
- **TFast is live only at early thresholds** (OAT at thr 4 showed 0 %):
  within a (ecn=2, GInit=0.5) column the three TFast columns span
  16675→18560 µs (~11 %); at thr ≥ 4 the columns are identical.
- Retransmission heatmaps (row 2) separate the regimes: no-CC 892,
  thr=8 978 (framing only), marked cells 1040–1228 — more marks, more
  retransmissions, LESS completion time (shaping trades cheap deferral
  for expensive drop stalls).
