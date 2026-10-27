# PFC alignment: ProtoBridge vs ns3-rdma

This note documents a head-to-head comparison of PFC behavior between
ProtoBridge's switch PFC (modes 1/3, per-VC thresholds) and ns3-rdma
(Yibo Zhu's HPCC-era RoCEv2 simulator, ns-3.17 base, `qbb-net-device` +
`BroadcomNode`/`SwitchMmu`), on a matched 2:1 incast workload.

It records what was measured, what matches, and what differs structurally.
It is a working note, not a spec — the authoritative description of our PFC
semantics remains `doc/CONFIG_GUIDE.md`.

## Matched workload

Two senders → one receiver (2:1 incast):

- **ns3-rdma**: `mix/topology.txt`, `flow_small.txt` (2 flows × 50,000
  packets × 1000 B), 40 Gbps links, 1 µs latency, PAUSE_TIME 5 µs, QCN off.
  Flows start at t = 1.0 s; FCT measured as the time the seq-49999 packet
  of each flow is enqueued at the receiver.
- **ProtoBridge**: `--topology=switched --collective=incast --numGpus=3
  --numNodes=3 --gpusPerNode=1 --interNodeTopology=host
  --interNodeBandwidth=40 --interNodeLatency=1000 --dataSize=50000000
  --bulkChunkSize=1000 --switchPfcMode=<mode> --pfcPauseThreshold=20
  --pfcPauseTime=5 --pfcResumeOffset=8` (mode 1 omits the pause time).
  `--gpusPerNode=1 --interNodeTopology=host` are REQUIRED: the defaults
  (8 GPUs/node, inter-node fullmesh) build a 24-rank fullmesh fabric with
  no leaf switch, where PFC is inert. Same 2× 50 MB of payload, same
  40 Gbps, same 1 µs links.

Both sides carry PAUSE instrumentation: ns3-rdma via `PAUSE_SENT`/
`RESUME_SENT` prints added to `qbb-net-device.cc` `CheckQueueFull()`;
ProtoBridge via the `pfcPausesSent`/`pfcResumesSent` RESULT counters.

Note that the two stacks PAUSE on opposite ends of the congested link:
ns3-rdma's switch PAUSEs the two *ingress* (sender-side) NICs based on
ingress-PG occupancy; ProtoBridge's switch PAUSEs traffic per
(output-port, destination) as it contends for the egress port. Counts are
therefore compared per ingress/port, not as a global equivalence.

## Results

| Scenario | FCT | PAUSEs | RESUMEs | Drops |
|---|---|---|---|---|
| ns3-rdma, static thr | 20.29 ms | 27,035 | 2,254 | 0 |
| ns3-rdma, dynamic thr (α=16) | 19.51 ms | 24,936 | 2,078 | 0 |
| ns3-rdma, PFC off (control) | 20.29 ms | 0 | 0 | 0 |
| ns3-rdma, DCQCN on (control) | ~20.3 ms | 0 | 0 | 0 |
| PB mode 1 (explicit RESUME) | 42.95 ms | 4 | 36 | 0 |
| PB mode 3 (quanta 5 µs) | 42.95 ms | 51,518 | 4 | 0 |
| PB mode 1, 9000-B chunks | 23.49 ms | 4 | 36 | 0 |
| PB mode 3, 9000-B chunks, | 23.49 ms | 27,821 | 4 | 0 |
| PB, PFC off (control) | 42.95 ms | — | — | 0 |

(PB FCT is `simTimeNs` for the same 2×50 MB incast. "PFC off" on the
ns3-rdma side raises the SwitchMmu ingress limits coherently — PG shared
limit, headroom, SP limit, and max buffer all to 400 MB — so no XOFF is
emitted and no admission path overflows. An earlier control that raised
ONLY the PG shared limit overflowed the ingress headroom: 2.46 M drops
and endless retransmission, which briefly produced a bogus "PFC costs 9%
FCT" reading; the coherent control shows FCT is PFC-INVARIANT on ns3-rdma
at this workload, same as ProtoBridge.)

### FCT

- At matched 1000-B packets, ProtoBridge's FCT is 2.1× ns3-rdma's. **This
  is not a PFC difference**: ProtoBridge's FCT is *identical* with PFC on
  or off (42,951,160 ns in both runs, and identical again at 9000-B
  chunks in both modes). The gap tracks the known per-packet processing
  overhead of the chunked-transfer path — the p2p control shows 2.15×
  over theoretical transfer time at 1000-B chunks vs 1.17× at 9000-B
  chunks. At 9000-B chunks the ProtoBridge FCT is 23.49 ms vs ns3-rdma's
  20.3 ms, a 1.16× residual that matches the p2p overhead factor almost
  exactly.
- On the ns3-rdma side, FCT is also PFC-invariant (20.29 ms with the
  static threshold, 20.29 ms with PFC off) — the 9 MB switch buffer
  absorbs the incast with 0 drops either way, so PFC's pause parking
  costs no transfer time at this workload on either simulator. (The
  dynamic threshold runs marginally faster, 19.51 ms, because its
  admission accounting admits slightly more in flight — a threshold
  effect, not a PFC effect.)

### PAUSE/RESUME counts

- ns3-rdma emits ~27k PAUSEs and ~2.2k RESUMEs over a ~20 ms transfer —
  roughly 1,332 pauses/ms summed over 2 paused ingresses (≈666 per
  ingress), and about one RESUME per 12 PAUSEs. With the dynamic
  threshold the counts drop ~8% (24.9k/2.1k) and FCT improves ~4%.
- ProtoBridge mode 3 emits a comparable pause *cadence* at matched packet
  size (51.5k pauses over 43 ms ≈ 1,198/ms summed over 2 paused ingresses,
  vs 1,332/ms on ns3-rdma — same order of magnitude) but almost never
  lifts the pause: 4 RESUMEs total, because the mode-3 re-check at
  `pausetime/2` re-asserts PAUSE whenever the output queue still exceeds
  the threshold, and under sustained 2:1 incast it never drains below it
  mid-run.
- ProtoBridge mode 1 pauses only 4 times and resumes 36 times: the
  hysteresis pair (threshold 20 / offset 8) trips once per congestion
  episode and is lifted as soon as the queue drains past the resume
  point. Mode 1 is thus far quieter on the control plane while achieving
  the same FCT and 0 drops.

### Structural differences (by design, not defects)

1. **Where the pause is asserted.** ns3-rdma is hop-by-hop 802.1Qbb: the
   switch tracks ingress priority-group occupancy (SwitchMmu admission
   accounting, static `m_pg_shared_limit_cell` = 20,600 B or dynamic
   α = 16) and PAUSEs the upstream NIC. ProtoBridge's switch PFC pauses
   per (output port, destination) — the congestion point — and (mode 1)
   tracks explicit RESUME, or (mode 3) carries the pause time in the
   PFC frame and lets the endpoint auto-expire, mirroring qbb's
   `PauseFinish` timer.
2. **Re-check cadence.** qbb re-runs `CheckQueueFull` every
   `pausetime/2` per paused queue and PAUSEs again while congested;
   ProtoBridge mode 3 mirrors this (`RecheckPfcPause` at
   `pausetime/2`), which is why its re-assertion count is high under
   sustained incast. Mode 1 has no timer — RESUME is explicit.
3. **Buffer accounting.** ns3-rdma pauses at ~20 KB ingress PG occupancy
   (small — tuned for 1000-B packets); ProtoBridge's threshold is in
   chunks (default comparisons in this experiment: 20 chunks = 20 KB at
   1000-B chunks). Both produce zero drops at 2:1 incast.
4. **Interaction with rate control.** With DCQCN enabled, ns3-rdma emits
   zero PAUSEs — the rate controller keeps ingress occupancy under the
   threshold. ProtoBridge has no DCQCN arm; its PFC is the only
   congestion backpressure, and (as the controls show) it does not
   change the transfer time.

## Reproduction

- ns3-rdma: build per the patches in `/tmp/ns3rdma-build` (python2 waf
  1.7.11; PAUSE instrumentation in `qbb-net-device.cc`), then run
  `build/scratch/third mix/config_static_small.txt` (and
  `config_dynamic_small.txt`, `config_nopfc.txt`, `config_qcn.txt`) from
  `/tmp/pfc_align/ns3rdma`.
- ProtoBridge:
  `./build/scratch/ns3.47-gpu-cluster-sim-debug --topology=switched
  --collective=incast --numNodes=3 --interNodeBandwidth=40
  --interNodeLatency=1000 --dataSize=50000000 --bulkChunkSize=1000
  --switchPfcMode=3 --pfcPauseThreshold=20 --pfcPauseTime=5
  --pfcResumeOffset=8` and the mode-1 / 9000-B variants.
