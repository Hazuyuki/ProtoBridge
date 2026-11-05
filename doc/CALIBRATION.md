# Calibration and validation

This guide records reference data, fitting conditions, comparison results
and reproduction commands. Protocol mechanisms are defined in
[Architecture](ARCHITECTURE.md), parameters in [Configuration](CONFIG_GUIDE.md),
and analytical equations in [Surrogate](SURROGATE.md#mechanism-equations).

The local GPU-fabric reference is NVIDIA H200 NVLink4. Third-party measurements,
fits to ns-3, and comparisons with ns3-rdma have separate scopes. Detailed
RDMA/IBGDA and PCIe defaults remain uncalibrated mechanism inputs.

## Reference map

| Case | Reference | What the recorded comparison establishes |
|---|---|---|
| H200 ring | H200 reference values and a committed ns-3 baseline | Workload-specific ring settings; credit and topology conventions matter. |
| RoCE P2P | Embedded third-party ConnectX-7 perftest data | Raw-RDMA latency/bandwidth parameters for the recorded setup. |
| Full-mesh | ns-3 sweep | A surrogate fit to packet simulation, without a hardware reference. |
| Tree | ns-3 sweep on the 18-lane fabric | A surrogate fit to packet simulation, without an H200 tree hardware reference. |
| Loss recovery | SRNIC Figure 11 / ConnectX-6 | Normalized recovery trends under random packet loss. |
| NCCL protocol shapes | Demystifying NCCL Figure 6 / four GH200s | Ring/tree and LL/LL128/SIMPLE trends under the mapped intra-node setup. |
| PFC and scale-out | Independent ns3-rdma runs | Matched simulator behavior with different pause criteria. |
| Shared contention | Native regression workloads | Controlled separation of link sharing and the optional WFQ penalty. |

Results below describe recorded experiments. Re-run their scripts to establish
numbers for a new commit or configuration. A fit to simulation does not supply
hardware timing for a different GPU, NIC, memory path or submission mode.

## H200 ring and baseline data

The hardware/protocol reference lives in
[`h200-ll128.profile`](../configs/protocol_profiles/h200-ll128.profile).
The eight-GPU, 1 MiB ring example produces 38.3 us against the documented
37.24 us hardware reference. Use the full workload conditions when comparing
other sizes; this single point is not a dataset-wide accuracy metric.

[`h200_ring4_ar_baseline.json`](../surrogate/calibration/h200_ring4_ar_baseline.json)
contains 27 **ns-3** observations: nine sizes, three seeds each, four GPUs and
64 KiB–1 GiB payloads. Its 400 Gb/s link override differs from the eight-GPU
profile's 170 Gb/s effective per-lane value. The JSON does not record a credit
window; do not treat it as an unqualified pin for arbitrary credit settings.

One baseline-style run is:

```bash
./ns3 run "gpu-cluster-sim --numGpus=4 --topology=ring --algorithm=ring --dataSize=1048576 --bandwidth=400 --latency=400 --startupLL=15000 --startupLL128=25000 --startupSIMPLE=46000" --no-build
```

The analytical ring preset uses 177000 B/us, fitted to the H200 reference;
the wire-rate factory gives 180000 B/us under its 18-lane/25000-B/us inputs.
Recorded eight-GPU/256-MiB predictions change from approximately 1347.65 us
at 128 credits to +42.3% at 64 credits. NVLS uses a separate multicast-derived
rate of 535500 B/us; recorded comparisons on the 18-lane spray fabric place
it about 1.07–1.17 times ns-3. These presets are not universal lower bounds.

The [consistency report script](../examples/gpu-cluster/surrogate_ns3_consistency.py)
compares the four schedule presets. Stored reports can use older constants;
use the fitted files and workload conditions below for current parameter provenance.

## RoCE P2P

The [calibration script](../examples/gpu-cluster/calibration/roce_p2p_calib.py)
embeds a third-party perftest reference: ConnectX-7 400G, RoCEv2, RC, MTU 4096,
TX depth 128. Extracted constants are stored in
[`roce_p2p_calib.json`](../configs/calibration/roce_p2p_calib.json).
This reference measures raw one-sided RDMA; it does not identify collective
launch, GPU WQE construction, congestion-control or PCIe TLP service costs.

| Constant | Value | How extracted | Fit quality |
|----------|-------|----------------|-------------|
| per-QP single-flow ceiling | 49.0 Gbps | single-QP `ib_write_bw` saturation | direct |
| wire efficiency | 0.988 | 8-QP saturation 395/400 = 0.9875 | direct |
| per-message fixed latency | 1.45 µs | 2-byte `ib_write_lat` (doorbell + prop + completion) | — |
| single-write serialization rate | ~199 Gbps | 2B→64 KB `ib_write_lat` slope (JSON stores 199.26; surrogate uses the round 192 Gbps = 24 k B/µs, which fits the linear endpoints better) | — |
| NIC message-issue rate | ~8.2 Mmsg/s | 2B/4B/64B `ib_write_bw` (∝ S; mean 8.24, 64 B point 8.16) | MAPE 1.6 % |
| `ib_write_lat` curve L(S)=a+b·S^c | a=1.45, b=1.65e-3, c=0.669 | 4-point `curve_fit` | MAPE 0.11 % |
| saturation knee | 4 KB | 90 % of ceiling | — |
| line rate | 400 Gbps | ConnectX-7 spec | match |

The analytical `make_connectx7_p2p_perftest_theory()` preset records 2.60% MAPE
against `ib_write_lat`. The packet-level latency sweep records 5.26% MAPE
with startup 950 ns, link latency 100 ns, switch service 200 ns and 400-Gb/s
links on a shared-leaf (`tor`) topology. The bandwidth sweep with a 49-Gb/s
per-QP ceiling records 3.20% MAPE for sizes at least 64 MB; its small-size
transition remains sensitive to chunk coalescing and message issue rate.

```bash
python examples/gpu-cluster/calibration/roce_p2p_calib.py
python examples/gpu-cluster/roce_p2p_ns3_sweep.py
python examples/gpu-cluster/roce_p2p_bw_sweep.py
```

Use the embedded reference and each script's recorded settings. The data gives
no independent validation of multi-hop scaling, PFC/DCQCN, GPU initiation or
collective completion. RoCE framing and flow-control mechanisms are described
in [Architecture](ARCHITECTURE.md#flow-control-and-congestion).

## Plain Ethernet

The plain-Ethernet surrogate is an uncongested approximation with
`wire_efficiency=1.0` and store-and-forward service. It has no measured
hardware anchor in this repository. Packet simulation executes loss, NACKs
and LLR recovery; the analytical estimate omits those stateful dynamics.
Use [the surrogate's Ethernet stage](SURROGATE.md#plain-ethernet-scale-out)
and the [Ethernet configurations](../examples/gpu-cluster/README.md#runnable-configurations)
for the corresponding interfaces.

```bash
./ns3 run "gpu-cluster-sim --protocolConfig=configs/protocol_configs/p2p-eth.cfg --numNodes=2 --gpusPerNode=2 --dataSize=65536" --no-build
```

## Full-mesh AllReduce/AllGather

The [sweep](../examples/gpu-cluster/calibration/fullmesh_ns3_sweep.py) runs
N in {4,8,16}, AllReduce/AllGather and 1–256 MiB on dedicated peer bundles.
The recorded set contains 54 points; 30 SIMPLE-regime points (`D/N >= 2 MiB`)
fit one schedule-bandwidth parameter. The fit uses 18 lanes at 200 Gb/s each,
400 ns link latency and 15 us SIMPLE startup.

| Constant | Value | Anchor |
|----------|-------|--------|
| `fullmesh_bw` | 449890 B/µs | ns-3 sweep least-squares fit |
| `eta_fullmesh` | 0.9998 | `fullmesh_bw / (18 × 25000)` = 99.98% of the dedicated-bundle physics |
| MAPE | 0.10% | 30 SIMPLE-regime fit points |

The raw sweep and fit are
[`fullmesh_sweep.csv`](../configs/calibration/fullmesh_sweep.csv) and
[`fullmesh_calib.json`](../configs/calibration/fullmesh_calib.json).
The fit straddles ns-3; its recorded worst deviation is about 0.31%. LL/LL128
sizes outside the fit range have different startup and wire efficiencies.
AllReduce uses two phases and AllGather one, giving
`T_AR ≈ 2*T_AG - startup` under the matched conditions.

```bash
python examples/gpu-cluster/calibration/fullmesh_ns3_sweep.py
python -m pytest test/parity/test_surrogate_vs_ns3_fullmesh.py -q
```

The parity band is a regression tolerance, separate from the recorded fit error.
No physical full-mesh hardware timing is supplied by this fit.

## Tree AllReduce

The [tree sweep](../examples/gpu-cluster/calibration/tree_ns3_sweep.py) uses
18 separate GPU-to-switch lanes at 170 Gb/s, 400 ns links, 131072-byte spray
chunks, startup 15/25/46 us and unconstrained credits (`vcCredits=104857600`).
The aggregate trunk is 382500 B/us. Its 36 points cover N in {4,8,16,32}
and 1–256 MiB. The model fits bandwidth and fixed switch service jointly;
the structural tree-load factor is defined in the
[schedule equations](SURROGATE.md#mechanism-equations).

| Fitted quantity | Recorded value |
|---|---:|
| Tree bandwidth used by the preset | 308300 B/us, rounded from the fit |
| Fixed switch service used by the preset | 1.06 us, rounded from the fit |
| All-point MAPE | 1.88% |
| SIMPLE-regime MAPE | 2.00% |
| Per-N MAPE, N=4/8/16/32 | 2.39 / 1.28 / 1.63 / 2.25% |

Exact fitted values are in [`tree_calib.json`](../configs/calibration/tree_calib.json);
observations are in [`tree_sweep.csv`](../configs/calibration/tree_sweep.csv).
At the recorded N=8/256-MiB point, 64 credits gives 3367.8 us versus 2699.4 us
with unconstrained credits. This difference belongs to the experiment's credit
convention. A single aggregated link also differs from separate spray-routed
lanes; use the topology that the fit records.

```bash
python examples/gpu-cluster/calibration/tree_ns3_sweep.py
# Refit the recorded CSV without packet simulations:
python examples/gpu-cluster/calibration/tree_ns3_sweep.py --refit
```

This is a fit to the current full-volume tree schedule in ns-3, not an H200
tree hardware calibration. Refit after changes to that schedule, cut-through
service, credits or lane layout.

## Loss recovery under packet loss

The measured reference is [SRNIC, NSDI 2023, Figure 11](https://www.cse.ust.hk/~kaichen/papers/srnic-nsdi23.pdf):
ConnectX-6 go-back-N and the SRNIC selective-repeat prototype under random
packet loss. Digitized data and provenance are in
[`srnic_fig11_cx6_reference.csv`](../examples/gpu-cluster/srnic_fig11_cx6_reference.csv).

| ns-3 arm | cfg | measured analogue |
|---|---|---|
| `llrMode=gobackn` | `configs/protocol_configs/p2p-roce-lossy.cfg` | ConnectX-6 |
| `llrMode=sack` | `configs/protocol_configs/p2p-roce-lossy-sack.cfg` | SRNIC |

The [sweep](../examples/gpu-cluster/eth_gbn_loss_sweep.py) uses a 100G RoCE
`tor` fabric, two GPUs per node, 4096-byte chunks, a 16-MiB transfer, a 500-us
retry timer and three seeds. SACK uses `soTxWindow=16`; strict GBN uses
`soGbnStrict=1` and `soTxWindow=64`. Each curve is normalized by its own
zero-loss goodput; absolute plateaus differ from the reference hardware.

| loss rate | ns-3 GBN (strict) | CX-6 measured | ns-3 SACK | SRNIC measured |
|---|---|---|---|---|
| ≤1e-4 | 0.976–0.995 | 1.000 | 1.000 | 1.000 |
| 1e-3 | 0.919 | 0.933 | 0.979 | 0.990 |
| 2e-3 | 0.861 | 0.889 | 0.977 | 0.979 |
| 5e-3 | 0.725 | 0.744 | 0.968 | 0.907 |
| 1e-2 | 0.580 | 0.467 | **0.814** | **0.773** |
| 2e-2 | 0.383 | 0.278 | 0.590 | 0.619 |

Recorded SACK goodput is within about six percentage points of its reference;
strict GBN is within about three points up to 0.5% loss and remains below SACK.
At 1–2% loss, strict GBN is about ten points optimistic. The model's timeout
backstop does not reproduce every long hardware RTO stall. Keep the different
per-arm windows and normalization when reproducing this table.

```bash
python examples/gpu-cluster/eth_gbn_loss_sweep.py
python -m pytest test/parity/test_so_gbn_strict.py -q
```

A separate collective-level comparison with the
[Crusoe cluster report](https://saturncloud.io/blog/crusoe-nccl-cluster-validation/)
has different topology, scale and barrier behavior. Its figures remain
qualitative context; they are not a measured calibration for this loss model.

## NCCL protocol shapes

*Demystifying NCCL* (arXiv:2507.04786), Figure 6, supplies intra-node Ring/Tree
AllReduce timings for four GH200s on CSCS Alps, NCCL 2.19.1, forced protocols,
sizes 2^14–2^32 B and medians of 20 runs. The
[digitized reference](../examples/gpu-cluster/demystify_nccl_fig6_reference.csv)
records extraction provenance. Inter-node Slingshot/host-proxy results have a
different network and are outside this mapped comparison.

| arm | ns-3 CLI | physics |
|---|---|---|
| ring | `--topology=ring --numGpus=4 --numLanes=18 --bandwidth=200` | NCCL multi-channel rings aggregate all 3 pair-links → 450 GB/s per-GPU egress (same modeling decision as the H200 ring preset) |
| tree | `--topology=fullmesh --numGpus=4 --numLanes=6 --bandwidth=200` | intra-node tree/chain edges are single GPU pairs → 150 GB/s per edge |

The [sweep](../examples/gpu-cluster/demystify_nccl_sweep.py) fits six startup
offsets at 2^14 B, then keeps bandwidth and topology fixed for 2^16–2^32 B.
Recorded per-curve MAPE is ring LL/LL128/SIMPLE = 25.1/21.1/18.0%, and tree
LL/LL128/SIMPLE = 5.8/8.1/16.3%. Ring is optimistic through the transition
range; large-size tree ratios are closer to the measurements. The 2^32-B
runtime ratios isolate protocol shape:

| ratio | measured | ns-3 | dev |
|---|---|---|---|
| tree LL/Simple | 2.005 | 1.998 | −0.3% |
| tree LL128/Simple | 1.073 | 1.066 | −0.7% |
| ring LL/Simple | 2.363 | 1.872 | −21% |
| ring LL128/Simple | 1.089 | 0.996 | −8.5% |
| tree/ring (Simple) | 1.435 | 1.787 | +24.5% |

These residuals include kernel/channel scheduling and hardware efficiency that
the model does not explicitly execute. Use the mapped topology and protocol
conditions rather than treating the ratios as a global accuracy score.

```bash
python examples/gpu-cluster/demystify_nccl_sweep.py
python examples/gpu-cluster/plot_measured_anchors.py
```

## PFC and scale-out cross-checks

The ns3-rdma comparison uses 40-Gb/s links, 1-us link latency, 50 MB per
sender, 1000-byte chunks and `soTxWindow=64`. A shared-leaf `tor` with one GPU
per node matches the reference star; `host` and `leafspine` add forwarding
depth. External ns3-rdma patches and trace fixtures are not shipped here.
The commands below reproduce the ProtoBridge side of the recorded comparison.

### Two-sender PFC comparison

| Scenario | FCT | PAUSEs | RESUMEs | Drops |
|---|---|---|---|---|
| ns3-rdma, static thr | 20.29 ms | 27,035 | 2,254 | 0 |
| ns3-rdma, dynamic thr (α=16) | 19.51 ms | 24,936 | 2,078 | 0 |
| ns3-rdma, PFC off (control) | 20.29 ms | 0 | 0 | 0 |
| ns3-rdma, DCQCN on (control) | ~20.3 ms | 0 | 0 | 0 |
| PB mode 1 (explicit RESUME), tor | 21.90 ms | 1,584 | 14,253 | 0 |
| PB mode 1 (explicit RESUME), host | 23.60 ms | 1,548 | 13,932 | 0 |
| PB mode 3 (quanta 5 µs), tor | 22.35 ms | 13,932 | 13,928 | 0 |
| PB mode 3 (quanta 5 µs), host | 24.38 ms | 13,932 | 13,932 | 0 |
| PB, PFC off (control), tor/host | 21.90 ms | — | — | 0 |
| PB mode 1, 9000-B chunks, tor | 21.15 ms | 2 | 18 | 0 |
| PB mode 1, 9000-B chunks, host | 21.15 ms | 182 | 1,638 | 0 |
| PB mode 3, 9000-B chunks, tor/host | 21.15 ms | 16,871 | 18 | 0 |
| PB, PFC off, 9000-B chunks, tor/host | 21.15 ms | — | — | 0 |

All 1000-byte ProtoBridge rows use the bounded window. The 9000-byte rows have
the same FCT with or without that window under these conditions. The stacks
use different pause criteria: ProtoBridge uses output/destination occupancy;
ns3-rdma uses ingress priority-group occupancy. Compare FCT/recovery under
matched settings; global pause/resume counts are not equivalent.

### Incast scaling

FCT in ms, 50 MB per flow:

| Scenario | 2:1 | 4:1 | 8:1 | Drops |
|---|---|---|---|---|
| ns3-rdma, static thr | 20.29 | 40.58 | 81.17 | 0 |
| ns3-rdma, dynamic thr (α=16) | 19.51 | 39.79 | 80.37 | 0 |
| ns3-rdma, PFC off (control) | 20.29 | 40.58 | 81.17 | 0 |
| PB tor, PFC off | 21.90 | 43.79 | 87.58 | 0 |
| PB tor, mode 1 (static) | 21.90 | 43.79 | 87.58 | 0 |
| PB tor, mode 2 (dynamic) | 21.97 | 43.80 | 87.59 | 0 |
| PB tor, mode 3 (quanta) | 22.35 | 44.31 | 87.88 | 0 |
| PB host, PFC off | 21.90 | 43.79 | 87.58 | 0 |
| PB host, mode 1 | 23.60 | 45.60 | 89.40 | 0 |
| PB leafspine, PFC off | 21.90 | 43.80 | 87.58 | 0 |
| PB leafspine, mode 1 | 23.85 | 45.74 | 89.45 | 0 |
| PB leafspine, mode 3 | 23.85 | 46.48 | 90.05 | 0 |

```bash
./ns3 run "gpu-cluster-sim --topology=switched --collective=incast --numNodes=3 --gpusPerNode=1 --interNodeTopology=tor --interNodeBandwidth=40 --interNodeLatency=1000 --dataSize=50000000 --bulkChunkSize=1000 --soTxWindow=64 --switchPfcMode=3 --pfcPauseThreshold=20 --pfcPauseTime=5 --pfcResumeOffset=8" --no-build
```

Sweep node counts 3/5/9, PFC modes 0–3 and the three topologies for the scaling
table. The [data-size/injection study](../examples/gpu-cluster/datasize_injection_sweeps/README.md)
extends this workload. Report drops and completion along with latency when
changing headroom or thresholds.

## Shared contention regression

The following recorded native durations are in ns: `RngRun=1`, 16-MiB
allreduce, 1-MiB background packets starting at 5 us. Set all contention
weights to 1.0 to remove the additional WFQ penalty while retaining the same
traffic and link competition. The mechanism is defined in
[Architecture](ARCHITECTURE.md#shared-contention-and-workload-schedules).

| regime | bg flow | default weights | weights=1.0 | baseline |
|--------|---------|-----------------|-------------|----------|
| 2-node RoCE hierarchical | rank 0→4 (cross-node) | 2776201 | 2776201 | 1248928 |
| 2-node RoCE hierarchical | rank 0→1 (intra-node) | 1330831 | 1315771 | 1248928 |
| single-node Su ring | rank 0→2 | 450982 | 405462 | 319658 |

The cross-node background case is dominated by link sharing; its two weighted
runs are identical. The local background cases expose the additional weighted
service effect. The source [regression](../test/parity/test_contention_interference.py)
specifies each command. These are simulator regressions, without an independent
hardware calibration for the contention service term.

## Reporting a new validation result

Record the Git commit, exact command and resolved profile, size/rank/iteration
counts, algorithm, topology/lane layout, credit/window settings, error model,
seed and completion condition. Identify which parameters were fitted and keep
fitting points separate from the points used to report error. Preserve the
reference's units and operation layer. The
[example catalog](../examples/gpu-cluster/README.md) locates each run/fit/plot
script and its data; [CONTRIBUTING.md](../CONTRIBUTING.md) gives the test gates.
