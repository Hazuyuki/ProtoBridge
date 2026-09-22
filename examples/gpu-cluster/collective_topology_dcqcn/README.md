# Hierarchical collectives vs So topology (16 nodes x 8 GPUs, DCQCN)

Deterministic-traffic counterpart of
[`random_traffic_dcqcn/`](../random_traffic_dcqcn/): compares the 8
scale-out (`--interNodeTopology`) topologies under the calibrated
hierarchical collectives instead of a synthetic random load.

## Setup

- 16 nodes x 8 GPUs (128 ranks), RoCE 400 Gbps So links, 1 us link latency;
  Su fabric is the calibrated NVLink/NVSwitch model.
- `--collective=allreduce|alltoall --algorithm=hierarchical`, data size
  1..256 MiB per GPU (x4 steps), `--dcqcn=1`, `RngRun=1`.
- Bus bandwidth uses the NCCL convention with N = 128 ranks:
  allreduce `2(N-1)/N * S/t`, alltoall `(N-1)/N * S/t`.
- DCQCN on an inline collective requires the So payload-builder seam:
  `--dcqcn=1` on a RoCE fabric mounts a dedicated `RocePayloadBuilder` for
  inter-node sends (SoHeader + `RoceProtocol::SIMPLE`, which the switch
  ECN-mark and receiver CNP-echo guards key on) while intra-node (Su)
  traffic keeps the NCCL builder's framing. With `--dcqcn=0` (default) the
  seam is unmounted and runs are byte-identical to before.

## Run

```bash
./ns3 build                       # from the repo root
python3 run_sweep.py              # 80 runs (2 collectives x 8 topologies x 5 sizes) -> results.csv
python3 plot_collectives.py       # -> collective_vs_topology.{pdf,png}
```

## Reading the figure

- **fullmesh** wins both collectives at large sizes (allreduce ~463 Gbps,
  alltoall ~694 Gbps busbw at 256 MiB): every node pair has a dedicated
  400G link, so the all-to-all phase of the hierarchical schedule never
  shares a port. No switch in the So path -> ECN never marks.
- **host / tor / rail / leafspine** (switched, one NIC per GPU) cluster
  together (allreduce ~340-360 Gbps, alltoall ~160-210 Gbps): the
  bottleneck is each GPU's single 400G NIC, identical across them; the
  small spread is switch-hop count. These are the only topologies where
  DCQCN is active (ECN/CNP counters in `results.csv`); `tor` and `rail`
  are bit-identical for hierarchical allreduce (same effective So paths)
  but diverge slightly on alltoall.
- **ring** is worst at 128 ranks (allreduce ~123 Gbps, alltoall ~21 Gbps):
  all cross-node traffic of a node funnels through GPU 0's two ring links
  and multi-hop forwarding grows with node count. **torus2d / dragonfly**
  sit in between, ordered by gateway path diversity. The gateway shapes
  have no EthernetSwitch in the So path, so ECN is structurally zero
  there (same finding as the random-traffic sweep).
- As in the random-traffic experiment, DCQCN in this lossless fabric does
  not change completion time (a saturated bottleneck port stays busy either
  way; DCQCN relocates queueing to the senders) -- its effect shows as the
  ECN/CNP activity counters, not as a latency delta.

Smoke gate: `python3 test/parity/test_collective_dcqcn.py` (run directly)
pins the seam's invariants: `--dcqcn=0` stays dormant (ecn=0, unchanged
completion), `--dcqcn=1` lights the ECN->CNP chain on an oversubscribed
leafspine allreduce.
