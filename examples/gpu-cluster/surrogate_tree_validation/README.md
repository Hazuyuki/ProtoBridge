# Tree surrogate ground-truth revalidation (fixed ns-3 vs both tree models)

Question: should the paper fork's `whitebox_surrogate_v2.py`
(`doc/paper/experiments/`, parent repo) absorb the 2026-09-18 tree
recalibration from `protobridge/surrogate/theory/whitebox_surrogate_v2.py`
(commit `f984edb`, alpha(N) trunk-load model fitted against the
volume-fix tree injector `de16a7c`)?

Answer: **yes for the model, but the paper's tree ground truth must be
regenerated first** — and the paper grid itself has two simulator-facing
problems that dominate everything else (see findings 1-2). Purely from
model quality, the recalibrated model is strictly better on large sizes
and the old formula is unsalvageable (up to 9x off at 256 MiB).

## What was done (parent repo read-only; all writes inside protobridge)

1. `run_tree_validation.py` — re-ran 90 stratified tree configs from the
   paper's exact command list (`commands/exp3_1_commands.csv`, seed 1)
   on the FIXED protobridge binary
   (`build/scratch/ns3.47-gpu-cluster-sim-debug`, post-`de16a7c`).
   All 90 complete. NOTE: the paper commands point at the PARENT binary
   (`build/scratch/ns3.47-gpu-cluster-sim`), which is currently BROKEN
   (`undefined symbol: FailOpticalInterSwitchLink`) — redirected here.
2. `eval_tree_validation.py` — evaluates BOTH tree models against the
   fixed-ns-3 reruns and against the June paper values for the same cells.

## Findings

### 1. The paper grid's tree link rate is wrong (pre-fix guidance)

Every tree command in the paper grid uses `--linkDataRate=80` (the retired
pre-fix guidance). The volume-fix commit retired 80/100/50 Gbps; the current
fabric convention is `--linksPerGpu=18` @ 170 Gbps per lane, spray-routed
(18 × 170 Gbps full trunk; the `--linkDataRate=3060` single-pipe form is
also retired). Same
cell, same fixed binary: 256 MiB @ cr=32 runs **15775 µs at 80 G vs
704 µs at 3060 G — a 22× difference**. The tree trunk is the bottleneck
resource, so the link-rate choice IS the experiment.

### 2. The old tree ground truth is stale on two axes

Sim drift on our 90 matched cells (June parent binary vs fixed binary,
same commands): mean |Δ| **49.9%** — the volume fix multiplies tree
traffic ~4-6×, so all June tree rows underestimate. The paper CSV tree
"ground truth" is not usable for calibrating or evaluating any tree model
against the current code.

### 3. Model quality on FIXED ns-3, per size (old formula -> recal)

| size | old MAPE | recal MAPE |
|---|---|---|
| 4 KiB | 4.4% | 47.3% (startup-dominated; recal T_min_switch overshoots tiny sizes) |
| 64 KiB | 31.6% | **6.3%** |
| 1 MiB | 67.8% | **49.7%** |
| 16 MiB | 86.7% | **78.7%** |
| 256 MiB | 86.7% | **78.4%** |

The large-size residuals come almost entirely from finding 1 (those runs
are at the wrong link rate); the recal model was fitted at 3060 G, where
the old formula is up to **905% off** (n=4, 256 MiB) while the recal
model reproduces its own fit dataset at **7.59% MAPE** (verified
end-to-end: startup 15/25/46 µs regimes + cr=64, the profile's actual
credit depth — the commit's headline number, reproduced exactly).

### 4. Cross-check on the paper's own (stale) data

For the record: on the June data the old formula scores better overall
(23.5% vs 29.1%) — but that data encodes the volume bug AND the wrong
link rate; per size the recal model already wins at 1-256 MiB (48.7% ->
18.7% at 16 MiB).

## Recommendation

1. Regenerate the tree ground truth with the fixed binary at the TRUE H200
   fabric (`--linksPerGpu=18` @ 170 Gbps, spray-routed; and revisit ring's
   170/nvls' 850 `--bandwidth` choices for consistency), then re-fit/evaluate.
2. Absorb the recal tree model (alpha(N) trunk-load + 270653 B/µs +
   T_min_switch 2.99 µs) into the paper fork — with the fixed ground
   truth it is expected to land near its fit MAPE (~5-8% on SIMPLE).
3. Keep the tiny-size startup term from the old model or refit
   T_min_switch jointly (4 KiB rows are startup-dominated and the recal
   constant overshoots them ~47%).
4. Narrative cost: the recal constants are ns-3 back-fitted (eta=0.7076),
   not purely physics-derived — same framing already used for fullmesh
   (0.12% MAPE). The paper's "0 fitted parameters" claim needs updating
   either way.

## Resolution (2026-10-16, absorbed into the surrogate)

The follow-up fresh 36-point sweep (`tree_sweep_current.csv`, current binary,
profile convention with `--vcCredits=64`) exposed the structural defect
behind finding 3's residuals: the 9/18 model's implied alpha_eff DECREASES
with D (N=8: 5.13 at 8 MiB -> 2.67 at 256 MiB), i.e. pipeline fill is an
ADDITIVE per-step byte volume, not a multiplicative alpha factor. Absorbed
into `surrogate/theory/whitebox_surrogate_v2.py` as

refit on the canonical sweep (`configs/calibration/tree_sweep.csv`, 36 points
incl. N=32) at the TRUE H200 fabric — `--linksPerGpu=18` separate p2p NVLink4
lanes @ 170 Gbps, spray-routed, credits unconstrained (the earlier
`--linkDataRate=3060 --linksPerGpu=1` convention collapsed the 18 lanes into
ONE pipe and is retired; same cell N=8/256 MiB: 2851.1 -> 2699.4 µs):

    T = S(D) + steps*1.20us + (alpha_inf(N)*D + min(2.76MiB*steps, 3.45*D)) / 270653
    alpha_inf(N) = 1.0/1.62/2.59/2.74/2.80 (N<=2 / <=4 / <=8 / <=16 / >=17)

MAPE 3.96% all / 2.30% SIMPLE (old constants on the 9/18 data: 12.4%);
consistency regression tree preset 11.22% -> 0.56% at the 18-link fabric.
See `configs/calibration/
tree_ns3_sweep.py` and the rewritten tree section in `doc/CALIBRATION.md`.

## Files

| file | what |
|---|---|
| `run_tree_validation.py` | rerun driver (90 stratified paper tree commands on the fixed binary) |
| `tree_runs.txt` | the sampled command list (binary path redirected to protobridge) |
| `results_tree_rerun.csv` | fixed-ns-3 results, 90/90 complete |
| `paper_seed1_tree.json` | June parent-binary values for the same 90 cells |
| `eval_tree_validation.py` | dual-model evaluation |
| `tree_validation.csv` / `tree_validation_report.txt` | per-cell and summary outputs |
| `tree_sweep_current.csv` | fresh 36-point sweep (N=4/8/16/32 x 1-256 MiB, vcCredits=64 inline convention) that exposed the fill-term defect |
