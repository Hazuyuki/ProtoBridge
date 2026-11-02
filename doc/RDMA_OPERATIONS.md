# RDMA operations

The detailed runtime supports the following operations over ProtoBridge's RC
RoCE packet path. CPU/GPU initiation and host/staged/GPUDirect memory paths work
with every operation. See [RDMA.md](RDMA.md) for submission and memory parameters.

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

## Run an operation

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

## What is modeled

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

## C++ integration

Configure the submission runtime on both endpoints, as in [RDMA.md](RDMA.md).
Then register simulated buffers and initialize the data:

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

## Parameters and results

All operation controls work as CLI flags and profile/`[stack]` keys; stack and
profile precedence follows [RDMA.md](RDMA.md).

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

## Validation and references

```bash
./ns3 build gpu-cluster-sim test-runner
./ns3 run "test-runner --suite=gpu-cluster-rdma-operations --verbose" --no-build
python -m pytest test/parity/test_rdma_operations.py webui/tests -q
```

Tests check actual memory bytes and atomic return values, same-address
serialization, receive matching and immediate notification, RNR transitions,
finite credits/buffers, staging direction, put/signal ordering, quiet coverage,
access errors, packet-loss recovery, and the six initiator/path combinations.

Operation semantics follow the [rdma-core verbs manual](https://man7.org/linux/man-pages/man3/ibv_post_send.3.html)
and [NVSHMEM signaling API](https://docs.nvidia.com/nvshmem/api/latest/gen/api/signal.html).
