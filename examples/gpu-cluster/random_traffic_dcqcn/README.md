# Random-traffic throughput vs injection rate (8 So topologies, DCQCN)

Compares the 8 scale-out (`--interNodeTopology`) topologies under
uniform-random cross-node traffic, sweeping the per-GPU injection rate and
plotting delivered aggregate throughput, with DCQCN enabled.

## Setup

- 8 nodes x 4 GPUs (32 ranks), RoCE 400 Gbps So links, 1 us link latency.
- `--randTraffic` mode: every rank injects fixed-interval 64 KiB messages to
  uniform-random ranks on OTHER nodes at `--randInjectionRate` x line rate;
  offered/delivered bytes are counted inside a 800 us measurement window
  after a 200 us warmup (fixed-duration run, `RngRun=1` reproducible).
- `--dcqcn` on: EthernetSwitch marks the SoHeader ECN bit past the VOQ
  threshold, receivers echo CNPs, senders back off via DcqcnRateMachine.

## Run

```bash
./ns3 build                       # from the repo root
python3 run_sweep.py              # 88 runs (8 topologies x 11 rates) -> results.csv
python3 plot_throughput.py        # -> throughput_vs_injection.{pdf,png}
```

## Reading the figure

- **fullmesh** is linear to full line rate: every node pair has a dedicated
  link, so uniform-random traffic never contends (and there is no switch,
  so ECN never marks).
- **host / tor / rail / leafspine** (switched, one NIC per GPU) track the
  offered load until ~0.7, then bend to ~75% accepted: the limit is
  destination collisions (several sources picking the same destination GPU
  serialize at its 400G NIC), the classic uniform-random saturation of
  non-blocking fabrics. These are the topologies where DCQCN is active:
  ECN marks and CNPs rise sharply past rate 0.6 (panel b).
- **ring / torus2d / dragonfly** (GPU-0-gateway shapes) saturate early at
  the gateway funnel: all cross-node traffic of a node squeezes through
  GPU 0's inter-node links (ring ~2.2 Tbps, dragonfly ~3.8, torus2d ~4.5,
  ordered by gateway path diversity). Their So path has no EthernetSwitch
  (direct GPU0-GPU0 links; intra-node forwarding is an NVSwitch), so ECN
  never marks and DCQCN is structurally inert there -- the saturation shows
  up as endpoint/VOQ queueing instead. Ring's slight post-saturation
  decline is multi-hop forwarding overhead growing with load.
- DCQCN in a lossless fabric does not raise saturated throughput (the
  bottleneck port stays busy either way); it drains switch VOQ backlog by
  moving the queueing to the senders. Its activity is therefore shown as
  the ECN-mark signal in panel (b), not as a throughput delta.

Smoke gate: `python3 test/parity/test_random_traffic.py` (run directly)
pins the mode's invariants: fixed-duration completion, non-blocking tor
delivers ~offered, and a 4:1 oversubscribed leafspine lights the full
ECN -> CNP -> DCQCN chain.
