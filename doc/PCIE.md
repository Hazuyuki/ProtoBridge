# PCIe packet model

Detailed RDMA can use either the existing bandwidth model or an error-free
PCIe TLP model. The choice is independent of CPU/GPU initiation and the
host/staged/GPUDirect payload path. The default remains `pcieModel=bandwidth`.

## Run

```bash
./ns3 build gpu-cluster-sim
./ns3 run "gpu-cluster-sim --numNodes=2 --gpusPerNode=2 --interNodeTopology=host --interNodeFabricType=RoCE --collective=rdma --algorithm=p2p --rdmaSubmission=detailed --rdmaInitiator=gpu --rdmaDataPath=gpudirect --rdmaOperation=write --dataSize=1048576 --pcieModel=tlp --pcieTraceFile=/tmp/pcie.csv --rdmaTraceFile=/tmp/rdma.csv"
```

`pcieModel=tlp` also works with `rdmaSubmission=ibgda`, the existing transfer
stencils, and detailed multi-node collectives. Change `rdmaOperation` to `read`,
`send`, `write_imm`, `send_imm`, `fetch_add`, `compare_swap`, `put_signal`, or
`signal` for the native RDMA workload. Use `rdmaDataPath=staged` to include copies
through host DRAM.

PCIe controls work as CLI flags and profile/`[stack]` keys. Apply the
common [configuration precedence](CONFIG_GUIDE.md#configuration-precedence)
when switching an existing profile to `pcieModel=tlp`.

## Paths and shared resources

Each simulated host owns one PCIe fabric. The default `pcieTopology=switch`
connects host DRAM/root complex to a shared switch, with a separate GPU port and
NIC port for each RDMA endpoint. GPU-to-NIC P2P stays below the root uplink.
Traffic to host DRAM shares that uplink across all endpoints on the host.
`pcieTopology=root` attaches the GPU/NIC ports directly to the root complex, so
P2P crosses the root's forwarding path instead. Every link has independent
transmit and receive directions.

| Activity | PCIe transactions |
|---|---|
| Doorbell | CPU/root or GPU sends an MWr to its NIC. |
| WQE fetch | NIC sends MRd to host DRAM for CPU submission, or HBM for GPU submission; CplD returns the descriptor. |
| NIC payload read | NIC sends MRd to the selected source memory; CplD returns the data. |
| NIC payload write | NIC sends MWr to the selected destination memory. |
| D2H staging | GPU sends MWr to host DRAM. |
| H2D staging | GPU sends MRd to host DRAM; CplD returns data into HBM. |
| CQ write | NIC sends MWr to the initiating CPU's host memory or GPU's HBM. Receive completions also use this path. |

TLPs use source/target memory service queues as appropriate. Host DRAM service
is shared by the host's NICs; each GPU has its own HBM read/write service queues.
Copy-engine rate limits can overlap PCIe transfer service. A copy completes after
both constraints finish, plus its configured fixed device latency.

The TLP model replaces `rdmaPcieReadGbps` and `rdmaPcieWriteGbps`; those two legacy
inputs have no effect in TLP mode. HBM/host-memory rates, copy-engine rates, and
fixed device costs still apply. Defaults and shipped examples are mechanism
inputs, not measured GPU/NIC calibration.

## Packet execution

The transaction adapter splits Memory Writes at MPS and 4-KiB boundaries, and
Memory Reads at MRRS, 4-KiB, and configured read-byte limits. It accounts for
unaligned addresses and byte enables. Read responses split into CplD TLPs at MPS,
with non-final completions ending on an RCB boundary. Tag state matches and
reassembles all valid response bytes before releasing a read request.

PEX executes actual `ns3::Packet` objects carrying `PcieTlpHeader`:

- Memory requests route by their mapped destination address; completions route
  by Requester ID. The model assigns each endpoint a synthetic 1-TiB window.
  RDMA data values and registered addresses remain owned by the RDMA operation
  engine; PCIe supplies transfer timing and resource contention. The RDMA adapter
  retains the low 12 address bits for byte alignment and 4-KiB boundaries.
  Staging buffers use synthetic aligned regions; this is not a page-table model.
- Each output has a bounded TLP queue. Received packets retain their incoming
  buffer credits while waiting for an output. Backpressure holds packets rather
  than dropping them.
- VC0 has six independent credit pools per link direction: PH, PD, NPH, NPD,
  CplH, and CplD. Header units count packets; data units count 16 bytes. MRd uses
  NPH, MWr uses PH/PD, and CplD uses CplH/CplD. This subset has no Non-Posted
  data-bearing requests, so NPD is configured but not consumed.
- Credits return when receive buffers drain. After `pcieCreditUpdateNs`, UpdateFC
  DLLPs arbitrate for the reverse link. The sender regains credits when those
  updates arrive. Flow updates are separate from link ACKs.
- Eligible TLP classes use round-robin arbitration. Posted writes can pass
  blocked Non-Posted reads. Completions respect earlier Posted traffic unless
  RO permits bypass. Same-tag completions retain order; different tags can
  complete independently. The baseline conservatively preserves Non-Posted
  request order within an input stream. DLLPs take priority over TLPs.
- Each requester has finite tags, outstanding-read bytes, and reserved completion
  payload space. These resources are independent of the RDMA WQE queue, RDMA
  Read/atomic credits, and hop-by-hop receive-buffer credits.
- Transmitted TLPs occupy a finite link replay buffer until a cumulative ACK
  arrives. Normal ACK batching and tail flushing run even without errors.
  No NAK, corruption, retransmission, or FEC path is used.

Link service uses the selected Gen3/4/5 lane rate and 128b/130b encoding. The
model charges 16-byte Memory Request headers, 12-byte Completion headers,
DWORD payload padding, an 8-byte non-FLIT TLP link overhead, and 8 bytes per DLLP.
It does not model SKP scheduling or individual physical symbols. A nonempty
transmission occupies at least one ns-3 time tick. Forwarding starts after a
complete TLP arrives; switch cut-through and vendor-specific crossbar pipelines
are not modeled.

Three events have different meanings: a **link ACK** acknowledges reception on
one hop; **CplD** returns data for a PCIe read; an **RDMA CQE** reports RDMA
completion to software. MWr is posted and creates no transaction Completion TLP.
Its simulation callback indicates endpoint memory visibility.

## Parameters

All PCIe links in the generated host topology use the same link configuration.
The C++ topology API also supports explicit endpoint/switch connectivity.

| Parameter | Default | Meaning |
|---|---:|---|
| `pcieModel` | `bandwidth` | `bandwidth` or `tlp`; TLP requires detailed RDMA/IBGDA. |
| `pcieTopology` | `switch` | `switch` or `root`. |
| `pcieGeneration`, `pcieLanes` | 4, 16 | Gen3–5; x1/x2/x4/x8/x16/x32. |
| `pcieMps`, `pcieMrrs`, `pcieRcb` | 256, 512, 128 | Bytes; MPS/MRRS are powers of two from 128 to 4096; RCB is 64 or 128. |
| `pcieTags` | 32 | Tags per requester, 1–256; extended tag capability is assumed when needed. |
| `pcieMaxReadBytes` | 16384 | Maximum valid bytes in outstanding PCIe reads per requester. |
| `pcieCompletionBufferBytes` | 32768 | Completion payload space reserved for outstanding reads. |
| `pcieQueuePackets` | 64 | TLP capacity per output queue, 1–65536. |
| `pciePostedHeaderCredits`, `pciePostedDataCredits` | 32, 512 | PH packets, PD 16-byte units. |
| `pcieNonPostedHeaderCredits`, `pcieNonPostedDataCredits` | 32, 32 | NPH packets, NPD 16-byte units. |
| `pcieCompletionHeaderCredits`, `pcieCompletionDataCredits` | 32, 512 | CplH packets, CplD 16-byte units. |
| `pcieLinkLatencyNs` | 10 | Propagation latency per hop. |
| `pcieSwitchLatencyNs`, `pcieRootLatencyNs` | 20, 50 | Forwarding latency through a switch or root complex. |
| `pcieCreditUpdateNs` | 50 | Receive-buffer release to UpdateFC eligibility. |
| `pcieReplayBufferBytes` | 65536 | Per-direction storage retained until link ACK. |
| `pcieAckEvery`, `pcieAckDelayNs` | 8, 50 | ACK batch size and maximum tail wait after the first unacknowledged arrival. |
| `pcieRelaxedOrdering` | 0 | Set RO on generated requests and their responses. |
| `rdmaDoorbellBytes`, `rdmaCqeBytes` | 8, 64 | Illustrative doorbell/CQE sizes, configurable independently of WQE size. |

Each Posted/Completion data pool must hold an MPS-sized payload. The replay
buffer must hold a maximum-sized TLP. Zero credit/ACK delays are supported;
transmission still takes link service time. No PCIe BER or packet-loss option is
provided. Existing Ethernet/RoCE error controls apply to the network separately.

## Observe and integrate

Native result fields include `pcieReadRequests`, `pcieWriteTlps`,
`pcieCompletionTlps`, `pcieTlpHops`, `pcieWireBytes`, credit/queue/read-window/order
stall counters, `pciePeakOutstandingReads`, `pciePeakOutstandingReadBytes`, and
`pcieBytes_payload/wqe/doorbell/cq/d2h/h2d`. TLP counts are logical packets;
`pcieTlpHops` and wire bytes include every traversed link. Traffic byte counters
count valid bytes once per PCIe transfer, so source and destination DMA both
contribute to payload bytes. Peaks are the maximum observed on one resource,
not sums across hosts. Stall counters count entries into a blocked condition,
not stalled nanoseconds.

`--pcieTraceFile=/path/pcie.csv` writes host, time, event, endpoints, transaction,
TLP type, traffic class, tag, address, valid/wire bytes and six available-credit
balances. `tx`/`rx` describe TLP hops; `ack_tx`/`update_fc_tx` describe outgoing
DLLPs. `link_ack`/`credit_update` describe application of feedback to the
original data direction. Link ACKs and flow updates still in flight when a
workload stops need not have arrived. A CSV is limited to 100000 rows by default;
`pcieTraceEvents` and `pcieTraceTruncated` report its extent. Set the CLI-only
`--pcieTraceMaxEvents=0` for an unlimited trace. Aggregate counters are unaffected.

The WebUI exposes these controls under **Local PCIe**, after selecting detailed
RDMA or IBGDA. TLP runs include `pcie.csv` in the run archive and at
`/api/runs/<id>/artifacts/pcie.csv`.

For C++ integration, use `PcieFabric::Configure`, `AddRootComplex`,
`AddEndpoint`, `AddSwitch`, and `AddLink`, then issue asynchronous `Read`/`Write`
operations. The [topology development guide](TOPOLOGY_GUIDE.md#pcie-example-connect-gpu-and-nic-endpoints-explicitly)
walks through a complete example and explains the additional binding needed for
custom RDMA GPU/NIC placement. Memory-service callbacks return nonnegative delays;
the fabric invokes them at the relevant endpoints. `TlpTx` and `TlpRx` trace sources expose
packets and hop endpoint IDs. `SetObserver` provides the richer CSV event data.
Build the topology before traffic. With RDMA runtimes, share one
`RdmaHostMemoryResources` across the host and set it before submitting work;
the runtime builds GPU/NIC attachments during this call. Use `SourceRead`,
`DestinationWrite`, and `DestinationH2d` callbacks for either model. The legacy
`Reserve*` functions return scalar delays and reject TLP mode.

`SourceRead` and `DestinationWrite` accept an optional memory address after the
callback. Explicit RDMA operations pass registered-buffer/fragment offsets;
timing-only bulk transfers carry a local metadata offset on each fragment.

This version covers steady-state memory traffic on established links. It omits
configuration enumeration, LTSSM, ASPM, IOMMU/ATS, caches/coherence, PCIe AtomicOp
TLPs, ID-based ordering, multiple VCs/TCs, and Gen6 FLIT/FEC. RDMA atomics continue
to execute at the responder NIC and use modeled memory reads/writes. The model
does not model internal NIC SRAM occupancy or instruction execution. It is for
performance simulation, not bit-level PCIe compliance verification.

## Validation

```bash
./ns3 build gpu-cluster-sim test-runner
./ns3 run "test-runner --suite=gpu-cluster-pcie --verbose" --no-build
python -m pytest test/parity/test_pcie.py webui/tests/test_pcie_ui.py -q
```

Protocol references: [Altera credit update loop](https://docs.altera.com/r/docs/683733/18.0/arria-v-avalon-st-interface-for-pcie-solutions-user-guide/throughput-optimization?contentId=m5Zxk3oXPYfIAbJ_UOCfmA),
[AMD transaction ordering](https://docs.amd.com/r/en-US/pg156-ultrascale-pcie-gen3/Receive-Transaction-Ordering),
[AMD Tag management](https://docs.amd.com/r/en-US/pg156-ultrascale-pcie-gen3/Tag-Management-for-Non-Posted-Transactions),
and [Microchip data-link behavior](https://onlinedocs.microchip.com/oxy/GUID-E54C85EF-C33B-4E1E-A006-276B04B0FF9B-en-US-12/GUID-FD0C6C13-962D-4F56-BAE6-B3301F9F5E4F.html).
