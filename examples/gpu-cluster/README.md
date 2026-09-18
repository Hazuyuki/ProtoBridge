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
| `surrogate_ns3_consistency.py` | this repo's ns-3 (4 surrogate presets, N=8) | ring MAPE 1.87 % (a HW back-fit, see below), tree 7.39 %, fullmesh 0.12 %; nvls is switched-link-rate-convention-dependent (0.09–0.16× vs the single-170G-link cfg default, 1.07–1.17× vs the `--linkDataRate=3060` full trunk) |
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
trunk on the CLI (`--linkDataRate=3060` = 18 × 170 Gbps; the profile's
`bandwidthGbps=170` is a per-lane value — see the `.cfg` headers).

## Notes

- Every script writes its outputs next to itself, so a re-run regenerates
  the committed artifacts in place; `git diff` shows any drift.
- These are alignment *reports*, not pass/fail gates. The enforced gates
  are `test/parity/` (bit-identity + regression pins) and the C++/pytest
  suites — see [CONTRIBUTING.md](../../CONTRIBUTING.md).
- The surrogate presets have different anchoring (HW back-fit vs ns-3
  back-fit vs derivation); `surrogate_ns3_consistency.py`'s docstring and
  doc/CALIBRATION.md spell out which is which.
