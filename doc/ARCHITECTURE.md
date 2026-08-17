# Architecture

ProtoBridge models GPU-fabric communication at L2/MAC, below any IP stack.
This document describes the layering, the fabric header, the flow-control /
spraying / reorder model, the NVSwitch datapath, the resilience tiers, and the
topology grammar.

## Layering: OTP (operation → packet) and PEX (packet execution)

The simulator is split into two abstraction layers, mirroring the paper's
OTP/PEX model. **OTP — operation to packet** (a.k.a. C1) turns a tensor
operation into a stream of fabric packets. **PEX — packet execution**
(a.k.a. C2) drives those packets across the wire. The two layers are
decoupled by a small transaction-graph seam (see below): OTP emits a graph of
ACTION/delivery-WAIT nodes; PEX fires each ACTION as a wire send and feeds
delivery events back. A collective can be expressed either by a hand-written
injector (the validated path) or by a few lines of config compiled through
that seam (the `--protocolConfig` path).

**OTP — operation to packet.** A collective injector (ring-allreduce,
tree-allreduce, nvls-allgather, sharp-allreduce, fullmesh, alltoall, …)
decomposes a tensor operation into a stream of fabric packets. For each packet
it asks:

- `ProtocolModel` (one of 3 vendor models validated in the paper:
  `NcclProtocolModel`, `UbProtocolModel`, `McclProtocolModel`) to select the
  wire protocol (e.g. NCCL LL / LL128 / SIMPLE) and wire efficiency.
- `ProtocolPayloadBuilder` to pack the tensor chunk into the payload.
- `FabricHeader` to stamp packet type, sequence number, flow ID, virtual
  channel, source/dest rank.

**PEX — packet execution.** Each packet traverses:

- `CreditManager` — per-VC credit gating; a DATA packet consumes one credit on
  the matching VC and the receiver returns a CREDIT packet when the buffer
  drains.
- `FecModel` — RS(N,K,T) encoding/decoding; an uncorrectable codeword raises
  an LLR retry, a correctable one is fixed with a decode-latency cost.
- `LlrManager` — go-back-N or SACK retransmission of lost/unacknowledged
  packets.
- `LinkDegradationModel` — per-link BER sampling (separate rates for
  intra-node electrical, intra-rack electrical, inter-rack electrical, and
  inter-rack optical); sampled drops trigger the FEC/LLR path.
- `ContentionModel` — WFQ arbitration between collective, P2P, and (legacy)
  memory traffic classes at a shared egress.
- **NVSwitch** — input VOQs per output port, a crossbar arbiter that resolves
  Head-of-Line contention on a per-cycle round-robin schedule, and SHARP
  in-network allreduce (buffers aligned packets, models a compute delay, then
  multicasts the result to all endpoints).

## OTP/PEX boundary: the transaction-graph seam

OTP and PEX never call each other directly. They meet at a small data
structure, the `ProtocolTransactionGraph`, and a runner,
`ProtocolTransactionExecutor`:

```
  OTP side                                  PEX side
  ┌──────────────────────┐                  ┌──────────────────────┐
  │ ProtocolTransaction- │  ACTION node     │  action callback     │
  │ Graph                │ ───────────────▶ │  → SendBulkWire-     │
  │  (ACTION / WAIT /   │                  │     TransferSize()   │
  │   DELAY / COMPLETE)  │  PACKET_DELIVERED │                      │
  │                      │ ◀─────────────── │  receive callback    │
  │ Executor::NotifyEvent│   (event)        │  → NotifyEvent()     │
  └──────────────────────┘                  └──────────────────────┘
```

- **OTP** builds the graph: `ProtocolModel::AddTransaction` expands one
  logical transfer into an `ACTION` (the wire send) plus a delivery `WAIT`
  (matched on packet type, source/dest rank, flow id, stage id, byte count).
  `DELAY` nodes insert startup / inter-step software overhead;
  `COMPLETE` is the terminal fan-in/fan-out node.
- **The seam** is two callbacks: the executor's `ActionCallback`
  (OTP → PEX: "fire this wire send") and `NotifyEvent` (PEX → OTP: "a
  packet matching this WAIT arrived"). The runner derives `stageId =
  flowId − baseFlowId` on the receive side so the delivery event routes to
  the right WAIT — this is what lets a multi-step ring pipeline advance.
- **PEX** owns the `FabricEndpoint` + `FecModel` + `CreditManager` +
  `LlrManager` + `LinkDegradationModel` + `NVSwitch` models below.

The hand-written injectors (e.g. `RingAllReduce`) build this graph in C++.
The config path (next section) builds the same kind of graph from a `.cfg`,
so a protocol is "defined in tens of lines of config" — the paper's OTP
stencil — without touching the PEX models.

## Config-driven protocol stack (`.cfg`)

A `.cfg` declares the two layers in one file: a `[stack]` PEX bundle (by
reference to a `ProtocolProfile`) and an `[op]` OTP stencil (a replicated
set of logical transfers). A compiler (`ProtocolConfig::Compile`) expands
the stencil through the vendor `ProtocolModel` — which emits the ACTION +
delivery WAIT nodes — so the config declares *transfers*, never raw graph
nodes.

```
[stack]
# PEX bundle by reference: protocol/FEC/credits/BER/flow-control/LLR.
profile = configs/protocol_profiles/h200-ll128.profile

[op]
# Symbolic params, evaluated in order; may reference numGpus, dataSize,
# and earlier params. Integer arithmetic only (+ - * / mod, parentheses).
param.N        = numGpus
param.segment  = dataSize / N
param.steps    = 2 * (N - 1)

# Replication domain: one transfer instance per (gpu, step). The first
# replicate var is the chain axis (each gpu's steps are chained in order).
replicate.gpu  = 0 .. N-1
replicate.step = 0 .. steps-1

# Per-chain startup. "auto" = the protocol model's startup delay for the
# total data size at N GPUs (matches the hand-written RingAllReduce).
startup        = auto
# Inter-step software overhead (ns) inserted between a gpu's steps.
per_step_delay = 0

# The transfer stencil, evaluated per instance with gpu/step/N/segment in
# scope. flowId and stageId are assigned by the compiler (baseFlowId +
# stage counter). kind ∈ DATA | P2P | MEMORY_READ | MEMORY_WRITE.
transfer.0.kind  = DATA
transfer.0.src   = gpu
transfer.0.dst   = (gpu + 1) mod N
transfer.0.bytes = segment
transfer.0.vc    = 0

# Completion: every gpu's final step must deliver (all | any).
complete = all
```

Run it with the validated topology / BER / FEC wiring kept on the PEX side:

```
./ns3 run "gpu-cluster-sim --protocolConfig=\
configs/protocol_configs/h200-ring-allreduce.cfg --numGpus=8 --dataSize=1048576"
```

Two example configs ship: `h200-ring-allreduce.cfg` (reproduces the
hand-written ring) and `h200-request-response.cfg` (a two-leg ping-pong
showing the stencil is not ring-specific). The config path uses the
profile's PEX parameters (4 VCs × 64 credits, RS(544,514,15) FEC, 15 µs
startup), so its latency is profile-canonical rather than bit-identical to
the default-path injector (which uses the protocol model's 65 µs startup
default). See [configs/protocol_configs/](../configs/protocol_configs) and
[configs/protocol_profiles/](../configs/protocol_profiles).

## FabricHeader layout

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
| PacketType(8) | FabricType(8) | VirtualChan(8)| VirtualLane(8)|
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
| MemoryAccess(8)|      FlowId(16)              |  SourceRank(16)|
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                       SequenceNumber(32)                       |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|  DestRank(16)  | PayloadSize(16) |  CreditCount(16)            |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
| Protocol(8) |  TTL(8)  |        EffectiveDataSize(32)          |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

`FabricPacketType` (the first byte): `DATA`, `CREDIT`, `ACK`, `NACK`,
`ALLREDUCE`, `ALLGATHER`, `ALLTOALL`, `REDUCESCATTER`, `BROADCAST`, `P2P`,
`MEMORY_READ`, `MEMORY_WRITE`, `MEMORY_RESP`, `RETRY_REQUEST`, `RETRY_ACK`,
`PERMANENT_LOSS`.

### Why `FabricHeader` is necessary: the unified-abstraction contract

`FabricHeader` is **not** an attempt to model any one hardware wire format
(real NVLink flits are 1280-bit units with a vendor routing overlay; that
layout is not publicly fixed and varies by vendor). Its role is different and
load-bearing: it is the **vendor-agnostic control-plane contract** that lets
one datapath serve all vendors, topologies, and protocols at once.

The target of this simulator is a *single* datapath spanning 7 vendor
protocols × ring/tree/SHARP/NVLS/full-mesh/hierarchical topologies ×
credit/window/rate flow control × the 4-tier resilience model. If every
vendor/topology pair defined its own header fields, the datapath would explode
into N×M specialized switch/reorder/credit/FEC/LLR paths. The only way to
collapse that combinatorial space is to lift the *semantic* fields the control
plane needs — packet type, end-to-end GPU address, sequence/flow/VC key, TTL,
payload vs effective size — into one common schema, independent of any
vendor's on-wire bit layout. `FabricHeader` is that schema.

Two facts make the spine/plug relationship concrete:

- Of `FabricHeader`'s 16 fields, **only `Protocol` (1 byte) carries vendor
  identity.** The other 15 are vendor-agnostic control semantics. Vendor
  encoding is delegated to the `ProtocolPayloadBuilder`; the heavy datapath
  logic (NVSwitch routing/VOQ/arbiter, reorder buffer, credit manager, FEC
  model, LLR, collective dispatch) runs entirely on the common schema. This is
  why plugging in a vendor protocol is a *local* change (the `Protocol` byte
  + a payload builder), not a rewrite of the spine.
- `effectiveDataSize` is kept separate from `payloadSize` so that
  overhead protocols (LL/LL128) can vary their wire size without disturbing
  collective-progress accounting, which must stay consistent across vendors.

### Contract vs. encoding (swappable wire bytes)

`FabricHeader`-as-contract is necessary and not optional — the unified model
cannot exist without it. `FabricHeader`-as-39-byte-wire-encoding is the
*default encoding* of that contract and is separable: if a real flit layout
becomes available, the on-wire bytes (and the serialization overhead they
model) can be swapped without touching the control semantics, which would then
flow through a side channel (a `PacketTag`) rather than the wire header. The
default 39-byte encoding is what the calibration invariants (88.2 µs / 44.6 µs)
are pinned to; swapping the wire encoding requires re-calibration. **Contract
≠ encoding.**

## Credit-based flow control + packet spraying + reorder

- **Credit gating.** Each VC maintains a credit counter initialized to the
  profile credit count (e.g. 4 VCs × 64 credits on H200). `ConsumeCredit(vc,
  seq)` decrements by one; when a VC is empty the DATA packet is held in the
  local send queue until a CREDIT packet returns. `creditBypass` mode lets
  control traffic (CREDIT/ACK/NACK) skip the gate, preventing flow-control
  deadlock under BER.
- **Packet spraying.** A sender with multiple physical links (NVLink lanes)
  sprays tensor chunks round-robin across them, so a single flow uses the
  full bisection bandwidth rather than one link's.
- **Reorder buffer.** The receiver maintains a per-flow reorder buffer keyed
  on sequence number; out-of-order arrivals (multi-path spraying) are held
  until the gap fills, then delivered in-order to the collective injector.

## Four-tier resilience model

A link's effective reliability is the composition of FEC + link-level retry
(LLR), selected per link via the protocol profile:

| Tier | FEC | LLR | Behavior under BER |
|------|-----|-----|---------------------|
| no-FEC | off | off | errors are permanent loss (baseline only; BER=0) |
| FEC-only | on | off | correctable errors fixed at decode cost; uncorrectable → permanent loss |
| FEC+retry | on | on | correctable fixed; uncorrectable → go-back-N/SACK retransmit |
| retry-only | off | on | every errored packet retransmits (high overhead, no FEC cushion) |

Control packets (CREDIT/ACK/NACK/RETRY_*) always bypass BER sampling, so
flow-control credit never deadlocks under a degrading link.

## Extension seams: flow control, arbitration, switching

The PEX datapath exposes three polymorphic hooks so an alternative flow-control
policy, arbitration algorithm, or switch architecture can be plugged in
without touching the validated datapath or its call sites. Each has a default
that reproduces the simulator's historical (calibration-identical) behavior.

- **Arbitration — `Arbiter` strategy.** `Arbiter` (abstract, `arbiter.h`) decides
  which output ports a switch drains each wake-up:
  `SelectGrants(voqs, outputBusyUntil, now)`. `NvSwitch::Arbitrate()` delegates
  grant-selection to a `Ptr<Arbiter>` (forwarding + rescheduling stay in the
  switch). The default `RoundRobinArbiter` grants every port whose VOQ is
  non-empty and whose egress link is free — the non-blocking crossbar model.
  Install another policy on a switch via `NvSwitch::SetArbiter()` (or, on the
  multi-node path, `MultiNodeTopologyHelper::SetArbiter()` / the `--arbiter`
  CLI flag, e.g. `--arbiter=ns3::WfqArbiter`).
- **Flow control — virtual send-gate hook.** `FabricEndpoint::FlowControlGate`
  is `virtual`. The built-in `FlowControlPolicy` enum (CREDIT/WINDOW/RATE) is
  the 3-policy fast path; a subclass can override `FlowControlGate` to plug an
  arbitrary policy beyond the enum without editing the base or the single
  send-gate call site.
- **Switching — `FabricSwitch` abstract base + type selector.** Every switch
  derives from `FabricSwitch` (pure virtuals: `AddPort`, `GetNPorts`, `GetPort`,
  `AddStaticRoute`, `GetVendorName`). `NvSwitchHelper::SetSwitchType(typeId)`
  selects any `FabricSwitch` subclass as the switch implementation (default
  `ns3::NvSwitch`); `Install`/`AddPort` operate on the `FabricSwitch` base so a
  non-`NvSwitch` subclass plugs in directly.

## Topology grammar

`surrogate/topo/topo_grammar.py` enumerates 12 topology families across their
parametric variations and link technologies (electrical / optical, NVLink /
RoCE / ICI), yielding the topology axis of the design space:

`ring`, `fullmesh`, `hypercube`, `3d_torus`, `mesh2d`, `2dfullmesh`,
`switched` (single NVSwitch), `nvl72`, `multiplane`, `leafspine`,
`3levelhier`, `fattree`, `railfattree`, `dragonflyplus`, `2dfullmeshclos`.

Each `TopoSpec` exports physical terms the surrogate consumes: `hop_count`,
`bw_eff_gbps`, `step_count`, `is_switched`, `supports_nvls`, plus a
`to_ns3_cli()` that emits the exact `--topology=…` + link-tech flags the
simulator understands. Feasibility is enforced against shipping hardware
limits: ≤72 GPUs for a single NVLink fabric (NVL72), ≤18 NVLinks/GPU (H200
NVLink4), ≤576 ports/switch chassis, and NVLS only within a single
NVSwitch ASIC domain (≤8 GPUs).
