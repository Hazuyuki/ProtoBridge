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

**Switched fabric** (`--topology=switched`, every pair 2 hops) — lat =
200 µs, 800 G:

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
  at W=∞ they diverge (35.5 vs 19.3 ms) because alltoall's 56 concurrent
  flows contend network-wide while fanout only saturates the sender's egress.

**Fullmesh** (`--topology=fullmesh`, every pair a DIRECT 1-hop link) — the
cleanest isolation of the window-sharing effect: the per-chunk credit RTT is
uniformly ser + 2·lat (no hop-count confound, no switch traversal), lat =
200 µs, 800 G:

| Pattern | T_W=1 | T_∞ | vs p2p |
|---|---|---|---|
| p2p | 15.28 ms | 2.88 ms | 1.0× |
| incast | 15.28 ms | 2.88 ms | **1.00× (identical)** |
| fanout | 108.19 ms | 2.88 ms | **7.08×** |
| alltoall | 108.26 ms | 2.95 ms | **7.08×** |

- p2p == incast **exactly** (15.28 ms, bit-for-bit): 7 senders with
  independent windows over 7 dedicated links — no receiver bottleneck
  because rank 0's ingress is also 7 links wide.
- fanout/alltoall = 7.08× p2p = 224 chunks × the uniform 483.886 µs
  per-chunk RTT — pure shared-window serialization.
- T_∞ is line-bound at 2.88 ms (7 parallel egress links), so the same
  window yields the **lowest utilization of the three fabrics**: even at
  W=24, fanout/alltoall sit at 0.256/0.232 while p2p/incast reach 1.000.
  (Completion time still beats the switched fabric — 11.3 vs 19.3 ms —
  because the 1-hop credit RTT makes each window credit worth more.)

**Fixed-bandwidth view** (`plot_credit_wlat.py` →
`credit_patterns_wlat.{pdf,png}`, default 800 G / W=4; override with
`BW_GBPS=1600` or `CEIL_W=24`): utilization vs W with one line per one-way
latency. Reading it:

- The W needed for full utilization shifts right ~linearly with latency —
  the credit RTT is the horizontal asymptote of every curve.
- Bottom row at the small default W=4: every curve is window-bound and
  latency-responsive somewhere. On the switched fabric p2p falls
  1.000 → 0.418 and fanout 1.000 → 0.357 as latency rises 0.5 → 200 µs,
  while incast stays 1.000 at every latency (7 independent windows:
  4 credits × 7 senders cover the receiver-side BDP). On fullmesh p2p
  and incast fall together 1.000 → 0.735 (the 1-hop credit RTT makes each
  credit worth more, so the same W=4 binds only at the largest latency),
  and fanout/alltoall sit near-flat at ~0.15 → 0.11 (the shared window
  dominates at every latency).
- `CEIL_W=24` restores the ceiling view ("can this pattern reach 1.0 at
  all"): on the switched fabric only alltoall stays capped (0.711 → 0.613 —
  its 56 concurrent flows make even the large-window ceiling latency-bound);
  on fullmesh **fanout and alltoall both stay capped** (0.286 → 0.256 and
  0.253 → 0.232) because T_∞ there is 7 parallel links wide — the widest
  T_∞ of the two fabrics shows the largest shared-window penalty in
  utilization.

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
| `plot_credit_wlat.py` | fixed-bandwidth companion figure: utilization vs W, one line per latency (+ min-W-for-95% panel); `BW_GBPS` / `CEIL_W` env overrides |
| `results_credit_patterns.csv` | pattern sweep results (the table view) |
| `credit_patterns.{pdf,png}` | the pattern figure |
| `credit_patterns_wlat.{pdf,png}` | the fixed-bandwidth W/latency figure |

## Reproduce

```sh
python3 run_credit_bdp.py     # ~10 min, 16 parallel
python3 plot_credit_bdp.py    # reads the CSV + cached trace
python3 run_credit_patterns.py  # ~15 min, 16 parallel
python3 plot_credit_patterns.py
python3 plot_credit_wlat.py   # BW_GBPS=1600 / CEIL_W=24 to switch
```

Single-run example (the W=2 / 800 G / 200 µs sawtooth scenario):

```sh
build/scratch/ns3.47-gpu-cluster-sim-debug --numGpus=8 \
  --collective=allreduce --algorithm=ring --dataSize=268435456 \
  --bandwidth=800 --latency=200000 --vcCredits=2 --RngRun=1
```
