# ProtoBridge example catalog

Run commands from the repository root. The main command-line simulator lives
at [`scratch/gpu-cluster-sim.cc`](../../scratch/gpu-cluster-sim.cc); the examples
below use that application or show how to call the module from C++.

```bash
./ns3 configure --enable-examples --enable-tests -d debug
./ns3 build gpu-cluster-sim gpu-cluster-example ring-topology-example hybrid-cluster-example protocol-profile-demo
```

The C++ examples require `--enable-examples`. Several experiment scripts call
the debug simulator directly, so use the debug build above for those scripts.
Full sweeps run many simulations and regenerate their recorded CSV/JSON or
figures; the single-configuration examples are the quickest starting point.

## C++ API examples

All four live in [`src/gpu-cluster/examples/`](../../src/gpu-cluster/examples/)
and are registered with the module build.

| Executable | Purpose |
|---|---|
| `gpu-cluster-example` | Build endpoints and exchange packets on point-to-point links. |
| `ring-topology-example` | Build a ring with an optional link-degradation model. |
| `hybrid-cluster-example` | Connect GPU fabrics through Ethernet gateways. |
| `protocol-profile-demo` | Load a `.profile` and inspect the protocol/FEC/credit bundle; no traffic is simulated. |

```bash
./ns3 run "gpu-cluster-example --numGpus=2 --numPackets=4" --no-build
./ns3 run "ring-topology-example --numGpus=4" --no-build
./ns3 run "hybrid-cluster-example --gpusPerNode=2" --no-build
./ns3 run "protocol-profile-demo --profile=configs/protocol_profiles/h200-ll128.profile" --no-build
```

## Runnable configurations

The 19 files in [`configs/protocol_configs/`](../../configs/protocol_configs/)
are inputs to `gpu-cluster-sim`, not separate programs.

| Configuration | Scenario |
|---|---|
| `h200-ring-allreduce.cfg` | Calibrated H200 ring allreduce. |
| `h200-tree-allreduce.cfg` | H200 tree allreduce. |
| `h200-nvls-allgather.cfg` | H200 NVLS allgather. |
| `h200-request-response.cfg` | Request/response transfer stencil. |
| `p2p-single.cfg` | One scale-up point-to-point transfer. |
| `p2p-roce.cfg` | Scale-out RoCE point-to-point transfer. |
| `p2p-roce-lossy.cfg` | RoCE with loss and go-back-N recovery. |
| `p2p-roce-lossy-sack.cfg` | RoCE with loss and selective recovery. |
| `p2p-eth.cfg` | Plain Ethernet point-to-point transfer. |
| `p2p-eth-incast.cfg` | Plain Ethernet incast. |
| `eth-hierarchical.cfg` | Hierarchical collective over Ethernet scale-out. |
| `rdma-host-p2p.cfg` | CPU initiation, host-memory payload. |
| `rdma-staged-p2p.cfg` | CPU initiation, GPU payload through host staging. |
| `rdma-gpudirect-p2p.cfg` | CPU initiation, direct GPU-memory payload. |
| `ibgda-p2p.cfg` | GPU initiation with GPUDirect payload. |
| `rdma-read.cfg` | Addressed RDMA Read/Get. |
| `rdma-send.cfg` | Send/Recv with posted receives. |
| `rdma-atomic.cfg` | 64-bit RDMA atomic. |
| `rdma-put-signal.cfg` | Payload followed by a signal update. |

Single-node example:

```bash
./ns3 run "gpu-cluster-sim --protocolConfig=configs/protocol_configs/h200-ring-allreduce.cfg --numGpus=8 --dataSize=1048576" --no-build
```

Detailed multi-node RDMA example, with optional PCIe TLP execution:

```bash
./ns3 run "gpu-cluster-sim --protocolConfig=configs/protocol_configs/rdma-read.cfg --numNodes=2 --gpusPerNode=2 --dataSize=9000 --pcieModel=tlp --pcieTraceFile=/tmp/pcie.csv" --no-build
```

The three reusable profiles are `h200-ll128.profile`, `rdma-common.profile`
and `ibgda-roce.profile`. See [Configuration](../../doc/CONFIG_GUIDE.md) for
precedence and authoring, [RDMA](../../doc/RDMA.md) for operations and memory
paths, and [PCIe](../../doc/PCIE.md) for TLP settings. RDMA/IBGDA example costs
are illustrative; the calibration guide records which other inputs are fitted.

## Reference comparisons

For Python sweeps and figures, install the calibration and plotting dependencies:

```bash
python -m pip install -r surrogate/requirements.txt matplotlib
```

| Script | Reference and output |
|---|---|
| [`surrogate_ns3_consistency.py`](surrogate_ns3_consistency.py) | Compare ring/tree/fullmesh/NVLS surrogate predictions with packet simulation; writes CSV/JSON. |
| [`roce_p2p_ns3_sweep.py`](roce_p2p_ns3_sweep.py) | Compare RoCE P2P latency against a third-party perftest curve. |
| [`roce_p2p_bw_sweep.py`](roce_p2p_bw_sweep.py) | Sweep the per-QP bandwidth ceiling against the perftest bandwidth curve. |
| [`eth_gbn_loss_sweep.py`](eth_gbn_loss_sweep.py) | Compare go-back-N and selective recovery with the SRNIC loss-recovery reference. |
| [`demystify_nccl_sweep.py`](demystify_nccl_sweep.py) | Compare NCCL protocol shapes with digitized Ring/Tree measurements. |
| [`plot_measured_anchors.py`](plot_measured_anchors.py) | Plot the recorded SRNIC, NCCL and perftest comparisons. |

Measured-reference CSVs, recorded JSON/CSV results and comparison figures stay
beside their scripts. Their assumptions and fit limits are in
[Calibration and validation](../../doc/CALIBRATION.md).

## Parameter-sweep studies

| Directory | What to run and inspect |
|---|---|
| [`collective_topology_dcqcn/`](collective_topology_dcqcn/README.md) | Collective/topology, NCCL-ring, DCQCN-parameter and lossy-RoCE sweeps; plotting scripts reproduce each comparison. |
| [`random_traffic_dcqcn/`](random_traffic_dcqcn/README.md) | Random-traffic injection rate and congestion-control throughput. |
| [`credit_bdp_sweep/`](credit_bdp_sweep/README.md) | Credit-window versus BDP, traffic patterns and completion latency. |
| [`datasize_injection_sweeps/`](datasize_injection_sweeps/README.md) | Data size, injection rate, message size, node count and chunk size on the matched scale-out fabric. |

The per-study README lists its run and plotting scripts. Some stored results
predate model changes; use their recorded parameters and the calibration guide
when interpreting them. The other directories under repository `examples/`
contain bundled ns-3 examples.

## Calibration scripts

Executable fitting scripts live in [`calibration/`](calibration/). Their
parameter JSON and sweep CSV data remain in
[`configs/calibration/`](../../configs/calibration/).

| Script | Action |
|---|---|
| [`fullmesh_ns3_sweep.py`](calibration/fullmesh_ns3_sweep.py) | Run the full-mesh sweep and fit schedule bandwidth. |
| [`tree_ns3_sweep.py`](calibration/tree_ns3_sweep.py) | Run the tree sweep and fit bandwidth/pipeline cost; `--refit` uses the cached CSV. |
| [`roce_p2p_calib.py`](calibration/roce_p2p_calib.py) | Fit scalar RoCE parameters to the embedded perftest reference. |

```bash
python examples/gpu-cluster/calibration/tree_ns3_sweep.py --refit
```

This refits and writes `configs/calibration/tree_calib.json`. See
[Calibration](../../doc/CALIBRATION.md) before treating a fitted parameter as a
hardware measurement.
