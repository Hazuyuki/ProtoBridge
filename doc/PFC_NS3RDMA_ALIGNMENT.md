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
  --numNodes=3 --gpusPerNode=1 --interNodeTopology=tor
  --interNodeBandwidth=40 --interNodeLatency=1000 --dataSize=50000000
  --bulkChunkSize=1000 --switchPfcMode=<mode> --pfcPauseThreshold=20
  --pfcPauseTime=5 --pfcResumeOffset=8 --soTxWindow=64` (mode 1 omits the
  pause time). `--gpusPerNode=1 --interNodeTopology=tor` are REQUIRED: the
  defaults (8 GPUs/node, inter-node fullmesh) build a 24-rank fullmesh
  fabric with no leaf switch, where PFC is inert; `tor` is the
  shared-switch star (the ns3-rdma structural twin), `host` adds one
  switch per node (results for both in the table). `--soTxWindow=64` is
  defaults (8 GPUs/node, inter-node fullmesh) build a 24-rank fullmesh
  fabric with no leaf switch, where PFC is inert. `--soTxWindow=64` is
  REQUIRED at 1000-B chunks: it bounds the in-flight bulk chunks (the
  perftest tx_depth analog) so the senders' injection is ACK-clocked.
  Without it the unbounded window dumps the whole transfer into the
  network before the first PAUSE can propagate, and the receiver's VOQ
  hits the 10,000-chunk hard cap → silent drops → (lossless RoCE, no
  retransmit) an incomplete transfer pinned at the 3600-s ceiling. The
  9000-B rows below ran without it (their BDP fits the cap) and are
  byte-identical with or without it. Same 2× 50 MB of payload, same
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
| PB mode 1 (explicit RESUME), tor | 21.90 ms | 1,584 | 14,253 | 0 |
| PB mode 1 (explicit RESUME), host | 23.60 ms | 1,548 | 13,932 | 0 |
| PB mode 3 (quanta 5 µs), tor | 22.35 ms | 13,932 | 13,928 | 0 |
| PB mode 3 (quanta 5 µs), host | 24.38 ms | 13,932 | 13,932 | 0 |
| PB, PFC off (control), tor/host | 21.90 ms | — | — | 0 |
| PB mode 1, 9000-B chunks, tor | 21.15 ms | 2 | 18 | 0 |
| PB mode 1, 9000-B chunks, host | 21.15 ms | 182 | 1,638 | 0 |
| PB mode 3, 9000-B chunks, tor/host | 21.15 ms | 16,871 | 18 | 0 |
| PB, PFC off, 9000-B chunks, tor/host | 21.15 ms | — | — | 0 |

All PB 1000-B rows use `--soTxWindow=64`; the 9000-B rows are identical
with or without it (21.153216 ms in every run), so the window is a no-op
for a BDP-sized transfer that never approaches the VOQ cap. The `tor`
rows use the shared-switch topology (the structural twin of the ns3-rdma
star, and the topology used for the scale-out alignment); the previously
published `host` rows are retained for reference — `host` gives each
node its own switch (one extra hop), which adds PFC-drain time at
1000-B chunks (mode 1: 23.60 vs 21.90 ms) and slightly different pause
counts, but is byte-identical at 9000-B chunks where queues stay shallow.

(PB FCT is `simTimeNs` for the same 2×50 MB incast. "PFC off" on the
ns3-rdma side raises the SwitchMmu ingress limits coherently — PG shared
limit, headroom, SP limit, and max buffer all to 400 MB — so no XOFF is
emitted and no admission path overflows. An earlier control that raised
ONLY the PG shared limit overflowed the ingress headroom: 2.46 M drops
and endless retransmission, which briefly produced a bogus "PFC costs 9%
FCT" reading; the coherent control shows FCT is PFC-INVARIANT on ns3-rdma
at this workload, same as ProtoBridge.)

### FCT

- At matched 1000-B packets, ProtoBridge's tor FCT (21.90 ms, PFC off) is
  1.08× ns3-rdma's 20.29 ms. **This is not a PFC difference**:
  on the shared-switch `tor` topology the FCT is near PFC-invariant
  (21.90 ms off → 21.90 ms mode 1 → 22.35 ms mode 3); the `host`
  topology's extra hop makes PFC-variant behavior visible (21.90 →
  23.60 mode 1 → 24.38 mode 3): the ACK-clocked window (soTxWindow=64)
  paces injection, and each pause/resume episode parks the window,
  adding a bounded restart cost — the price of backpressure actually
  engaging, visible now that the unbounded window no longer masks it.
  Before the cut-through accounting fix, the serial pipeline charge
  derated first-hop egress by ~2× at 1000-B chunks, which accidentally
  rate-limited the senders below the receiver's drain rate; FCT was
  42.95 ms (2.1× ns3-rdma) and PFC-invariant because congestion never
  truly formed. At 9000-B chunks the FCT is 21.15 ms vs ns3-rdma's
  20.3 ms (1.04×), PFC-invariant — the larger serialization per chunk
  keeps the queue shallow and pause episodes are rare.
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
- ProtoBridge mode 3 at 1000-B chunks emits 13,932 PAUSEs and 13,932
  RESUMEs over 24.4 ms — 571/ms summed over 2 paused ingresses, roughly
  one PAUSE/RESUME pair per quanta window per ingress: the ACK-clocked
  window drains the VOQ past the resume offset within each 5-µs quanta,
  so the pause lifts and re-asserts continuously instead of latching.
  (Pre-window, mode 3 latched: 51.5k pauses / 4 resumes over 43 ms —
  the unbounded sender kept the VOQ pinned above threshold so the
  re-check never lifted.) At 9000-B chunks the old latching pattern
  persists (16,871 / 18) because 9000-B backlogs drain slowly relative
  to the quanta.
- ProtoBridge mode 1 at 1000-B chunks: 1,548 pauses / 13,932 resumes —
  the hysteresis pair (threshold 20 / offset 8) trips per congestion
  episode and lifts as soon as the window-paced queue drains past the
  resume point, so the control plane stays ~10× quieter than mode 3
  while achieving the same 0 drops.

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
   threshold. These ProtoBridge runs mount no DCQCN arm (`--dcqcn` unset;
   the RoCE rate machine exists — `--dcqcn=1` mounts `DcqcnRateMachine`
   and the SoHeader/ECN/CNP chain — but is deliberately off here to
   isolate PFC behavior); PFC is then the only congestion backpressure,
   and (as the controls show) it does not change the transfer time.
5. **XOFF in-flight absorption (headroom).** ns3-rdma's SwitchMmu keeps
   a dedicated ingress headroom (`m_pg_hdrm_limit` = 100 cells = 103 KB
   in the mix config) so the data still in flight when the XOFF leaves
   the switch — link RTT plus the source's already-serialized burst —
   has somewhere to land under PFC's no-drop contract (credit-based
   fabrics never oversend; lossy fabrics drop instead). ProtoBridge's
   original semantics admitted unbounded toward the VOQ hard cap while
   a hop was paused, which is behaviorally an infinite headroom.
   `--pfcHeadroomChunks` (EthernetSwitch attribute
   `PfcHeadroomChunks`, default 0 = legacy unbounded) now models the
   budget explicitly: while a hop is PAUSE-latched, each post-XOFF data
   admission draws the per-hop budget; exhaustion drops with a NACK
   (the "ingress headroom full" analog) and the LLR/SACK path
   retransmits — with a finite headroom the sim forces `llrEnabled=1`
   exactly like the lossy tail-drop regime, because a silent drop in a
   lossless fabric is unrecoverable. The VOQ hard cap
   (`--switchVoqDepth`) drops with the same NACK (round 35, finding 1):
   with a finite headroom and a small hard cap the cap binds before the
   budget exhausts, and a silent drop there stalls the transfer — every
   hard-cap data drop now NACKs back and retransmits via LLR/SACK too.
   Verified on the matched workload:
   headroom ≥ 12 chunks is FCT-identical to the legacy (21.899-ms
   PFC-off / 24.378-ms mode-3 values, byte-identical counters) — the
   budget simply never exhausts at this BDP, which reproduces ns3-rdma's
   PFC-invariance; headroom 10 chunks exhausts (99k drops) and completes
   only via 536k LLR retransmissions (115.3 ms) — the drop+retry cost
   of an undersized headroom. This closes the mechanism inventory: the
   earlier "PB lacks headroom" hypothesis for the mode-3 residual is
   disproven (the residual is quanta quantization, threshold vs quanta
   sizing), and the knob's value is mechanism isomorphism with
   `m_pg_hdrm_limit`, not FCT.

## Reproduction

- ns3-rdma: build per the patches in `/tmp/ns3rdma-build` (python2 waf
  1.7.11; PAUSE instrumentation in `qbb-net-device.cc`), then run
  `build/scratch/third mix/config_static_small.txt` (and
  `config_dynamic_small.txt`, `config_nopfc.txt`, `config_qcn.txt`) from
  `/tmp/pfc_align/ns3rdma`.
- ProtoBridge (base 1000-B reproduction; the single-switch `tor`
  inter-node topology with one GPU per node is the structural twin of the
  ns3-rdma star — `tor` places all nodes on one shared switch, while
  `host` creates one switch per node and adds a hop, so `host` FCTs run
  ~8% higher at equal settings):
  `./build/scratch/ns3.47-gpu-cluster-sim-debug --topology=switched
  --collective=incast --numNodes=3 --gpusPerNode=1 --interNodeTopology=tor
  --interNodeBandwidth=40 --interNodeLatency=1000 --dataSize=50000000
  --bulkChunkSize=1000 --soTxWindow=64 --switchPfcMode=3
  --pfcPauseThreshold=20 --pfcPauseTime=5 --pfcResumeOffset=8`
  and the mode-1 / 9000-B variants. The headroom control adds
  `--pfcHeadroomChunks=<N>` (0 = legacy, ≥12 = ns3-rdma-like
  never-exhausts at this BDP, 10 = undersized: drop + LLR retransmit
  recovery).
