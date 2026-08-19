# ProtoBridge — An ns-3-Based LLM-to-Packet Simulator for Superpod Interconnect Exploration

Larger LLMs and longer contexts exceed the memory of a single GPU, so serving
systems partition weights, key-value cache, and activations across many
devices. Partitioning relieves memory pressure but introduces collective,
point-to-point, and remote-memory communication that bounds serving latency and
throughput — motivating *superpods* that connect tens to thousands of GPUs over
high-bandwidth, low-latency scale-up fabrics.

Superpod interconnect design has not converged: products and proposals differ in
link technology, topology, and scale-up protocol stack, so architects must
accurately compare a wide range of designs for target LLM workloads. But the two
usual tools both fall short:

- **Latency-bandwidth models** estimate each operation from message size and
  effective bandwidth with a fixed startup. They are fast but inaccurate (up to
  ~40% error) and cannot distinguish one scale-up protocol from another.
- **Packet-level simulators** such as ns-3 model routing, queueing, and packet
  timing, but each scale-up protocol stack requires a separate LLM-to-packet
  implementation — so protocols cannot be compared on a common substrate — and
  detailed simulation takes 2.38–75.70 s per operation, while one serving
  configuration invokes thousands: hours or days per design.

**ProtoBridge** closes this gap. It is an ns-3-based LLM-to-packet simulator
that makes protocol effects explicit through two abstraction layers separating
protocol-specific behavior from a shared ns-3 path:

1. **Operation-to-Packet (OTP)** — translates each communication step into
   packet transfers and defines step dependencies and completion rules,
   capturing the protocol-specific packet exchange (vendor wire formats:
   NVIDIA NCCL/NVLink LL·LL128·SIMPLE, Huawei Unified Bus, MetaXLink MCCCL).
2. **Packet-Execution (PEX)** — carries the shared packet-level mechanisms —
   credit-based flow control, FEC + go-back-N/SACK retry, per-link BER
   degradation, and an NVSwitch model (input VOQs, crossbar arbiter, SHARP
   in-network allreduce) — and drives the ns-3 event system.

Different protocol configurations are therefore compared on a common ns-3
simulation substrate by changing only the input configuration; the application
layer drives `NetDevice` `Send()` / `ReceiveCallback()` directly over a custom
bare-metal fabric header, with no TCP/IP stack. ProtoBridge matches measured
superpod communication latency with **17.8% MAPE on H200 NVLink4** across
1 KB–1 GB. The accompanying paper (in preparation) reports the full validation
against real NCCL measurements, the surrogate's accuracy relative to the
latency-bandwidth baseline, and the coarse-to-fine design workflow (with case
studies on scale-up memory pools and co-packaged optics).

For fast design exploration, ProtoBridge ships a **protocol-defined latency
surrogate** that combines OTP parameters (startup, effective bandwidth) with
analytical models of each PEX mechanism. It is grounded in the H200 NVLink4
reference and predicts per-operation latency in microseconds —
fast enough to sweep many designs where the packet simulator cannot. See
[doc/SURROGATE.md](doc/SURROGATE.md) for the derivation and
[doc/CALIBRATION.md](doc/CALIBRATION.md) for the in-repo H200 calibration data.

## What this is (and is not)

**Is:** an interconnect-only simulator + a surrogate model. Credit-based flow
control, packet spraying + reorder, NVSwitch VOQ/crossbar/SHARP, a four-tier
FEC/retry resilience model, link BER degradation, and a parametric topology
grammar spanning 12 families.

**Is not:** a full system simulator. There is no TCP/IP stack and no LLM serving
runtime (no model execution, KV-cache, memory-hierarchy, or PD-split model);
the latency surrogate here is interconnect-only, not the
end-to-end multi-model DSE used in the paper's case studies. The application
layer issues direct collectives (allreduce / allgather / alltoall / broadcast /
reduce / reduce-scatter) against the fabric.

## Architecture

```
            ┌─────────────────────────────────────────────────────┐
   App ───▶ │  OTP: operation → packet                             │
 (collective│   ProtocolModel (3 vendors) + ProtocolPayloadBuilder │
  injector) │   FabricHeader (type/seq/flow/VC)                    │
            └──────────────────────┬──────────────────────────────┘
                                   ▼
            ┌─────────────────────────────────────────────────────┐
            │  PEX: packet execution                                │
            │   FecModel  CreditManager  LlrManager  LinkDegradation│
            │   NVSwitch: VOQ + crossbar arbiter + SHARP allreduce │
            │   ContentionModel (WFQ collective vs P2P)             │
            └──────────────────────┬──────────────────────────────┘
                                   ▼
                          PointToPoint channel (BER + delay + bw)
```

- **OTP** turns a collective operation into a stream of fabric packets:
  `ProtocolModel` selects the vendor wire format / protocol (LL / LL128 /
  SIMPLE for NCCL; …), `ProtocolPayloadBuilder` packs the
  tensor chunks, and `FabricHeader` carries Packet Type (DATA/CREDIT),
  Sequence Number, Flow ID, and Virtual Channel ID.
- **PEX** executes those packets: `FecModel` (RS codes), `CreditManager`
  (per-VC credit gating), `LlrManager` (go-back-N / SACK retransmission),
  `LinkDegradationModel` (per-link BER sampling), and the NVSwitch model
  (input VOQs, a crossbar arbiter resolving output-port contention, and
  SHARP in-network allreduce with multicast return).

See [doc/ARCHITECTURE.md](doc/ARCHITECTURE.md) for the full design, the
FabricHeader layout, the four-tier resilience model, and the topology grammar.

## Quickstart

### Build

```bash
./ns3 configure --enable-examples --enable-tests -d debug
./ns3 build
```

> **Repo scope.** This repository bundles the full ns-3.47 framework, so it
> builds standalone — `./ns3` and `CMakeLists.txt` are the ns-3 build system.
> The ProtoBridge-specific code is `src/gpu-cluster/` (OTP + PEX + NVSwitch +
> injectors), `surrogate/` (latency model), `configs/`, and the two `scratch/`
> entry points; everything else is stock ns-3.

### Run a collective from a config (primary form)

A `.cfg` is the primary input: it names a calibrated builtin collective, the
fabric topology, and — via the `[stack]` profile — the PEX bundle and fabric
hardware, so a one-line run reproduces a measured H200 latency with no per-run
flags.

```bash
./ns3 run "gpu-cluster-sim --protocolConfig=\
configs/protocol_configs/h200-ring-allreduce.cfg --numGpus=8 --dataSize=1048576"
```

Output (8-GPU 1 MiB ring allreduce):

```
RESULT_START
simTimeUs=38.3
RESULT_END
```

This reproduces the H200 NVLink4 measurement (37.24 µs) to ~3% and is
**bit-identical** to the inline path run with the profile's values as flags —
`test/parity/test_config_builtin_parity.py` asserts exact `simTimeNs` equality
across `{ring, tree, nvls}` × sizes × GPU counts. Three builtins ship:
`h200-ring-allreduce.cfg` (`builtin=ring`), `h200-tree-allreduce.cfg`
(`builtin=tree`), `h200-nvls-allgather.cfg` (`builtin=nvls`). `numGpus` and
`dataSize` stay on the CLI. See [doc/CONFIG_GUIDE.md](doc/CONFIG_GUIDE.md).

### Equivalent inline flags

The config form sources the profile into the same local variables the inline
path consumes, so the two are interchangeable. The ring `.cfg` is bit-identical
to:

```bash
./ns3 run "gpu-cluster-sim --topology=ring --collective=allreduce --algorithm=ring \
  --numGpus=8 --dataSize=1048576 \
  --bandwidth=170 --latency=400 --numLanes=18 --linksPerGpu=1 \
  --startupLL=15000 --startupLL128=25000 --startupSIMPLE=46000 \
  --forceProtocol=0 --fecN=544 --fecK=514 --fecT=15 ..."   # full set in test/parity/
```

Key flags: `--topology` (ring/fullmesh/switched/fattree/leafspine/nvl72/
hypercube/torus/mesh/…), `--algorithm` (ring/tree/sharp/nvls/fullmesh/auto),
`--numGpus`, `--dataSize`, `--ber` + `--fecN/--fecK/--fecT` (resilience),
`--llrEnabled --llrMode=gobackn|sack`, `--protocolModel`,
`--arbiter` (crossbar arbitration strategy, default `roundrobin`).

> **Extending the datapath.** Three polymorphic seams let you plug an
> alternative flow-control policy (`FabricEndpoint::FlowControlGate` is
> `virtual`), arbitration algorithm (subclass `Arbiter`; select via
> `NvSwitch::SetArbiter` / `--arbiter`), or switch architecture (subclass
> `FabricSwitch`; select via `NvSwitchHelper::SetSwitchType`). Each default
> reproduces the calibrated behavior. See
> [doc/ARCHITECTURE.md](doc/ARCHITECTURE.md#extension-seams).

### Load a hardware profile

```bash
./ns3 run "protocol-profile-demo --profile=configs/protocol_profiles/h200-ll128.profile"
```

Prints the resolved bundle: vendor, FEC (544/514/15), 4 VCs × 64 credits,
credit flow control, LLR off.

### Define a custom protocol stencil in config

A `.cfg` whose `[op]` lists transfers (no `builtin=`) compiles to a transaction
graph run by the generic runner — the "define a protocol in tens of lines of
config" seam; the validated PEX topology/BER/FEC wiring is reused unchanged.
`h200-request-response.cfg` (a two-leg ping-pong) ships as the stencil example;
the ring/tree/nvls `.cfg`s above use `builtin=` to delegate to the calibrated
injectors. See [doc/ARCHITECTURE.md](doc/ARCHITECTURE.md) for the `.cfg` schema
and the OTP/PEX transaction-graph seam.

## Surrogate model

A latency surrogate lives under `surrogate/`, grounded in H200 NVLink4
(18 NVLinks/GPU, 900 GB/s peak aggregate per direction, 400 ns link latency):

| Model | Location | What it is | Accuracy |
|-------|----------|------------|----------|
| Theory-derived | `surrogate/theory/whitebox_surrogate_v2.py` | First-principles physical bound (startup + serialization + credit round-trip + FEC/retry terms) | lower bound, ~0.6–0.75× ns-3 |

```bash
# Predict ring allreduce latency at 4 GPU / 1 MiB (theory model, lower bound).
PYTHONPATH=surrogate/theory python3 -c 'import whitebox_surrogate_v2 as wb; print(wb.make_h200_ring_theory().predict(1<<20,4,"ring",credits=32,ber=0))'

# Custom hardware: override any H200 parameter (see doc/SURROGATE.md).
PYTHONPATH=surrogate/theory python3 -c 'import whitebox_surrogate_v2 as wb; print(wb.make_h200_ring_theory(num_lanes=8, startup_us=7).predict(1<<20,4,"ring",credits=32,ber=0))'
```

See [doc/SURROGATE.md](doc/SURROGATE.md) for the derivation and
[doc/CALIBRATION.md](doc/CALIBRATION.md) for the H200 calibration data.

## Tests

```bash
# C++ unit + integration suites
./ns3 run "test-runner --suite=gpu-cluster"
./ns3 run "test-runner --suite=gpu-cluster-integration"

# Python surrogate tests (needs pytest — see surrogate/requirements.txt)
pip3 install -r surrogate/requirements.txt
python3 -m pytest surrogate/test/ -v
```

## Calibration data

All calibration is H200 NVLink4 (no A800). Under `surrogate/calibration/`:

- `h200_ring4_ar_baseline.json` — 27 raw ns-3 ring-allreduce points (4 GPU,
  64 KiB–1 GiB, 3 seeds each).

## Architecture constraints

- **No TCP/IP stack.** `InternetStackHelper` is never installed on a node;
  there is no IPv4/IPv6, no TCP/UDP.
- **Bare-metal communication.** The application layer calls `NetDevice::Send()`
  and registers a `ReceiveCallback` directly.
- **Discrete-event only.** No `while(true)` polling or 1-cycle timers; every
  state machine advances on ns-3 events and callbacks.

## Repository layout

```
src/gpu-cluster/        OTP + PEX + NVSwitch + collective injectors + tests
scratch/                gpu-cluster-sim.cc (simulator entry), protocol-profile-demo.cc
configs/                protocol_profiles/ (h200-ll128.profile), protocol_configs/ (.cfg), dse/topo_specs.csv
surrogate/              theory/  calibration/  test/
test/parity/            test_config_builtin_parity.py (config-vs-inline bit-identity gate)
doc/                    ARCHITECTURE / CALIBRATION / CONFIG_GUIDE / SURROGATE (+ ns-3 manual)
```

## License

GPL-2.0 — see [LICENSE](LICENSE). The simulator is built on the ns-3
framework; see [CONTRIBUTING.md](CONTRIBUTING.md) and the ns-3
[doc/](doc/) manual for the underlying framework.
