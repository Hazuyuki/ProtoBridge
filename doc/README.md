# ProtoBridge documentation

Start with the repository [quickstart](../README.md), then choose a guide:

| Guide | Contents |
|---|---|
| [Configuration](CONFIG_GUIDE.md) | `.cfg` and `.profile` inputs, precedence, topology and protocol parameters, time bounds. |
| [Architecture](ARCHITECTURE.md) | OTP/PEX, packet execution, resilience, topology and extension interfaces. |
| [RDMA and IBGDA](RDMA.md) | CPU/GPU submission, host/staged/GPUDirect memory paths, operations, completion and C++ integration. |
| [PCIe](PCIE.md) | Bandwidth and TLP models, shared paths, queues, credits, tags and traces. |
| [Surrogate](SURROGATE.md) | Python API, topology and hierarchical predictions, mechanism equations and limits. |
| [Calibration and validation](CALIBRATION.md) | Measurement provenance, fitted parameters, independent comparisons and reproduction commands. |

Runnable configurations, C++ examples and experiment scripts are listed in
the [example catalog](../examples/gpu-cluster/README.md). WebUI setup, SSH/Docker
access and controls are covered in its [English](../webui/README.md) and
[Chinese](../webui/README.zh-CN.md) guides.

For development, use [CONTRIBUTING.md](../CONTRIBUTING.md) and the project
[change summary](../CHANGES.md). Detailed development history is available in
Git.

The other subdirectories here contain the bundled ns-3 framework's
[manual](manual/source/index.rst), [tutorial](tutorial/source/index.rst),
[installation](installation/source/index.rst),
[model documentation](models/source/index.rst) and
[contribution guide](contributing/source/index.rst).
