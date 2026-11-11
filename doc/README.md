# ProtoBridge documentation

Choose a reading path for the task:

| Task | Reading path |
|---|---|
| Run an experiment | Repository [quickstart](../README.md) → [Configuration](CONFIG_GUIDE.md) → [example catalog](../examples/gpu-cluster/README.md). |
| Understand or extend simulation | [Architecture](ARCHITECTURE.md) → [RDMA/IBGDA](RDMA.md) and [PCIe](PCIE.md) for the relevant device path. |
| Implement a topology | [Topology development](TOPOLOGY_GUIDE.md) → the runnable C++ network and PCIe examples. |
| Sweep designs analytically | [Surrogate](SURROGATE.md) → [Calibration](CALIBRATION.md) for reference conditions and validation. |
| Use the browser interface | WebUI [English](../webui/README.md) or [Chinese](../webui/README.zh-CN.md) guide. |

## Guides

| Guide | Contents |
|---|---|
| [Configuration](CONFIG_GUIDE.md) | Input syntax, precedence, units/defaults, authoring and the simulation time horizon. |
| [Architecture](ARCHITECTURE.md) | OTP/PEX, execution paths, packet metadata, network mechanisms and extension interfaces. |
| [Topology development](TOPOLOGY_GUIDE.md) | Build network and PCIe graphs in C++, configure routes, run traffic and add a simulator option. |
| [RDMA and IBGDA](RDMA.md) | Submission/memory events, operation and completion semantics, parameters/results and C++ integration. |
| [PCIe](PCIE.md) | Local TLP topology, execution, credits/tags/queues, API, traces and scope. |
| [Surrogate](SURROGATE.md) | Python API, analytical composition and equations. |
| [Calibration and validation](CALIBRATION.md) | Dataset provenance, fitted values, recorded metrics, limits and reproduction commands. |

The [example catalog](../examples/gpu-cluster/README.md) lists configurations
and scripts. The root [README](../README.md) covers installation and the first
run; [CONTRIBUTING.md](../CONTRIBUTING.md) covers development and test requirements. [CHANGES.md](../CHANGES.md) summarizes user-visible changes;
Git retains detailed development history.

## Bundled ns-3 documentation

The other subdirectories contain the framework's
[manual](manual/source/index.rst), [tutorial](tutorial/source/index.rst),
[installation](installation/source/index.rst),
[model documentation](models/source/index.rst) and
[contribution guide](contributing/source/index.rst).
