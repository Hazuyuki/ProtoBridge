# GPU-initiated RDMA submission

For CPU submission, host-memory payloads, GPU staging, and the common C++
interface, see [RDMA.md](RDMA.md). IBGDA is the GPU-initiated GPUDirect case of
that model. Existing examples and `ibgda*` names on this page remain supported.

ProtoBridge has these selectable RDMA timing paths:

| `rdmaSubmission` | Behavior |
|---|---|
| `startup` | Default. Keeps the existing startup-delay, packetization and completion behavior. No IBGDA runtime is mounted. |
| `detailed` | Select CPU/GPU initiation with `rdmaInitiator` and host/staged/GPUDirect payloads with `rdmaDataPath`. |
| `ibgda` | Adds GPU WQE generation, per-peer RC QPs, finite WR queues, doorbell batching, NIC fetch, chunk DMA, ACK aggregation and GPU-observed completion. |

These options select a submission model. Both use the existing packet network.
The current IBGDA implementation runs over **RoCE**, not an InfiniBand link-layer
model. It is a communication timing model; running it needs neither CUDA nor a
physical GPU or NIC.

## Run an example

Build once:

```bash
./ns3 build gpu-cluster-sim
```

Run the example profile:

```bash
./ns3 run "gpu-cluster-sim --protocolConfig=configs/protocol_configs/ibgda-p2p.cfg --numNodes=2 --gpusPerNode=2 --dataSize=8192 --switchCutThroughDelay=649 --ibgdaTraceFile=/tmp/ibgda.csv"
```

The profile contains **illustrative, uncalibrated stage times**. They are not
perftest defaults or measurements of a specific GPU. The example uses one
transfer from rank 0 to the final rank, ensuring it crosses a node boundary.

For repeated writes and queue-depth experiments, use the inline P2P workload.
Rank 1's next rank is rank 2, on the other node:

```bash
./ns3 run "gpu-cluster-sim --numNodes=2 --gpusPerNode=2 --collective=p2p --rootRank=1 --interNodeTopology=host --interNodeFabricType=RoCE --interNodeBandwidth=200 --interNodeLatency=0 --switchCutThroughDelay=649 --dataSize=512 --p2pIterations=5000 --rdmaSubmission=ibgda --ibgdaProducers=4 --ibgdaQpsPerPeer=2 --ibgdaQueueDepth=128 --ibgdaDoorbellBatch=16 --ibgdaCqModeration=16 --ibgdaWqeBuildNs=100 --ibgdaDoorbellNs=100 --ibgdaWqeFetchNs=100 --ibgdaTraceFile=/tmp/ibgda-writes.csv"
```

The 100-ns values in this command are example inputs. Replace them with measured
stage costs before comparing with hardware. All timing inputs default to zero
except the tail-batch timeout, which defaults to 100 ns. A zero bandwidth input
removes that bandwidth constraint; it does not derive a hardware bandwidth.

For an existing run, select `--rdmaSubmission=startup`,
`--rdmaSubmission=ibgda`, or `detailed` with explicit initiator/path choices.
Omitting the flag selects `startup`. As with other
ProtoBridge hardware settings, an explicit `[stack]` / profile value takes
precedence over the corresponding CLI flag. To switch a profile-based run back
to legacy timing, change `rdmaSubmission` in the configuration to `startup`.

## Configuration and WebUI

Set `rdmaSubmission` and any `ibgda*` parameters in `[stack]` or the referenced
`.profile`. The same names are native CLI flags. Mode and parameters are also
available in the WebUI's **RDMA submission** section, in English and Chinese.
IBGDA requires a multi-node RoCE scenario in the main simulator.

The profile example retains `StartupDelayNs = 40000` to make mode selection
explicit: it is suppressed in IBGDA mode and retained in startup mode. The
RoCE startup scalar, stencil `[op] startup` (including numeric values), and
hierarchical inter-node startup are suppressed in IBGDA mode. Intra-node
collective startup and explicit compute/inter-step delays remain in place.
Application initialization, memory registration and kernel launch belong in
the upper simulator's request-ready times; they are not charged per WQE.

| Parameter | Default | Meaning |
|---|---:|---|
| `ibgdaProducers` | 1 | Abstract GPU producers, e.g. warps/CTAs |
| `ibgdaQpsPerPeer` | 1 | Independent RC queues for each remote peer |
| `ibgdaQueueDepth` | 128 | Outstanding WR slots per QP |
| `ibgdaMapQpByProducer` | 1 | Producer-to-QP mapping; 0 selects round robin per request |
| `ibgdaDoorbellBatch` | 1 | Published WRs before ringing a doorbell |
| `ibgdaBatchTimeoutNs` | 100 | Flush a partial published batch after this interval |
| `ibgdaCqModeration` | 1 | Completion grouping threshold; a completed tail is also signaled |
| `ibgdaMtu` | 4096 | Maximum abstract packet body, excluding FabricHeader/PPP |
| `ibgdaWqeBytes` | 64 | Bytes fetched for one abstract WQE |
| `ibgdaWqeBuildNs` | 0 | Serial construction service per GPU producer |
| `ibgdaPublishNs` | 0 | Ordered publication service per QP |
| `ibgdaDoorbellNs` | 0 | Shared NIC doorbell service |
| `ibgdaWqeFetchNs` | 0 | WQE fetch latency, ordered within each QP |
| `ibgdaDmaReadNs` | 0 | Source DMA latency per chunk |
| `ibgdaDmaWriteNs` | 0 | Destination DMA latency per accepted chunk |
| `ibgdaCqWriteNs` | 0 | Completion-write latency |
| `ibgdaGpuPollNs` | 0 | Delay until the GPU observes completion |
| `ibgdaPcieReadGbps` / `ibgdaPcieWriteGbps` | 0 | Shared PCIe read/write capacity in decimal Gb/s |
| `ibgdaHbmReadGbps` / `ibgdaHbmWriteGbps` | 0 | Shared HBM read/write capacity in decimal Gb/s |
| `ibgdaTraceFile` | empty | Native CLI output path for per-WR timing CSV |
| `p2pIterations` | 1 | WRs per inline p2p/fanout/incast flow; not used by stencil configs |

Counts must be positive. Doorbell batch and CQ moderation cannot exceed the
QP depth. Batching with a threshold above one requires a positive tail timeout.
Bandwidths must be finite and nonnegative. `ibgdaMtu` is bounded at 1 MiB.

`soTxWindow` continues to count in-flight **packets/chunks**. It is independent
of `ibgdaQueueDepth`, which counts **work requests**. For example, one 9,000-B
WR can occupy three 4-KiB-or-smaller packet slots, while occupying one WR slot.

## Event flow and layer boundary

OTP supplies logical transfers and dependencies. Remote
`FabricEndpoint::SendBulkWireTransferSize` calls enter the IBGDA runtime when
mounted; local sends continue on the scale-up path.

```text
request/data ready
  -> reserve a WR slot
  -> GPU producer constructs WQE
  -> publish contiguous SQ prefix
  -> batch / tail timeout -> doorbell
  -> NIC WQE fetch
  -> packetize -> source chunk DMA -> existing PEX network
  -> receiver accepts/reassembles chunk -> destination DMA -> delivery / ACK
  -> all fragments of WR acknowledged
  -> CQ write -> GPU observes completion -> recycle WR slot
```

Source and destination DMA operate on chunks, allowing memory service and
network transmission to overlap. HBM and PCIe are modeled as independent
pipeline capacity constraints, with the larger chunk-ready time governing
injection. NIC WQE fetches share the source PCIe-read resource. This abstraction
does not require buffering the complete transfer before its first packet.

An ACK only accounts for its packet's effective bytes. A WR completes after
its complete byte ledger is acknowledged and the GPU observes its completion.
Duplicate ACKs cannot reclaim a slot twice. A short doorbell batch and a short
completion group drain without another request. Permanent loss cannot produce
a successful WR completion.

The runtime exposes:

- `FabricEndpoint::SubmitIbgdaWrite(...)`: explicit producer ID and per-WR
  completion callback for an upper-level simulator.
- `IbgdaSubmissionModel::Quiet(callback)`: flush published batches and wait for
  GPU-observed completion of requests issued before the call. Later requests
  are outside this watermark.
- `IbgdaSubmissionModel::Fence(peer)`: order later same-peer requests across
  QPs by waiting for earlier remote completions. This is a conservative,
  stronger implementation than the minimum ordering guarantee of a fence.
- `SetObserver(...)`: inspect each completed WR's timing and producer/QP IDs.

The generic config runner waits for IBGDA quiet before reporting completion.
Inline collective runs similarly drain outstanding IBGDA work before stopping.
Fixed-duration random-traffic runs still stop at the measurement-window end;
they may intentionally report outstanding work.

## Reading results

`rdmaSubmission` records the selected mode in the RESULT block. IBGDA adds
submitted/completed/failed WR counts, doorbell/CQE counts, peak total and
per-QP occupancy, startup sample count, mean/max startup, and
`dataDeliveryDurationNs`.

Startup is measured at the source device's **physical first-transmit trace**:

```text
startup = first packet starts transmission - logical request/data ready
```

It includes WR-slot waiting and batching. It is separate from the steady-state
interval between WQEs. Per-WR trace columns include request-ready, reservation,
publication, doorbell, NIC execution, physical first wire, final ACK and
GPU-observed completion times. `simTimeNs` includes required completion drain;
`dataDeliveryDurationNs` records application data delivery before that drain.

Use stage measurements to set the timing inputs, then validate against
GPU-initiated benchmarks while varying message size, producer count, QP count,
WR depth and batching. CPU-issued `ib_write_bw` submission fits do not identify
GPU WQE-construction costs. A configured mechanism is not evidence that its
unmeasured hardware parameters are accurate.

## Current scope

This version models the IBGDA **RC RDMA Write submission/completion path** and
its use by remote bulk transfers and collectives. It reuses ProtoBridge's
abstract FabricHeader/SoHeader/PPP byte accounting and existing congestion,
flow-control and retry paths. It is not a literal mlx5 WQE/BTH encoder.

Read/Get, Send/Recv, immediate notifications, 64-bit atomics and PutSignal/Wait/Test
are supported by the [operation API](RDMA_OPERATIONS.md), including GPU initiation.
The examples on this page describe the original bulk Write interface.

DCI/DCT connection switching, GPU instruction
execution, detailed cache effects, and SM occupancy interference with compute
are not implemented here. GPU parallelism is represented by producer service
resources and producer-to-QP mapping. Native InfiniBand link-layer credits and
packet framing are also outside this RoCE-backed implementation.

The existing analytical latency surrogate is unchanged; its `sendrecv`
startup scalar does not automatically acquire these stateful queue semantics.

## Validation

```bash
./ns3 build test-runner gpu-cluster-sim
./ns3 run "test-runner --suite=gpu-cluster-ibgda --verbose" --no-build
python -m pytest test/parity/test_ibgda.py webui/tests -q
```

The tests cover analytical stage timing, finite per-QP WR depth, concurrent QPs,
partial doorbell and CQ groups, multi-fragment completion, fence ordering,
event cancellation, real endpoint ACKs, configuration precedence, mode parity
and English/Chinese UI metadata.

Mechanism references: [NVIDIA GDA-KI workflow](https://developer.nvidia.com/blog/doca-gpunetio-gda-ki-unified-gpu-networking/),
[NVSHMEM performance](https://docs.nvidia.com/nvshmem/release-notes-install-guide/best-practice-guide/performance.html),
and [NVSHMEM ordering](https://docs.nvidia.com/nvshmem/api/latest/gen/api/ordering.html).
