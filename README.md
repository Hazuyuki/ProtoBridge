# ProtoBridge

ProtoBridge simulates GPU collective, point-to-point and RDMA communication.
Use it to compare interconnect topologies, collective algorithms, flow control,
loss recovery and host/GPU data paths under the same workload.

Its **Operation-to-Packet (OTP)** layer describes transfers and dependencies;
its **Packet-Execution (PEX)** layer runs queues, flow control, congestion,
FEC/retry and switching on ns-3 events. A Python latency surrogate supports
parameter sweeps before packet simulation.

The repository bundles ns-3.47 and builds standalone. It models interconnect
communication; an upper-level simulator supplies compute and data-ready times.
No physical GPU, CUDA installation or RDMA hardware is required.

## Requirements

The native simulator and WebUI are tested in Linux CI. You need:

- A C++23 compiler: GCC 11 or later, or Clang 17 or later.
- CMake 3.20 or later and Ninja or Make.
- Python 3.10 or later, with `venv` and `pip` for the optional Python tools.
- Git to clone the repository.

On Ubuntu/Debian, install the build tools with:

```bash
sudo apt-get update
sudo apt-get install -y git g++ cmake ninja-build python3 python3-venv python3-pip
```

Other platform prerequisites are covered by the bundled
[ns-3 installation guide](doc/installation/source/index.rst).

## Quickstart

Clone the repository and create a Python environment:

```bash
git clone https://github.com/Hazuyuki/ProtoBridge.git
cd ProtoBridge
python3 -m venv .venv
source .venv/bin/activate
```

Build the ProtoBridge module and its ns-3 dependencies, then run the H200 ring
example. Run subsequent commands from the repository root:

```bash
./ns3 configure --enable-modules=gpu-cluster --enable-examples --enable-tests -d debug
./ns3 build gpu-cluster-sim protocol-profile-demo -j 2
./ns3 run "gpu-cluster-sim --protocolConfig=configs/protocol_configs/h200-ring-allreduce.cfg --numGpus=8 --dataSize=1048576" --no-build
```

The run prints a `RESULT_START` / `RESULT_END` block. Representative fields
from this example are shown below; the complete block contains more counters:

```text
RESULT_START
status=complete
simTimeNs=38308
simTimeUs=38.3
RESULT_END
```

`simTimeNs` and `simTimeUs` are simulated durations. `status=incomplete` means
the operation did not finish within the simulated workload limit; inspect the
resolved settings and counters before interpreting its duration.

For addressed RDMA Read with the error-free PCIe packet model:

```bash
./ns3 run "gpu-cluster-sim --protocolConfig=configs/protocol_configs/rdma-read.cfg --numNodes=2 --gpusPerNode=2 --dataSize=9000 --pcieModel=tlp --pcieTraceFile=/tmp/pcie.csv" --no-build
```

RDMA uses the RoCE packet path and defaults to the legacy startup model.
Detailed timing independently selects CPU/GPU initiation and
host/staged/GPUDirect memory paths. The IBGDA preset selects GPU initiation
with GPUDirect.

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

Use the [documentation index](doc/README.md) for configuration, architecture,
topology development, RDMA/IBGDA, PCIe, surrogate, and calibration guides.
The [topology development guide](doc/TOPOLOGY_GUIDE.md) walks through two runnable
C++ examples, including explicit links and forward/return routes.
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

## Validation and model scope

The [calibration guide](doc/CALIBRATION.md) records hardware references,
third-party measurements and fits to packet simulation separately. Check its
workload, topology, credit window and fit conditions when reusing a parameter.
Detailed RDMA/IBGDA and PCIe defaults are illustrative mechanism inputs.
Record the Git commit and resolved configuration with reported results.

GPU kernel execution, device firmware and application compute are represented
by configured costs or upper-simulator ready times. PCIe TLP execution covers
error-free Gen3–5 traffic on established links. The analytical surrogate has
its own scope; selecting detailed RDMA/PCIe does not add those state machines
to its predictions.

## Support and contributions

ProtoBridge is maintained by **NICS-EFC**. Use
[GitHub Issues](https://github.com/Hazuyuki/ProtoBridge/issues) for questions,
bug reports and feature requests. Include the commit, command, configuration
and relevant result/log output so others can reproduce a problem.

For contributions, see [CONTRIBUTING.md](CONTRIBUTING.md). Project and bundled
framework credits are recorded in [AUTHORS](AUTHORS).

## License

The primary license is GPL-2.0-only. See [LICENSE](LICENSE) and the component
license notices in [LICENSES/](LICENSES/).
