# Contributing to ProtoBridge

ProtoBridge bundles ns-3.47. Project models and helpers live in
`src/gpu-cluster/`; the simulator entry point is
`scratch/gpu-cluster-sim.cc`. The Python surrogate, configuration inputs,
experiment scripts and WebUI live in their named directories. Start with
[the documentation index](doc/README.md) and
[example catalog](examples/gpu-cluster/README.md).

## Build and test

```bash
./ns3 configure --enable-examples --enable-tests -d debug
./ns3 build
for suite in gpu-cluster gpu-cluster-integration gpu-cluster-ibgda \
  gpu-cluster-rdma-modes gpu-cluster-rdma-operations gpu-cluster-pcie \
  gpu-cluster-time-conversions gpu-cluster-time-safety; do
  ./ns3 run "test-runner --test-name=$suite --fullness=EXTENSIVE" --no-build
done
python -m pip install -r surrogate/requirements.txt -r webui/requirements-dev.txt
python -m pytest surrogate/test/ test/parity/ webui/tests/ -q
```

Run the checks relevant to a change. Changes to shared core, network or
point-to-point primitives also need their affected ns-3 suites. CI uses a
portable release build; its full list of checks is in
[ci.yml](.github/workflows/ci.yml).

## Simulation contracts

- Applications use `NetDevice::Send` and receive callbacks directly. Keep the
  bare-metal fabric independent of a TCP/IP stack.
- Keep configuration and equivalent inline runs bit-identical unless a
  behavior change is intentional. The config runner delegates calibrated
  collective algorithms to the same injectors as the inline path;
  `test/parity/test_config_vs_inline_parity.py` checks this contract.
- Use the extension interfaces for flow control, arbitration and switching
  described in [ARCHITECTURE.md](doc/ARCHITECTURE.md#extension-seams-flow-control-arbitration-switching).
- Separate hardware measurements, third-party references and fits to this
  simulator. Record the workload, topology, credit window, units and source
  with calibration results; see [CALIBRATION.md](doc/CALIBRATION.md).
- Explain changed completion semantics, default values and timing behavior in
  the PR and update the relevant topic guide.

## Documentation and examples

Maintain one guide per topic and link to it instead of copying parameter
lists. Add C++ API examples under `src/gpu-cluster/examples/` and register them
in its `CMakeLists.txt`. Put experiment and calibration scripts under
`examples/gpu-cluster/`; keep configuration inputs and fitted data under
`configs/`. Update the example catalog when adding or moving an entry.

Generated figures and reference results must identify their producing script
and workload. Local run logs, review notes and development checkpoints belong
outside the repository. Git retains removed or superseded documentation.

## Framework contributions

The bundled framework's [contribution guide](doc/contributing/source/index.rst)
covers ns-3 development and C++ style. ProtoBridge changes to shared primitives
must preserve ordinary ns-3 behavior and include focused regression coverage.
Keep the upstream manuals and release history separate from project guides.
