# RDMA initiation and payload paths

ProtoBridge models RC RDMA operations over its existing RoCE packet network. The
submission model determines how a request reaches the NIC. The payload path
determines which memory the NIC reads and writes. These choices are independent.
No physical GPU, NIC, CUDA installation, or RDMA driver is needed.

Write, Read/Get, Send/Recv, immediate variants, 64-bit FetchAdd/CAS and
PutSignal/Wait/Test are available through the [RDMA operation API and examples](RDMA_OPERATIONS.md).
The bulk-transfer examples below retain their timing-only Write behavior.

## Choose a mode

| Setting | Values | Purpose |
|---|---|---|
| `rdmaSubmission` | `startup` (default), `detailed`, `ibgda` | Legacy startup delay or explicit submission events. `ibgda` is the compatibility preset for detailed GPU submission with GPUDirect. |
| `rdmaInitiator` | `cpu`, `gpu` (default) | Selects worker/producer count and WQE construction, publication, doorbell, and completion-observation costs. |
| `rdmaDataPath` | `host`, `staged`, `gpudirect` (default) | Host DRAM, GPU payload copied through host DRAM, or direct NIC access to GPU memory. |

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

For a copy, the maximum of memory and copy-engine bandwidth service times
determines readiness, followed by fixed copy latency. NIC DMA similarly uses
the maximum of memory and PCIe service times, followed by fixed DMA latency.
Fixed latencies can overlap; bandwidth service reserves the relevant resources.

The main executable and config runner share host DRAM read/write resources
between endpoints on the same simulated host. HBM, NIC PCIe and D2H/H2D service
resources, CPU workers and GPU producers are per endpoint. `rdmaD2hGbps` /
`rdmaH2dGbps` are separate effective
copy-path limits; they do not contend with NIC PCIe resources. A shared PCIe root
complex, caches, page mapping and TLP arbitration are outside this model.

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

## Interfaces for an upper simulator

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

## Compatibility and scope

Existing `rdmaSubmission=ibgda`, `ibgda*` options, `ibgdaTraceFile`, and C++
`Ibgda*` names remain supported. Old GPU costs map to `rdmaGpu*` fields; e.g.
`ibgdaWqeBuildNs` sets `rdmaGpuWqeBuildNs`, not the CPU cost. `ibgda` requires
GPU initiation and GPUDirect; use `detailed` for other combinations. See
[IBGDA.md](IBGDA.md) for the older GPU-specific examples.

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
python -m pytest test/parity/test_rdma_modes.py test/parity/test_ibgda.py webui/tests -q
```

Checks cover independent choices, CPU/GPU costs, host/HBM selection, copy bytes,
bounded buffers, overlap, WR/request grouping, endpoint completion versus GPU
readiness, quiet, shared host contention, hierarchical collectives, loss
recovery, aliases, config precedence and WebUI execution/trace downloads.
