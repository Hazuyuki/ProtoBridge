# ProtoBridge — Bare-Metal GPU-Fabric Interconnect Simulator

ProtoBridge is a discrete-event simulator for the L2/MAC fabric communication of
large-scale GPU clusters (100-card-class super-nodes built on NVSwitch /
NVLink). It strips the TCP/IP stack entirely: applications drive `NetDevice`
`Send()` / `ReceiveCallback()` directly over a custom bare-metal fabric header,
modeling the wire behavior of vendor collective protocols (NVIDIA NCCL, Huawei
UB, Intel oneAPI/CXL, AMD Infinity Fabric, MetaXLink MCCCL, RoCE, ICI).

It ships three latency surrogates calibrated against an H200 NVLink4 reference,
so you can predict collective latency without running the full ns-3 sweep.

## What this is (and is not)

**Is:** an interconnect-only simulator + surrogate models. Credit-based flow
control, packet spraying + reorder, NVSwitch VOQ/crossbar/SHARP, a four-tier
FEC/retry resilience model, link BER degradation, and a parametric topology
grammar spanning 12 families.

**Is not:** a full system simulator. There is no TCP/IP stack, no LLM serving /
KV-cache / memory-hierarchy / PD-split model, no DSE harness. The application
layer issues direct collectives (allreduce / allgather / alltoall / broadcast /
reduce / reduce-scatter) against the fabric.

## Architecture

```
            ┌─────────────────────────────────────────────────────┐
   App ───▶ │  C1: operation → packet                              │
 (collective│   ProtocolModel (7 vendors) + ProtocolPayloadBuilder │
  injector) │   FabricHeader (type/seq/flow/VC)                    │
            └──────────────────────┬──────────────────────────────┘
                                   ▼
            ┌─────────────────────────────────────────────────────┐
            │  C2: packet execution                                 │
            │   FecModel  CreditManager  LlrManager  LinkDegradation│
            │   NVSwitch: VOQ + crossbar arbiter + SHARP allreduce │
            │   ContentionModel (WFQ collective vs P2P)             │
            └──────────────────────┬──────────────────────────────┘
                                   ▼
                          PointToPoint channel (BER + delay + bw)
```

- **C1** turns a collective operation into a stream of fabric packets:
  `ProtocolModel` selects the vendor wire format / protocol (LL / LL128 /
  SIMPLE for NCCL; HCCS for Huawei; …), `ProtocolPayloadBuilder` packs the
  tensor chunks, and `FabricHeader` carries Packet Type (DATA/CREDIT),
  Sequence Number, Flow ID, and Virtual Channel ID.
- **C2** executes those packets: `FecModel` (RS codes), `CreditManager`
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

### Run one collective

```bash
./ns3 run "gpu-cluster-sim --numGpus=8 --topology=switched --algorithm=auto --dataSize=1048576"
```

Output (interconnect-only direct-collective mode):

```
RESULT_START
simTimeUs=88.2
RESULT_END
```

Key flags: `--topology` (ring/fullmesh/switched/fattree/leafspine/nvl72/
hypercube/torus/mesh/…), `--algorithm` (ring/tree/sharp/nvls/fullmesh/auto),
`--numGpus`, `--dataSize`, `--ber` + `--fecN/--fecK/--fecT` (resilience),
`--llrEnabled --llrMode=gobackn|sack`, `--protocolModel`.

### Load a hardware profile

```bash
./ns3 run "protocol-profile-demo --profile=configs/protocol_profiles/h200-ll128.profile"
```

Prints the resolved bundle: vendor, FEC (544/514/15), 4 VCs × 64 credits,
credit flow control, LLR off.

## Surrogate models

Three latency surrogates live under `surrogate/`, calibrated on H200 NVLink4
(18 NVLinks/GPU, 900 GB/s peak aggregate per direction, 400 ns link latency):

| Model | Location | What it is | Accuracy |
|-------|----------|------------|----------|
| Theory-derived | `surrogate/theory/whitebox_surrogate_v2.py` | First-principles physical bound (startup + serialization + credit round-trip + FEC/retry terms) | lower bound, ~0.6–0.75× ns-3 |
| Analytical | `surrogate/analytical/analytical_surrogate.py` | Piecewise small/large-size regression + FEC/retry amplification overlay | **0.9% mean / 2.7% max APE** (27 pts, 64 KiB–1 GiB) |
| Topology-aware | `surrogate/topo/dse_topo_surrogate.py` | Decomposes latency into hop × bandwidth × step-count physical terms + a small per-group residual | 0.47% MAPE (H200 128-GPU ring sweep) |

```bash
# Run the analytical surrogate: calibrate + validate against the H200 baseline.
python3 surrogate/analytical/analytical_surrogate.py

# Predict latency across message sizes for the first feasible topology.
python3 surrogate/topo/dse_topo_surrogate.py
python3 surrogate/topo/dse_topo_surrogate.py --all            # one size, all feasible specs
```

See [doc/SURROGATE.md](doc/SURROGATE.md) for the derivations and
[doc/CALIBRATION.md](doc/CALIBRATION.md) for the H200 calibration data.

## Tests

```bash
# C++ unit + integration suites
./ns3 run "test-runner --suite=gpu-cluster"
./ns3 run "test-runner --suite=gpu-cluster-integration"

# Python surrogate tests
python3 -m pytest surrogate/test/ -v
```

## Calibration data

All calibration is H200 NVLink4 (no A800). Under `surrogate/calibration/`:

- `h200_ring4_ar_baseline.json` — 27 raw ns-3 ring-allreduce points (4 GPU,
  64 KiB–1 GiB, 3 seeds each).
- `h200_ring4_surrogate_calibration.json` — pre-fit analytical params
  (avg 0.92% / max 2.68% APE).
- `h200_nvls_ar_baseline.json`, `h200_surrogate_calibration.json` — NVLS /
  multi-collective baselines.
- `reference/` — 34 real NCCL measurement files on H200 8-GPU
  (ring/tree/nvls × allreduce/allgather/alltoall/broadcast × LL/LL128/SIMPLE).

## Architecture constraints

- **No TCP/IP stack.** `InternetStackHelper` is never installed on a node;
  there is no IPv4/IPv6, no TCP/UDP.
- **Bare-metal communication.** The application layer calls `NetDevice::Send()`
  and registers a `ReceiveCallback` directly.
- **Discrete-event only.** No `while(true)` polling or 1-cycle timers; every
  state machine advances on ns-3 events and callbacks.

## Repository layout

```
src/gpu-cluster/        C1 + C2 + NVSwitch + collective injectors + tests
scratch/                gpu-cluster-sim.cc (simulator entry), protocol-profile-demo.cc
configs/                protocol_profiles/ (h200-ll128.profile), dse/topo_specs.csv
surrogate/              theory/  analytical/  topo/  calibration/  test/
doc/                    ARCHITECTURE / CALIBRATION / SURROGATE (+ ns-3 manual)
```

## License

GPL-2.0 — see [LICENSE](LICENSE). The simulator is built on the ns-3
framework; see [CONTRIBUTING.md](CONTRIBUTING.md) and the ns-3
[doc/](doc/) manual for the underlying framework.
