# GPU-cluster alignment examples

Runnable demonstrations of every scenario where the simulator (and the
theory surrogate) has been aligned against a measured or independent
reference. Each script runs the pinned ns-3 commands, compares against the
reference data committed next to it, and regenerates the comparison
artifacts (`.json` results, `.pdf`/`.png` figures) in place.

The **fit machinery** that produced the calibrated constants (sweep +
least-squares scripts and their raw CSV/JSON outputs) lives in
`configs/calibration/`; the provenance of every constant is documented in
[doc/CALIBRATION.md](../../doc/CALIBRATION.md). C++ API examples for the
`gpu-cluster` module are under `src/gpu-cluster/examples/`.

All sweeps drive the debug build of `scratch/gpu-cluster-sim.cc` — build
first (`./ns3 configure -d debug --enable-tests --enable-examples &&
./ns3 build`). Individual ns-3 points take seconds to minutes; the full
sweeps run tens of points.

## Aligned scenarios

| Example | Reference | Alignment |
|---|---|---|
| `surrogate_ns3_consistency.py` | this repo's ns-3 (4 surrogate presets, N=8) | ring MAPE 1.87 % (a HW back-fit, see below), tree 0.56 % (anchored at the 18-link spray fabric), fullmesh 0.12 %; nvls preset sits 1.07–1.17× ABOVE ns-3 (its η=1.2 multicast amplification exceeds the 382500 B/µs trunk — not a lower bound) |
| H200 ring AllReduce (no script — single command below) | real NCCL H200-SXM5 scan (`surrogate/calibration/`, doc/CALIBRATION.md) | 8 GPU / 256 MiB: `simTimeUs=1353.58` vs HW 1347.65 µs (+0.44 %) |
| `demystify_nccl_sweep.py` | Demystifying NCCL (arXiv:2507.04786) Fig. 6, 4× GH200 measured LL/LL128/Simple × Ring/Tree | tree large-size ratios ≤ 3 % (confirms LL 0.5 / LL128 0.9375 wire efficiency); ring LL −21 % documented deviation |
| `roce_p2p_ns3_sweep.py` | ConnectX-7 400G `ib_write_lat` perftest curve (kubernetes.recipes blog) | latency-sweep MAPE 5.26 % |
| `roce_p2p_bw_sweep.py` | ConnectX-7 `ib_write_bw` single-QP ceiling (~49 Gbps) | with opt-in `--perQpCeilingGbps=49`: MAPE 3.20 % at ≥ 64 MB |
| `eth_gbn_loss_sweep.py` | SRNIC (NSDI'23) Fig. 11, ConnectX-6 go-back-N vs SRNIC selective repeat under injected loss | GBN arm within ~3 pts of CX-6 up to 0.5 % loss (no-RTO gap +10 pts at 1–2 %); SACK arm reproduces the flat SRNIC curve, no arm inversion |
| `plot_measured_anchors.py` | (plots only) | regenerates `srnic_fig11_comparison`, `crusoe_nccl_busbw`, `roce_p2p_perftest_comparison` from the committed `.json` results — fast, no ns-3 runs |

The H200 ring command (the one hardware-calibrated operating point,
`vcCredits=64` packets via the profile, credit-saturated regime):

```bash
./ns3 run "gpu-cluster-sim --protocolConfig=\
configs/protocol_configs/h200-ring-allreduce.cfg --numGpus=8 --dataSize=268435456"
# simTimeUs=1353.58   (HW: 1347.65 us, +0.44 %)
```

Tree and NVLS run over the switched fabric and need the full GPU↔NVSwitch
trunk on the CLI (`--linksPerGpu=18 --bandwidth=170` builds the 18 separate
point-to-point NVLink4 lanes, spray-routed, aggregate 3060 Gbps; see the
`.cfg` headers).

## Topology-comparison experiments

Not alignments against a measured reference — sweeps that compare
simulator configurations against each other:

| Experiment | What it compares |
|---|---|
| [`random_traffic_dcqcn/`](random_traffic_dcqcn/) | delivered throughput vs injection rate for the 8 `--interNodeTopology` values under uniform-random cross-node traffic (`--randTraffic`), RoCE 400G with DCQCN on; plus the ECN-mark congestion signal per topology |
| [`collective_topology_dcqcn/`](collective_topology_dcqcn/) | hierarchical allreduce + alltoall completion time and bus bandwidth vs data size (1–256 MiB) for the 8 `--interNodeTopology` values, 16 nodes × 8 GPUs (128 ranks), RoCE 400G with DCQCN on (via the So payload-builder seam); plus the `--algorithm=ncclring` (real-NCCL global pipelined ring) vs hierarchical comparison; plus the DCQCN parameter-sensitivity sweep (7 rate-machine knobs + marking threshold, two regimes); plus the lossy-RoCE (no-PFC, explicit `--voqDropThreshold`) sweep where those knobs actually move completion time |
| [`credit_bdp_sweep/`](credit_bdp_sweep/) | credit-window vs bandwidth-delay-product on the Su fabric: does the credit return delay cap utilization as bandwidth rises (5 bw × 5 latency × 6 windows, 8-GPU ring allreduce)? knee scales with W·chunk/(serialization + 2·lat) as predicted; the sawtooth panel shows the burst-drain-refill oscillation itself; plus the 1200-run pattern sweep (p2p/incast/fanout/alltoall × fullmesh/switched) — one shared per-endpoint window makes fanout/alltoall 7× p2p at W=1; on fullmesh (every pair a direct 1-hop link) p2p == incast exactly and the shared-window penalty shows up in utilization most sharply (fanout/alltoall still 0.26/0.23 at W=24) |
| [`surrogate_tree_validation/`](surrogate_tree_validation/) | revalidation of the paper fork's tree surrogate against the FIXED tree injector: 90 stratified paper-grid tree commands re-run on the post-`de16a7c` binary — the June paper tree ground truth is stale (mean sim drift 49.9%, volume fix ≈ 4-6× tree traffic) AND the paper grid uses the retired `--linkDataRate=80` guidance (256 MiB: 15775 µs @80 G vs 704 µs @3060 G on the same cell); follow-up fresh 36-point sweep (`tree_sweep_current.csv`, vcCredits=64 inline convention) exposed the 9/18 fit's structural defect (implied α_eff decays with D ⇒ pipeline fill is additive), absorbed into the surrogate as the α∞(N) + capped-fill tree model (`configs/calibration/tree_ns3_sweep.py`, 18-link spray fabric, MAPE 3.96%) |

## Notes

- Every script writes its outputs next to itself, so a re-run regenerates
  the committed artifacts in place; `git diff` shows any drift.
- These are alignment *reports*, not pass/fail gates. The enforced gates
  are `test/parity/` (bit-identity + regression pins) and the C++/pytest
  suites — see [CONTRIBUTING.md](../../CONTRIBUTING.md).
- The surrogate presets have different anchoring (HW back-fit vs ns-3
  back-fit vs derivation); `surrogate_ns3_consistency.py`'s docstring and
  doc/CALIBRATION.md spell out which is which.
