# Credit flow control: BDP-limited utilization & oscillation

**Question.** Can the packet-level Su (intra-node NVLink) simulator reproduce
credit-flow-control oscillation — where, as link bandwidth rises, the credit
return delay caps utilization below 100%?

**Answer.** Yes. The sweep below reproduces the phenomenon quantitatively: a
credit window of W packets (1 credit = one 8-MiB bulk chunk) sustains at most

```
W * chunkBytes / (serialization + 2 * latency)
```

so once the link bandwidth exceeds that, raising it further cannot shorten
completion — utilization = T_unlimited / T_W flattens below 1. The trace also
shows the oscillation itself: burst-drain-refill cycles whose idle gap is the
credit-return RTT (2·lat).

## Scenario

Single-node 8-GPU ring allreduce, 256 MiB per GPU — the su-parity scenario
(unlimited credits at 800 G / 500 ns reproduces the pinned 4769616 ns).
Swept: bandwidth 100–1600 G, one-way latency 0.5–200 µs, window 1–24 chunks
(+ unlimited).

**Prerequisite fix.** Before this experiment, the 11 single-node point-to-point
topology builds in `gpu-cluster-helper.cc` never called `SetLocalRankRange`,
so the constructor default (`base=0, count=1`) made `IsLocalRank` treat only
rank 0 as local: credit consume engaged only for rank-0 destinations and
credits were returned only for rank-0 sources. Squeezed windows then hit a
**deadlock cliff** (run to the 3600 s sim limit) instead of degrading
gracefully. The fix — `SetLocalRankRange(0, numGpus)` after each `SetRank`,
matching the `BuildMultiPlane` precedent — is parity-neutral: su pin 4769616
bit-identical (the per-packet CREDIT returns ride idle reverse links and never
bind at default 1e8 credits), and all other parity gates are unchanged.

## Key numbers (utilization = T_unlimited / T_W)

lat = 200 µs one-way:

| W \ Gbps | 100 | 200 | 400 | 800 | 1600 |
|---|---|---|---|---|---|
| 1 | 0.676 | 0.527 | 0.387 | 0.280 | **0.212** |
| 2 | 1.000 | 0.960 | 0.770 | 0.560 | 0.425 |
| 4 | 1.000 | 1.000 | 1.000 | 1.000 | 0.844 |
| 8+ | 1.000 | 1.000 | 1.000 | 1.000 | 1.000 |

- The window knee doubles with W exactly as the BDP law predicts (W=1 binds
  above ~100 G at 200 µs; W=2 above ~200–400 G; W=4 only at 1600 G).
- The knee scales with latency too (W=1, 1600 G: 0.98 → 0.212 as latency goes
  0.5 → 200 µs) because the credit return path pays 2·lat.
- Completion at 200 µs: W=1 falls 59.8 → 24.6 ms while unlimited falls
  40.4 → 5.2 ms — the W=1 line is nearly **flat past 200 G** (bandwidth
  cannot be spent), the signature of the phenomenon.
- Sawtooth (W=2, 800 G, 200 µs): 448 ConsumeCredit events in burst-drain-
  refill cycles; median idle gap between bursts 400 µs = 2·lat.

## Files

| File | What |
|---|---|
| `run_credit_bdp.py` | 150-run sweep (5 bw × 5 lat × 6 windows), writes `results_credit_bdp.csv` |
| `plot_credit_bdp.py` | four-panel figure from the CSV (+ regenerates the trace if missing) |
| `results_credit_bdp.csv` | sweep results (the table view for the figure) |
| `sawtooth_trace.log` | cached `NS_LOG=CreditManager=level_debug` run for the sawtooth panel |
| `credit_bdp.{pdf,png}` | the figure |

## Reproduce

```sh
python3 run_credit_bdp.py     # ~10 min, 16 parallel
python3 plot_credit_bdp.py    # reads the CSV + cached trace
```

Single-run example (the W=2 / 800 G / 200 µs sawtooth scenario):

```sh
build/scratch/ns3.47-gpu-cluster-sim-debug --numGpus=8 \
  --collective=allreduce --algorithm=ring --dataSize=268435456 \
  --bandwidth=800 --latency=200000 --vcCredits=2 --RngRun=1
```
