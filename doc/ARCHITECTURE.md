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

## Configuration boundary

A `.cfg` names a `[stack]` and an `[op]`. The stack loads a reusable protocol
profile and applies overrides; the operation supplies the collective schedule
or a custom transfer stencil. The config runner delegates supported collective
and algorithm pairs to the same injectors as the inline CLI path. The parity
tests check exact `simTimeNs` equality for equivalent inputs.

Custom stencils compile transfer instances and dependencies into the
transaction graph. The config runner supports DATA and P2P sends; addressed
Read/Write, Send/Recv, atomics and signals use the separate
[RDMA operation API](RDMA.md#operations). The
[configuration guide](CONFIG_GUIDE.md) defines syntax, profile keys, precedence
and complete runnable examples.

## FabricHeader layout

```
  Offset  Field              Width
    0     PacketType           8
    1     FabricType            8
    2     VirtualChannel       8
    3     VirtualLane           8
    4     MemoryAccessType      8
    5     FlowId              16
    7     SequenceNumber      32
   11     SourceRank          16
   13     DestRank            16
   15     PayloadSize         32
   19     CreditCount         16
   21     Protocol             8
   22     TTL                   8
   23     EffectiveDataSize   32
   27     SrcMac              48
   33     DstMac              48
                               ── total 39 bytes, 16 fields
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
default 39-byte encoding is what the calibration invariants (the config-vs-inline `simTimeNs` equality asserted by `test/parity/test_config_vs_inline_parity.py`; see [CONTRIBUTING.md](../CONTRIBUTING.md))
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
  CLI flag, e.g.
  `--arbiter=ns3::RoundRobinArbiter`, the only shipped `Arbiter` subclass; pass
  the TypeId of your own `Arbiter` subclass for a custom policy).
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

The `--topology` flag selects the fabric the simulator builds, spanning the
scale-up design space across direct-connect and switched families:

`ring`, `fullmesh`, `hypercube`, `torus`, `mesh`, `2dfullmesh`,
`2dfullmeshclos`, `switched` (single NVSwitch), `nvl72`, `multiplane`,
`leafspine`, `3levelhierarchical`, `fattree`, `railfattree`, `dragonflyplus`.

The design space is bounded by shipping hardware limits: ≤72 GPUs for a
single NVLink fabric (NVL72), ≤18 NVLinks/GPU (H200 NVLink4), and NVLS only
within a single NVSwitch ASIC domain (≤8 GPUs).

## Model limits

CPU/GPU execution, HBM/host-memory service and copy engines use configurable
service queues and latencies. Detailed RDMA adds submission/completion state;
optional PCIe TLP execution supplies local-link queues and contention. Device
instructions, cache/coherence effects and NIC internal SRAM occupancy are not
modeled. See [RDMA](RDMA.md) and [PCIe](PCIE.md) for path-specific limits.

RTTCC probes share the link and queue path but bypass BER corruption and
lossy tail-drop. The receiver immediately reflects a probe without a separate
NIC response-service delay. Link-down and configured packet-loss checks can
still discard control traffic. These assumptions affect interpretation of
RTT-feedback experiments.

PFC uses output-port/destination congestion; its pause counts differ from
models based on ingress priority groups. The
[scale-out comparisons](CALIBRATION.md#pfc-and-scale-out-cross-checks) record
that distinction. Hardware calibration, independent-simulator comparisons and
analytical fits have separate scopes in the calibration guide.
