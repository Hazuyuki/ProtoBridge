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

**Second fix (pattern experiment).** `FabricEndpoint::Initialize()` and
`StartApplication()` lazily re-initialize any VC with zero available credits
to the 64-credit default — they cannot distinguish "never configured" from
"configured and already exhausted". Bare t=0 sends (scheduled in `main`
before the application-start events, which carry higher event uids) can
consume the whole window before `StartApplication` resets it to 64, silently
unsqueezing the experiment. `SetVcCredits(credits>0)` now marks the VC in
`m_configuredVcs`, and both lazy-init sites skip configured VCs. This only
affects zero-startup-delay sends; the allreduce sweep above (injector startup
delays push first sends after `StartApplication`) was unaffected —
verified by re-running its parity battery and the su pin.

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

## Patterns (1-to-1 / many-to-1 / 1-to-many / all-to-all)

`run_credit_patterns.py` extends the sweep beyond allreduce to four traffic
shapes on the same 8-GPU Su fabric, per-FLOW volume fixed at 256 MiB:

| Pattern | Flows | Windows involved |
|---|---|---|
| `p2p` (1-to-1) | 1 (rank0→rank1) | one dedicated window |
| `incast` (many-to-1) | 7 (ranks 1..7→rank0) | 7 independent per-sender windows |
| `fanout` (1-to-many) | 7 (rank0→ranks 1..7) | **ONE shared per-endpoint window** (1 VC) |
| `alltoall` | 56 (all↔all) | one shared window per rank (7 outgoing flows each) |

Each endpoint has a single VC (`SetNumVirtualChannels(1)`), so all flows
originating at one endpoint serialize through the same W credits.

**Switched fabric** (`--topology=switched`, every pair 2 hops) — the clean
window-sharing result, lat = 200 µs, 800 G:

| Pattern | T_W=1 | T_∞ | vs p2p |
|---|---|---|---|
| p2p | 30.6 ms | 3.2 ms | 1.0× |
| incast | 31.1 ms | 19.3 ms | 1.02× |
| fanout | 216.5 ms | 19.3 ms | **7.08×** |
| alltoall | 217.1 ms | 35.5 ms | **7.10×** |

(T_∞ is receiver-ingress-bound for incast and sender-egress-bound for fanout,
both ~725 Gbps effective on one 800 G link; alltoall's T_∞ is network-wide
contention.)

- p2p follows the per-flow BDP law exactly (W=1: 32 chunks × per-chunk RTT).
- incast ≈ p2p: the 7 senders have independent windows; the receiver-side
  ingress serialization only adds a few percent.
- fanout = **7.08× p2p**: the sender pushes all 224 chunks through ONE
  1-credit window (224 × per-chunk RTT) — the shared window throttles the
  aggregate, not each flow. Recovering p2p-level utilization needs
  ~W × flows credits, not W.
- alltoall ≡ fanout at W=1 (217.1 vs 216.5 ms): the shared window dominates;
  at W=∞ they diverge (44.4 vs 11.8 ms) because alltoall's 56 concurrent
  flows contend network-wide while fanout only saturates the sender's egress.

**Ring** (`--topology=ring`, the scratch default) — a second, additive
effect: the credit RTT is **hop-distance dependent**. rank0→rank_k costs
min(k, N−k) hops each way, and the trace shows per-chunk credit RTTs of
k·(ser + 2·lat) (chunks to rank 1/2/3 return at 1/2/3 × 483.886 µs). p2p
(rank0→rank1) is a direct 1-hop link, but fanout pays the hop sum:
224 chunks × Σ_min(k,8−k) = 16 × 483.886 ns = 247.7 ms, matching the
observed 247.55 ms to 0.1%. incast is gated by the farthest sender
(rank4, 4 hops: 32 × 4 × 483.886 = 61.9 ms observed 61.1 ms).

**Fixed-bandwidth view** (`plot_credit_wlat.py` → `credit_patterns.{pdf,png}`
figure `credit_patterns_wlat.{pdf,png}`, default 800 G, override with
`BW_GBPS=1600 python3 plot_credit_wlat.py`): utilization vs W with one line
per one-way latency. Reading it:

- The W needed for full utilization shifts right ~linearly with latency —
  the credit RTT is the horizontal asymptote of every curve.
- incast is the cheapest (7 independent windows: W=2 holds 1.000 even at
  200 µs); p2p next; fanout needs ~W×7 (W=24 just reaches 1.000 at 200 µs
  on switched); alltoall never gets there (0.711 → 0.613 as latency rises
  0.5 → 200 µs at W=24 — its 56 concurrent flows make even the
  large-window ceiling latency-bound).
- On the ring the roles swap at the ceiling: fanout is the capped one
  (W=24 = 0.64 flat — the hop sum makes each credit worth less), while
  alltoall nearly recovers (0.987 at 200 µs) because every rank's own
  window sees an average 2-hop distance, not rank0's worst case.

## Files

| File | What |
|---|---|
| `run_credit_bdp.py` | 150-run sweep (5 bw × 5 lat × 6 windows), writes `results_credit_bdp.csv` |
| `plot_credit_bdp.py` | four-panel figure from the CSV (+ regenerates the trace if missing) |
| `results_credit_bdp.csv` | sweep results (the table view for the figure) |
| `sawtooth_trace.log` | cached `NS_LOG=CreditManager=level_debug` run for the sawtooth panel |
| `credit_bdp.{pdf,png}` | the figure |
| `run_credit_patterns.py` | 1200-run pattern sweep (4 patterns × 2 topologies × 5 bw × 5 lat × 6 windows), writes `results_credit_patterns.csv` |
| `plot_credit_patterns.py` | 12-panel pattern figure from the CSV |
| `plot_credit_wlat.py` | fixed-bandwidth companion figure: utilization vs W, one line per latency (+ min-W-for-95% panel); `BW_GBPS` env overrides |
| `results_credit_patterns.csv` | pattern sweep results (the table view) |
| `credit_patterns.{pdf,png}` | the pattern figure |
| `credit_patterns_wlat.{pdf,png}` | the fixed-bandwidth W/latency figure |

## Reproduce

```sh
python3 run_credit_bdp.py     # ~10 min, 16 parallel
python3 plot_credit_bdp.py    # reads the CSV + cached trace
python3 run_credit_patterns.py  # ~15 min, 16 parallel
python3 plot_credit_patterns.py
python3 plot_credit_wlat.py   # BW_GBPS=1600 to switch the fixed bandwidth
```

Single-run example (the W=2 / 800 G / 200 µs sawtooth scenario):

```sh
build/scratch/ns3.47-gpu-cluster-sim-debug --numGpus=8 \
  --collective=allreduce --algorithm=ring --dataSize=268435456 \
  --bandwidth=800 --latency=200000 --vcCredits=2 --RngRun=1
```
