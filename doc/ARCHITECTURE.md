# Architecture

ProtoBridge models GPU-fabric communication at L2/MAC, below any IP stack.
This document describes the layering, the fabric header, the flow-control /
spraying / reorder model, the NVSwitch datapath, the resilience tiers, and the
topology grammar.

## Layering: C1 (operation → packet) and C2 (packet execution)

**C1 — operation to packet.** A collective injector (ring-allreduce,
tree-allreduce, nvls-allgather, sharp-allreduce, fullmesh, alltoall, …)
decomposes a tensor operation into a stream of fabric packets. For each packet
it asks:

- `ProtocolModel` (one of 7 vendor models: `NcclProtocolModel`,
  `UbProtocolModel`, `HccsProtocolModel`, `IfProtocolModel`,
  `McclProtocolModel`, `RoceProtocolModel`, `IciProtocolModel`) to select the
  wire protocol (e.g. NCCL LL / LL128 / SIMPLE) and wire efficiency.
- `ProtocolPayloadBuilder` to pack the tensor chunk into the payload.
- `FabricHeader` to stamp packet type, sequence number, flow ID, virtual
  channel, source/dest rank.

**C2 — packet execution.** Each packet traverses:

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
