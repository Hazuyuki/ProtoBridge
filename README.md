# ProtoBridge

ProtoBridge simulates GPU collective, point-to-point and RDMA communication.
Its **Operation-to-Packet (OTP)** layer describes transfers and dependencies;
its **Packet-Execution (PEX)** layer runs queues, flow control, congestion,
FEC/retry and switching on ns-3 events. A Python latency surrogate supports
parameter sweeps before packet simulation.

The repository bundles ns-3.47 and builds standalone. It models interconnect
communication; an upper-level simulator supplies compute and data-ready times.
No physical GPU, CUDA installation or RDMA hardware is required.

## Build and run

Run commands from the repository root:

```bash
./ns3 configure --enable-examples --enable-tests -d debug
./ns3 build gpu-cluster-sim protocol-profile-demo
./ns3 run "gpu-cluster-sim --protocolConfig=configs/protocol_configs/h200-ring-allreduce.cfg --numGpus=8 --dataSize=1048576" --no-build
```

Results appear between `RESULT_START` and `RESULT_END`, including `simTimeNs`
and `simTimeUs`. The example uses the H200 ring profile. Measurement provenance,
fit conditions and accuracy limits are in [Calibration](doc/CALIBRATION.md).

For addressed RDMA Read with the error-free PCIe packet model:

```bash
./ns3 run "gpu-cluster-sim --protocolConfig=configs/protocol_configs/rdma-read.cfg --numNodes=2 --gpusPerNode=2 --dataSize=9000 --pcieModel=tlp --pcieTraceFile=/tmp/pcie.csv" --no-build
```

RDMA defaults to the legacy startup model. Detailed timing independently
selects CPU/GPU initiation and host/staged/GPUDirect memory paths. The IBGDA
preset selects GPU initiation with GPUDirect. Detailed RDMA/PCIe timing inputs
are configurable and have not been fitted to a particular GPU/NIC.

## WebUI

```bash
python -m pip install -r webui/requirements.txt
python -m webui --host 127.0.0.1 --port 8080
```

Open <http://localhost:8080>. The interface defaults to English and supports
Chinese. See the [English](webui/README.md) or
[Chinese](webui/README.zh-CN.md) guide for SSH/VS Code forwarding, Docker,
configuration controls and result downloads.

## Documentation and examples

Use the [documentation index](doc/README.md) for the six topic guides:
configuration, architecture, RDMA/IBGDA, PCIe, surrogate, and calibration.
The [example catalog](examples/gpu-cluster/README.md) lists every project C++
example, runnable configuration, sweep and plotting script.

The surrogate's Python API and mechanism equations are in
[the surrogate guide](doc/SURROGATE.md). It predicts interconnect latency;
stateful RDMA/PCIe submission remains part of packet simulation.

## Repository layout

| Path | Purpose |
|---|---|
| `src/gpu-cluster/` | ProtoBridge models, helpers and C++ tests. |
| `src/gpu-cluster/examples/` | Small C++ API and topology examples. |
| `scratch/gpu-cluster-sim.cc` | Main simulator command-line application. |
| `configs/protocol_configs/` | Runnable operation and collective `.cfg` files. |
| `configs/protocol_profiles/` | Shared hardware/protocol `.profile` values. |
| `configs/calibration/` | Fitted parameter JSON and recorded sweep CSV data. |
| `configs/dse/` | Topology specifications for design sweeps. |
| `examples/gpu-cluster/` | Reference comparisons, sweep/fit scripts and figures. |
| `surrogate/` | Analytical model, baseline data and Python tests. |
| `test/parity/` | Native simulation regression and parity tests. |
| `webui/` | Local web service, browser interface and tests. |
| `doc/` | Project guides and bundled ns-3 documentation. |

The bundled ns-3 modules, examples, release notes and manuals retain their
upstream organization. ProtoBridge also modifies selected core/network
primitives used by its packet path; see [CHANGES.md](CHANGES.md).

## Development

[CONTRIBUTING.md](CONTRIBUTING.md) lists build/test commands and the
configuration-parity requirements. The [CI workflow](.github/workflows/ci.yml)
runs the full build, C++ suites, surrogate/parity checks and WebUI tests.

## License

GPL-2.0; see [LICENSE](LICENSE) and [AUTHORS](AUTHORS).
