# Architecture

ProtoBridge models interconnect communication through two layers. Workloads
and upper simulators supply operations and data-ready times. OTP turns an
operation into ordered transfers; PEX executes their packet and device events.
For runnable inputs use [Configuration](CONFIG_GUIDE.md); calibration evidence
and recorded comparisons live in [Calibration](CALIBRATION.md).

## Layering: OTP (operation → packet) and PEX (packet execution)

| Layer | Responsibility | Main components |
|---|---|---|
| OTP | Collective schedules, transfer dependencies, protocol selection and completion. | Collective injectors, `ProtocolModel`, `ProtocolPayloadBuilder`, `ProtocolConfig`, transaction graphs. |
| PEX | Admission, serialization, routing, queues, flow control, errors and recovery. | `FabricEndpoint`, `NvSwitch`/`EthernetSwitch`, credits, FEC, LLR, link and contention models. |

`ProtocolModel` selects startup and wire-efficiency rules; payload builders
construct the modeled packet body. NCCL, UB and MCCL models describe scale-up
protocol choices; RoCE and plain Ethernet models supply scale-out behavior.
The shared packet path uses `FabricHeader` for routing and transfer accounting.
No `InternetStackHelper` is installed.

```text
upper workload / ready event
  -> OTP schedule and dependencies
  -> endpoint submission and packetization
  -> PEX queues, links and receiver memory service
  -> delivery/completion callback -> next dependent operation
```

Detailed RDMA wraps remote transfers with submission, DMA and completion events.
Local NVLink transfers retain their scale-up path. The [RDMA guide](RDMA.md)
describes the CPU/GPU and host/staged/GPUDirect event sequences; the
[PCIe guide](PCIE.md) explains local TLP packet execution and resources.

## OTP/PEX boundary: the transaction-graph seam

Graph-backed execution uses `ProtocolTransactionGraph` and
`ProtocolTransactionExecutor`:

| Node or callback | Role |
|---|---|
| `ACTION` | Invoke a transfer action through an executor callback. |
| Delivery `WAIT` | Wait for the matching packet type, ranks, flow/stage and effective byte count. |
| `DELAY` | Insert startup or inter-step service. |
| `COMPLETE` | Finish after the selected terminal dependencies. |
| `NotifyEvent` | Feed a delivery event back into the executor. |

`ProtocolConfigRunner` compiles custom stencils into this graph and maps send
actions to `FabricEndpoint::SendBulkWireTransferSize`. `RingAllReduce` uses
transaction execution by default and also retains a legacy callback path.
Other injectors can manage their schedules directly. These paths share the
endpoint/packet models; they do not all require a config-compiled graph.

## Configuration boundary

A `.cfg` supplies `[stack]` settings and an `[op]` schedule. Supported
collective/algorithm pairs delegate to the same injectors as inline CLI runs.
Custom stencils compile DATA/P2P transfers into transaction graphs. Addressed
RDMA operations use the [operation API](RDMA.md#operations).

See the [configuration guide](CONFIG_GUIDE.md) for syntax, parameter defaults,
precedence and time bounds. Exact config/inline timing parity is checked by
`test/parity/test_config_vs_inline_parity.py`.

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

The default serialized header is 39 bytes. Its fields carry routing, sequence,
flow/VC identity and effective payload accounting. `effectiveDataSize` lets
collective progress count application bytes while the payload builder charges
LL/LL128 or other wire overhead. Current packet types are defined in
[`fabric-header.h`](../src/gpu-cluster/model/fabric-header.h).

This header is an abstraction used by the simulator. Protocol-specific byte
accounting and payload construction are handled by their models. Changing
header size changes serialization and requires new parity/calibration checks.

## Flow control and congestion

### Scale-up credits, spraying and reorder

Local scale-up DATA consumes per-VC credits and waits when its send window is
empty. Receiver buffer release returns credits. `FlowControlGate` provides
the admission interface, including queued sends and the credit-bypass cases.
Scale-out traffic bypasses this local credit accounting and uses its network's
congestion/backpressure mechanisms.

With several physical links, senders spray chunks across the links; receivers
track sequence and flow state to handle reordered arrivals and retries.
NVSwitch VOQs hold traffic by input/output, and the arbiter selects available
outputs. SHARP reduction waits for aligned contributions, adds compute service
and multicasts the result.

### RoCE feedback and pacing

An Ethernet switch marks congested RoCE packets. Receiver feedback generates
CNPs, and `DcqcnRateMachine` updates the sending flow's rate. Inline remote
collective traffic uses a scale-out RoCE payload builder when this loop is
enabled; local traffic keeps its scale-up framing. The per-QP ceiling and
rate machines pace traffic at the endpoint egress.

`RttccRateMachine` instead samples a reflected RTT probe and adjusts the flow
rate relative to a target RTT. The probe traverses the queue/link path and the
receiver reflects it without separate response-service latency. Probes bypass
BER corruption and lossy tail-drop; independent link-down/packet-loss checks
can still discard them. RTTCC and DCQCN pacing combine using the larger delay.

### PFC and loss recovery

PFC parks data directed toward a congested output/destination. Control and
retry traffic can continue, and a resume drains queued data. Static mode uses
pause/resume thresholds; dynamic mode scales its threshold with available
headroom. Timed mode carries a pause duration, refreshes it while congestion
persists, and cancels it on explicit resume. Per-VC thresholds are available
through the switch API; otherwise the configured port-level threshold applies.

`soTxWindow` bounds outstanding scale-out packets/chunks. Strict GBN receive
mode accepts the expected sequence, discards out-of-order data and requests
recovery of the gap. This packet window is independent of the submission
model's WR queue and PCIe's read tags.

PFC requires a switch on the scale-out path. Its output/destination occupancy
criterion differs from ingress-priority-group models, so pause counts are
not interchangeable across simulators. The [recorded PFC comparisons](CALIBRATION.md#pfc-and-scale-out-cross-checks)
give the matched conditions. ECN, PFC and drop thresholds are configured in
[Configuration](CONFIG_GUIDE.md#3-protocol-and-network-parameters).

### Shared contention and workload schedules

Packets compete on actual NetDevice links and switch outputs. An optional
`ContentionModel` adds a weighted service penalty when more than one traffic
class is active. Single-node helpers share one model across endpoints; the
multi-node path uses per-endpoint models. Both use the intra-node rate for
this service term; scale-out switch/link contention is also executed by PEX.
The [contention comparison](CALIBRATION.md#shared-contention-regression)
separates the two effects with controlled weights.

Hierarchical collectives run local and remote phases. The `ncclring` allreduce
schedule instead uses parallel, rail-rotated global rings, allowing local and
remote hops to overlap. Uniform-random traffic supplies a fixed-duration
offered load with warmup and measurement windows. Inputs and counters are
listed in [Configuration](CONFIG_GUIDE.md#3-protocol-and-network-parameters); experiment
scripts are listed in the [example catalog](../examples/gpu-cluster/README.md).

## Four-tier resilience model

| FEC | LLR | Behavior |
|---|---|---|
| off | off | Uncorrected corruption or drops cause permanent loss. |
| on | off | Correctable errors incur decode service; uncorrectable errors remain lost. |
| on | on | Correct FEC-covered errors; recover remaining losses with go-back-N/SACK. |
| off | on | Retry errored/lost packets without FEC correction. |

Control/retry packets bypass BER corruption so recovery can progress. Separate
packet-loss and link-down checks still apply. These mechanisms run as packet
events; the [surrogate equations](SURROGATE.md#mechanism-equations) use an
analytical approximation with their own scope.

## Extension seams: flow control, arbitration, switching

| Extension | Interface | Installation |
|---|---|---|
| Arbitration | `Arbiter::SelectGrants(voqs, outputBusyUntil, now)` chooses output grants. | `NvSwitch::SetArbiter`, `MultiNodeTopologyHelper::SetArbiter`, or `--arbiter=<TypeId>`. |
| Admission | Virtual `FabricEndpoint::FlowControlGate`; built-in policies are CREDIT/WINDOW/RATE. | Subclass the endpoint to provide another admission policy. |
| Switching | `FabricSwitch` provides port, route and vendor interfaces. | `NvSwitchHelper::SetSwitchType` selects the switch class before installation. |

`RoundRobinArbiter` is the shipped arbiter and `NvSwitch` the default switch.
The switch retains forwarding and rescheduling; the arbiter supplies grants.
See [`arbiter.h`](../src/gpu-cluster/model/arbiter.h),
[`fabric-endpoint.h`](../src/gpu-cluster/model/fabric-endpoint.h) and
[`fabric-switch.h`](../src/gpu-cluster/model/fabric-switch.h) for the C++ contracts.

## Topology grammar

The `--topology` flag selects the fabric the simulator builds, spanning the
scale-up design space across direct-connect and switched families:

`ring`, `fullmesh`, `hypercube`, `torus`, `mesh`, `2dfullmesh`,
`2dfullmeshclos`, `switched` (single NVSwitch), `nvl72`, `multiplane`,
`leafspine`, `3levelhierarchical`, `fattree`, `railfattree`, `dragonflyplus`.

Topology-specific limits are enforced by the helpers. Hardware reference
settings and measurement conditions are recorded in [Calibration](CALIBRATION.md).

## Model limits

Device instructions, cache/coherence effects and internal NIC SRAM occupancy
are outside this model. CPU/GPU submission and HBM/host-memory/copy service use
configured resources and latencies. Read the [RDMA](RDMA.md#compatibility-and-scope)
and [PCIe](PCIE.md#observe-and-integrate) scope sections for those paths.
Hardware measurements, third-party references and simulator fits are identified
separately in the calibration guide.
