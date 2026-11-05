# RDMA and IBGDA

ProtoBridge models RC RDMA operations over its existing RoCE packet network. The
submission model determines how a request reaches the NIC. The payload path
determines which memory the NIC reads and writes. These choices are independent.
No physical GPU, NIC, CUDA installation, or RDMA driver is needed.

The [operation API](#operations) supports addressed Write, Read/Get, Send/Recv,
immediate notifications, 64-bit atomics and PutSignal/Wait/Test. Timing-only
bulk Write and collective transfers use the same submission resources.

## Choose a mode

| Setting | Values | Purpose |
|---|---|---|
| `rdmaSubmission` | `startup` (default), `detailed`, `ibgda` | Legacy startup delay or explicit submission events. `ibgda` is the compatibility preset for detailed GPU submission with GPUDirect. |
| `rdmaInitiator` | `cpu`, `gpu` (default) | Selects worker/producer count and WQE construction, publication, doorbell, and completion-observation costs. |
| `rdmaDataPath` | `host`, `staged`, `gpudirect` (default) | Host DRAM, GPU payload copied through host DRAM, or direct NIC access to GPU memory. |
| `pcieModel` | `bandwidth` (default), `tlp` | Legacy PCIe bandwidth/latency or shared, error-free TLP execution; see [PCIE.md](PCIE.md). |

`startup` keeps the existing behavior and ignores detailed submission/memory
parameters. `detailed` supports all six initiator/path combinations. The four
common arrangements are:

| Arrangement | Initiator | Payload path | Example config |
|---|---|---|---|
| Host-memory RDMA | CPU | Host DRAM → NIC → network → NIC → host DRAM | `rdma-host-p2p.cfg` |
| GPU payload with host staging | CPU | HBM → host DRAM → NIC → network → NIC → host DRAM → HBM | `rdma-staged-p2p.cfg` |
| CPU-initiated GPUDirect RDMA | CPU | HBM → NIC → network → NIC → HBM | `rdma-gpudirect-p2p.cfg` |
| GPU-initiated RDMA / IBGDA | GPU | HBM → NIC → network → NIC → HBM | `ibgda-p2p.cfg` |

The last two share a payload path. They differ in who constructs and publishes
WQEs, where the NIC fetches descriptors, and who observes CQ completion. CPU
handoff is an optional cost for CPU-issued GPU transfers. Selecting GPU
initiation does not change the network bandwidth or switch delay.

## Run an example

```bash
./ns3 build gpu-cluster-sim
./ns3 run "gpu-cluster-sim --protocolConfig=configs/protocol_configs/rdma-staged-p2p.cfg --numNodes=2 --gpusPerNode=2 --dataSize=1048576 --rdmaTraceFile=/tmp/rdma.csv"
```

Replace the config filename with another example from the table. These stencils
send from rank 0 to the last rank, crossing a node boundary. The main executable
requires at least two nodes and `interNodeFabricType=RoCE` for detailed timing.

The three CPU examples share `configs/protocol_profiles/rdma-common.profile`.
Its costs and bandwidths are **illustrative inputs, without hardware fitting**.
The IBGDA example retains its older profile; its numbers are not a controlled
comparison with the CPU examples. For a controlled comparison, keep one profile
and change `rdmaInitiator`, with explicit CPU/GPU costs and parallelism.

For repeated writes without a stencil:

```bash
./ns3 run "gpu-cluster-sim --numNodes=2 --gpusPerNode=2 --collective=p2p --rootRank=1 --interNodeTopology=host --interNodeFabricType=RoCE --interNodeBandwidth=200 --interNodeLatency=0 --switchCutThroughDelay=649 --dataSize=8192 --p2pIterations=100 --rdmaSubmission=detailed --rdmaInitiator=cpu --rdmaDataPath=gpudirect --rdmaCpuWqeBuildNs=100 --rdmaQueueDepth=128 --rdmaTraceFile=/tmp/rdma.csv"
```

The 100 ns cost is an example, not a perftest default. Timing inputs default to
zero except `rdmaBatchTimeoutNs=100`. Bandwidth inputs default to zero, meaning
unconstrained. The simulator does not infer missing hardware costs.

## Events and resources

For host-memory or direct GPU payloads:

```text
logical request/data ready
  → optional GPU-to-CPU handoff (CPU initiation, GPU payload only)
  → reserve a finite per-QP WR slot
  → CPU worker / GPU producer constructs WQE
  → publish ordered SQ prefix → doorbell batch / tail timeout
  → NIC fetches WQE from host memory (CPU) or HBM (GPU)
  → packetize → source memory read + PCIe service → PEX network
  → destination PCIe service + memory write → delivery / ACK
  → all WR bytes acknowledged → CQ write → CPU/GPU observes completion
  → release WR slot
```

Staging adds actual copy events and bounded host buffers:

1. Split a user transfer into pieces of at most `rdmaStagingChunkBytes`. Each
   piece becomes a network WR, retaining its application-byte offset.
2. Obtain source staging space and copy that piece from HBM to host DRAM. Only
   then enqueue its WQE. Send the payload from host DRAM.
3. At the receiver, obtain staging space and write accepted packets into host
   DRAM. ACK each packet after its host write finishes.
4. When a complete WR has reached host DRAM, copy it to HBM. Deliver its packets
   to the application after this copy, then release destination staging space.
5. At the source, retain staging space until WR completion is observed, then
   release it and start another copy. This is a conservative buffer lifetime.

Each endpoint has separate source and destination pools, each of size
`rdmaStagingBufferBytes`. Two chunk slots allow copy/network overlap. A full
destination pool queues accepted pieces before host writes and ACKs. This is
abstract receive backpressure, without detailed NIC SRAM occupancy or addresses.

With `pcieModel=bandwidth`, the maximum of memory and copy-engine bandwidth service times
determines readiness, followed by fixed copy latency. NIC DMA similarly uses
the maximum of memory and PCIe service times, followed by fixed DMA latency.
Fixed latencies can overlap; bandwidth service reserves the relevant resources.

The main executable and config runner share host DRAM read/write resources
between endpoints on the same simulated host. HBM, NIC PCIe and D2H/H2D service
resources, CPU workers and GPU producers are per endpoint. `rdmaD2hGbps` /
`rdmaH2dGbps` are separate effective
copy-path limits; they do not contend with NIC PCIe resources. A shared PCIe root
complex and TLP arbitration are outside the bandwidth model. With
`pcieModel=tlp`, DMA, copies, doorbells, WQE reads and CQ writes traverse a shared
PCIe topology with finite queues, six credit pools and finite read tags/bytes.
The TLP mode replaces the two `rdmaPcie*Gbps` limits while retaining memory,
copy-engine and device-service inputs. See [PCIE.md](PCIE.md) for scope and settings.

## Operations

| `rdmaOperation` | Behavior and completion condition |
|---|---|
| `write` | Copy a local registered buffer to a remote registered address. |
| `read` | Send a read request; the responder reads memory and returns payload. Complete after the local destination path finishes. This is also the Get operation. |
| `send` | Copy to the address in a matching posted receive, then produce a receive CQ completion. |
| `write_imm` | Write the remote address and consume a posted receive for notification. Its buffer is not the payload destination; a zero-capacity notification receive is valid. |
| `send_imm` | Send to the posted receive buffer and report a 32-bit immediate value in its CQ completion. |
| `fetch_add` | Atomically add an unsigned 64-bit operand. Return the old value to the issuer's local buffer and completion callback. Arithmetic wraps modulo 2^64. |
| `compare_swap` | Compare a remote 64-bit word and conditionally replace it. Return the old value on both a match and a mismatch. A mismatch is a successful operation. |
| `put_signal` | Transfer all payload chunks, then issue one SET/ADD signal update. The signal cannot become visible before this request's complete payload. |
| `signal` | Update a remote 64-bit signal with SET or ADD. |

Local `TestSignal` / `WaitSignal` support EQ, NE, GT, GE, LT and LE. They inspect
local registered memory and do not generate RDMA reads or network traffic.

### Operation examples

```bash
./ns3 build gpu-cluster-sim
./ns3 run "gpu-cluster-sim --protocolConfig=configs/protocol_configs/rdma-read.cfg --numNodes=2 --gpusPerNode=2 --dataSize=9000 --rdmaTraceFile=/tmp/rdma-read.csv"
```

The other presets are `rdma-send.cfg`, `rdma-atomic.cfg`, and
`rdma-put-signal.cfg`. In the WebUI, choose them under **Multi-node network**.
The **RDMA operation** selector also offers the immediate and standalone signal
variants. CPU/GPU and memory-path controls remain independent.

Without a profile, use `collective=rdma` and select the opcode:

```bash
./ns3 run "gpu-cluster-sim --numNodes=2 --gpusPerNode=2 --collective=rdma --algorithm=p2p --rdmaSubmission=detailed --rdmaInitiator=gpu --rdmaDataPath=gpudirect --rdmaOperation=fetch_add --rdmaInitialValue=10 --rdmaOperand=2 --p2pIterations=32 --interNodeTopology=host --interNodeLatency=0"
```

This performs 32 fetch-adds, leaving the remote word at 74. `rootRank` selects
the issuer; the peer is the same local GPU index on the next node. `dataSize`
is bytes per logical transfer. Atomics and standalone signals operate on an
8-byte word. `p2pIterations` counts logical operations. Example costs remain
illustrative and uncalibrated.

### Operation behavior

Read and atomic requests use the submission model's finite SQ and separate
`rdmaMaxReadAtomic` credits per QP. The responder has an endpoint-wide finite
`rdmaResponderDepth` pool. Read responses and atomic results are real packets in
the reverse direction, with memory service, bandwidth, congestion and retries.
Same-address atomic updates serialize at the responder, including requests from
different QPs. Requests are executed once; retrying a response does not repeat
an atomic update.

Send and immediate variants consume a posted receive. When none matches, the
receiver rejects the whole attempt and reports RNR. The NIC retries the payload
after `rdmaRnrRetryNs`, without rebuilding another WQE. It uses new simulation
packet sequence numbers while retaining the same WR/attempt identity. A receive
posted halfway through a rejected attempt cannot accept its remaining fragments.
`rdmaRnrRetryLimit` bounds retries; exhaustion produces an error completion and
releases the WR slot. Short receive buffers and invalid remote registration keys
also produce errors. `Quiet` reports failure when earlier work has failed.

Staged Read reverses the Write copy direction: the responder performs D2H and
the requester performs H2D. Buffers and copy bandwidth still apply. Staged
logical transfers split into chunk WRs. A staged Send/SendImm/WriteImm therefore
needs one posted receive per network chunk; each produces its own receive CQ
entry with a parent `requestId` and byte `offset` for aggregation.

Staged atomics use a serialized 8-byte D2H, host read/modify/write, and H2D proxy
path at the target. The result then returns to the issuer. This is an abstract
proxy for GPU memory without direct NIC access, rather than a claim that such
a NIC can atomically access GPU HBM. These copies count in the D2H/H2D totals.

PutSignal allocates its signal WR at submission, under the same logical request.
`Quiet` includes it even while data is still waiting for staging space. A failed
payload suppresses the signal update. Its ordering is conservative: the signal
waits for earlier same-peer work as well as its own payload. Explicit `Fence`
remains available for upper-level dependencies. A Read/atomic on an RC SQ also
waits for preceding writes on that SQ to become visible, preventing its control
request from overtaking a payload DMA.

The new operation API reports logical completion after the selected destination
memory path, followed by issuer CQ observation. Staged Read includes local H2D;
addressed Write/Send includes target H2D. This is a composite operation timing
model. The legacy timing-only `SubmitRdmaWrite(peer, bytes, ...)` and automatic
bulk Write path retain their earlier host-memory RDMA completion semantics.
Receiver CQ observation can occur later than source completion, and a local
signal wait has its own observation delay.

## User parameters

These names work as native CLI flags and `[stack]` / `.profile` keys.
`[stack]` overrides the profile, and resolved profile/stack values override CLI
values for the same keys. To switch a config-based run, change its `[stack]`
`rdmaSubmission`; a conflicting CLI flag does not override it.

| Group | Parameters | Meaning |
|---|---|---|
| CPU submission | `rdmaCpuWorkers`, `rdmaCpuWqeBuildNs`, `rdmaCpuPublishNs`, `rdmaCpuDoorbellNs`, `rdmaCpuPollNs` | Separate CPU worker count, construction, publication, doorbell and CQ-observation costs. |
| CPU handoff | `rdmaCpuHandoffNs` | GPU-ready to CPU-orchestration delay, once per logical request. Applies to CPU + `gpudirect` / `staged`; ignored for CPU + `host` and GPU initiation. |
| GPU submission | `rdmaGpuProducers`, `rdmaGpuWqeBuildNs`, `rdmaGpuPublishNs`, `rdmaGpuDoorbellNs`, `rdmaGpuPollNs` | Abstract GPU producer count and corresponding submission/observation costs. |
| Queues | `rdmaQpsPerPeer`, `rdmaQueueDepth`, `rdmaMapQpByProducer` | QPs per destination, outstanding network WRs per QP, producer mapping (`1`) or round robin per WR (`0`). |
| Batching | `rdmaDoorbellBatch`, `rdmaBatchTimeoutNs`, `rdmaCqModeration` | WQEs per doorbell, partial-batch timeout, abstract CQ grouping threshold. Completed tails drain. |
| NIC | `rdmaWqeBytes`, `rdmaWqeFetchNs`, `rdmaMtu`, `rdmaDmaReadNs`, `rdmaDmaWriteNs`, `rdmaCqWriteNs` | Descriptor bytes/fetch time, packet-body limit, per-packet DMA latencies, completion-write latency. |
| Memory / NIC bandwidth | `rdmaHostReadGbps`, `rdmaHostWriteGbps`, `rdmaHbmReadGbps`, `rdmaHbmWriteGbps`, `rdmaPcieReadGbps`, `rdmaPcieWriteGbps` | Effective decimal Gb/s limits, with separate read/write resources. |
| Staging | `rdmaStagingChunkBytes`, `rdmaStagingBufferBytes`, `rdmaD2hNs`, `rdmaH2dNs`, `rdmaD2hGbps`, `rdmaH2dGbps` | Chunk size, capacity of each pool, per-copy fixed costs and bandwidths. |

Default queue depth is 128. Worker, producer, QP, doorbell and CQ counts default
to 1. Default packet-body limit is 4096 B, descriptor size is 64 B, staging chunk
is 64 KiB, and each pool is 128 KiB. A pool must hold at least one chunk.
Doorbell batch and CQ moderation cannot exceed depth. All costs and rates must
be nonnegative. A batch above one needs a positive timeout.

`soTxWindow` limits outstanding **packets**; `rdmaQueueDepth` limits outstanding
**WRs**. Staging may turn one logical transfer into many WRs, each with several
packets. `p2pIterations` counts logical requests per inline P2P/fanout/incast
flow; it does not change stencil replication.

Detailed mode suppresses scale-out RoCE startup, stencil `[op] startup`, and
hierarchical inter-node startup to avoid charging submission twice. Intra-node
startup and explicit compute/inter-step delays remain. Initialization, memory
registration and kernel launch belong in the upper simulator's ready times.

### Operation parameters and results

All operation controls work as CLI flags and profile/`[stack]` keys; stack and
profile precedence follows the submission controls above.

| Parameter | Default | Meaning |
|---|---:|---|
| `rdmaOperation` | `write` | Native `collective=rdma` workload opcode; does not change collective algorithm transfers into reads. |
| `rdmaMaxReadAtomic` | 16 | Outstanding Read/atomic credits per QP; 0 disallows them. |
| `rdmaResponderDepth` | 16 | Concurrent responder operations per endpoint. |
| `rdmaReadServiceNs` / `rdmaAtomicServiceNs` | 0 | Responder service costs. |
| `rdmaReceiveQueueDepth` | 128 | Number of posted/pending-post receive entries. |
| `rdmaReceivePostNs` | 0 | Serial receive-posting cost. |
| `rdmaRnrRetryNs` / `rdmaRnrRetryLimit` | 1000 / 7 | Retry delay and maximum additional attempts after RNR. |
| `rdmaSignalPollNs` | 0 | Delay before a satisfied local wait callback is observed. |
| `rdmaReceivePostDelayNs` | 0 | Native workload's delay before posting receives. |
| `rdmaOperand` / `rdmaCompare` / `rdmaInitialValue` | 1 / 0 / 0 | Native workload atomic operands and initial remote word. |
| `rdmaImmediate` | 0 | Native workload's 32-bit immediate. |
| `rdmaLocalAddress` / `rdmaRemoteAddress` | 0 / 0 | Native workload addresses. |
| `rdmaSignalAddress` / `rdmaSignalValue` / `rdmaSignalOp` | 2^48 / 1 / `set` | Native workload's separate signal location and update. |

The WebUI bounds integer operands to JavaScript's exact-integer range. C++ and
native CLI operands support full uint64 values. Hex result fields preserve exact
64-bit values in the browser as well.

Results include operation success/failure counts, read response bytes, actual
atomic executions, RNR retries/exhaustion, receive completions, signal updates,
Read/atomic occupancy and responder occupancy peaks. `rdmaOperationDataBytes` and
`rdmaOperationControlBytes` count operation body bytes, excluding the common
FabricHeader/SoHeader/PPP headers. Read requests charge 16 B, atomic requests
28 B, standalone signal requests 20 B, and immediate notification 4 B per WR.
Atomic results return 8 B. Arguments live in simulation tags; these sizes are
modeled wire costs, rather than literal mlx5 encodings.

The WR CSV adds `opcode`. For Read/atomic operations, the compatibility column
`ackedNs` records the local operation-ready condition after response placement;
for Write/Send it is the completion ACK arrival. `observedNs` adds CQ observation.
`rdmaDataReadyNs` reports memory readiness. The native workload's delivery
duration includes required source/receive CQ and signal-wait observation, rather
than only the last packet's arrival.

Unsupported combinations do not silently fall back to Write. The operation
workload requires detailed timing and remote RoCE endpoints. Nonempty transfers,
64-bit atomics and ordinary SET/ADD signals are supported. Driver management
verbs, native InfiniBand framing, GPU instruction execution and DCI/DCT setup
are outside this operation model.

## Interfaces for an upper simulator

### Timing-only bulk transfers

Include `ns3/rdma-submission-model.h` and `ns3/fabric-endpoint.h`. Configure a
runtime on **both** remote endpoints before traffic starts:

```cpp
RdmaConfig config;
config.initiator = "cpu";
config.dataPath = "gpudirect";
config.cpuWorkers = 1;
config.cpuWqeBuild = NanoSeconds(100); // replace with a measured service cost
config.queueDepth = 128;
auto runtime = CreateObject<RdmaSubmissionModel>();
runtime->Configure(config);
endpoint->SetRdmaSubmissionModel(runtime);

// Call when the upper simulator makes the data/request ready.
uint64_t requestId = endpoint->SubmitRdmaWrite(
    peerRank, bytes, protocolId, flowId, vcId, /* producer/worker */ 0,
    [](bool success) { /* issuing CPU/GPU has observed completion */ });
runtime->Quiet([](bool success) { /* earlier requests have drained */ });
runtime->Fence(peerRank); // later same-peer work waits for prior remote ACKs
```

`SubmitRdmaWrite` returns a logical request ID and calls completion once, grouping
staged children. `UINT32_MAX` selects workers automatically. Destination
`SetReceiveCallback(...)` reports packet data after its memory path completes;
the upper simulator can aggregate effective bytes for whole-transfer readiness.

`Quiet` covers earlier requests, including chunks waiting for source staging;
later requests are outside its watermark. Completion/quiet mean completion at
registered remote memory, observed by the initiating CPU/GPU. In staged mode,
that memory is **host DRAM**; destination H2D may still be pending. Built-in
workloads wait for both application delivery and submission drain. Fixed-duration
traffic measurements retain their requested measurement window.

`SetObserver(...)` exposes completed-WR timing; `GetStats()` exposes counters;
`GetConfig()` returns settings. When constructing endpoints yourself, call
`SetHostMemoryResources(...)` with one shared
`std::shared_ptr<RdmaHostMemoryResources>` for NICs on the same host, before use.
Use consistent host bandwidth settings for those runtimes.

OTP still supplies transfers and dependencies. The runtime adds submission and
memory events around the existing PEX packet path. Remote
`SendBulkWireTransferSize` calls use it automatically; local NVLink sends retain
their existing path.

### Addressed operations

Configure the submission runtime on both endpoints as above, then register
simulated buffers and initialize the data:

```cpp
source->RegisterRdmaMemory(0, 1 << 20, /* key */ 1);
target->RegisterRdmaMemory(0, 1 << 20, /* key */ 1);
source->GetRdmaOperationEngine()->WriteMemory(0, {1, 2, 3, 4});

RdmaRequest put;
put.opcode = RdmaOpcode::WRITE;
put.peer = target->GetRank();
put.bytes = 4;
put.localAddress = 0;
put.remoteAddress = 4096;
source->SubmitRdmaOperation(put, [](const RdmaCompletion& completion) {
    // completion.requestId / success / bytes / value
});
```

For Read, set `opcode=RdmaOpcode::READ` or call `SubmitRdmaRead(request, cb)`.
The requester supplies the local destination and remote source addresses. For
atomics, use `SubmitRdmaFetchAdd` or `SubmitRdmaCompareSwap`; `operand` is the
addend/replacement and `compare` is the CAS comparison. Remote words and local
atomic-result buffers must be 8-byte aligned.

Post receives before Send, or schedule posting later to exercise RNR:

```cpp
target->PostRdmaReceive(77, /* buffer */ 8192, /* capacity */ 4096,
    [](const RdmaReceiveCompletion& completion) {
        // receiveId, requestId, offset, peer, qp, bytes,
        // success, hasImmediate, immediate, observed
    }, source->GetRank());
```

A receive post that cannot be scheduled within the simulation horizon reports
failure without consuming an RQ slot or advancing the posting ledger.

Receive entries match peer and QP. Defaults `UINT16_MAX` / `UINT32_MAX` accept
any peer/QP, sharing one bounded receive pool per endpoint. PutSignal uses
`signalAddress`, `signalValue`, and `signalOp=RdmaSignalOp::SET|ADD`; put data
and the signal must use distinct, registered locations. On the target:

```cpp
auto engine = target->GetRdmaOperationEngine();
engine->WaitSignal(65536, RdmaComparison::GE, 1,
    [](uint64_t value) { /* data is available; resume dependent work here */ });
bool ready = engine->TestSignal(65536, RdmaComparison::GE, 1);
```

`ReadMemory`, `WriteMemory`, `ReadWord` and `WriteWord` access the simulated
backing store. Addresses are integers in the simulated address space, not host
pointers. Registration keys and ranges are checked. Unwritten bytes read as
zero; a sparse store avoids allocating a large mostly-zero GPU buffer.
Registration and initialization do not advance simulated time. Deregistration
requires drained work, posted receives and waits; `CancelSignalWait` cancels a
pending local wait. There is no full verbs access-permission or QP-error-state
machine.

## Results and WebUI

The WebUI's **RDMA submission** section exposes all three choices. CPU/GPU
controls follow the initiator selection; staging controls appear for the staged
path. Four examples appear under **Multi-node network**, with English/Chinese
labels. Detailed jobs create `rdma.csv`, available at
`/api/runs/<id>/artifacts/rdma.csv` and inside `run.zip`. Native runs use
`--rdmaTraceFile=/path/rdma.csv`.

| Result | Interpretation |
|---|---|
| `rdmaSubmittedRequests` / `rdmaCompletedRequests` | Logical user requests, counted once even with staging. |
| `rdmaSubmittedWqes` / `rdmaCompletedWqes` | Network WRs; one per staging chunk in staged mode. |
| `rdmaMeanStartupNs` / `rdmaMaxStartupNs` | Request-ready to physical first transmission of any child WR, including queue/handoff/source-copy waits. |
| `rdmaMeanWqeStartupNs` | Mean delay from parent request-ready to each WR's first transmission. |
| `rdmaCompletionObservedNs` | Latest issuing CPU/GPU completion-observation timestamp. |
| `rdmaDataReadyNs` | Latest destination memory-readiness timestamp, including staged H2D. |
| `rdmaSourceD2hBytes` / `rdmaDestinationH2dBytes` | Actual staging-copy bytes; zero for host/direct paths. |
| `rdmaHostReadBytes`, `rdmaHostWriteBytes`, `rdmaHbmReadBytes`, `rdmaHbmWriteBytes` | Memory traffic, including WQE descriptor reads. |
| `rdmaSourceStagingPeakBytes` / `rdmaDestinationStagingPeakBytes` | Largest pool occupancy on one endpoint, rather than a sum of endpoint peaks. |
| `dataDeliveryDurationNs` / `simTimeNs` | Application delivery duration and final duration including completion drain. |

Absolute timestamps use the ns-3 clock origin; durations can start at collective
launch. ACK return and completion observation can take longer than H2D, so either
completion or data readiness may occur first.

The CSV retains the original IBGDA timing columns and adds `requestId`, `offset`,
`submittedNs`, `stagingAcquiredNs`, `sourceStagedNs`, `initiator`, and `dataPath`.
`ackedNs` is final ACK arrival; `observedNs` includes CQ/observer delay. Staging
timestamps are zero for other paths. Destination readiness is exposed through
receive callbacks and aggregate results, not per-WR in this source-side CSV.

## GPU-initiated RDMA (IBGDA)

`rdmaSubmission=ibgda` selects detailed GPU initiation with GPUDirect payloads.
The GPU producer constructs and publishes the WQE, rings the NIC doorbell, and
observes completion in HBM. Producers can map to separate QPs; WR depth and
doorbell batching determine how much work can overlap.

```bash
./ns3 run "gpu-cluster-sim --protocolConfig=configs/protocol_configs/ibgda-p2p.cfg --numNodes=2 --gpusPerNode=2 --dataSize=1048576 --rdmaTraceFile=/tmp/ibgda.csv"
```

Use the same submission, memory and PCIe controls described above. The preset's
costs are illustrative; CPU-issued perftest data does not identify GPU WQE
construction or publication costs. Startup is measured from logical readiness
to the first physical packet transmission and includes queueing and batching.

The compatibility API `FabricEndpoint::SubmitIbgdaWrite` accepts an explicit
producer ID and completion callback. `IbgdaSubmissionModel` retains `Quiet`,
`Fence`, and observer support. GPU initiation also works with all addressed
operations in this guide. DCI/DCT switching, GPU instructions, cache effects
and SM scheduling are outside the model.

## Compatibility and scope

Existing `rdmaSubmission=ibgda`, `ibgda*` options, `ibgdaTraceFile`, and C++
`Ibgda*` names remain supported. Old GPU costs map to `rdmaGpu*` fields; e.g.
`ibgdaWqeBuildNs` sets `rdmaGpuWqeBuildNs`, not the CPU cost. `ibgda` requires
GPU initiation and GPUDirect; use `detailed` for other combinations.

All modes share abstract RoCE framing, ACK/retry, flow-control and congestion
mechanisms. Host RDMA does not add TCP/IP or an operating-system model. Native
InfiniBand framing, mlx5 instruction encoding, GPU instruction execution and SM
scheduling are outside this version.
CQ moderation groups completions and drains tails; it does not reproduce a
specific NIC's full signaled-WQE policy.

## Validation

```bash
./ns3 build gpu-cluster-sim test-runner
./ns3 run "test-runner --suite=gpu-cluster-rdma-modes --verbose" --no-build
./ns3 run "test-runner --suite=gpu-cluster-ibgda --verbose" --no-build
./ns3 run "test-runner --suite=gpu-cluster-rdma-operations --verbose" --no-build
python -m pytest test/parity/test_rdma*.py test/parity/test_ibgda.py webui/tests -q
```

Checks cover independent choices, CPU/GPU costs, host/HBM selection, copy bytes,
bounded buffers, overlap, WR/request grouping, endpoint completion versus GPU
readiness, quiet, shared host contention, hierarchical collectives, loss
recovery, aliases, config precedence and WebUI execution/trace downloads.
