# ProtoBridge module change history

ProtoBridge bundles a self-authored `src/gpu-cluster/` module (OTP/PEX packet
execution + NVSwitch + collective injectors + four-tier FEC/retry), a
`surrogate/` latency model, and `configs/` profiles on top of the upstream
ns-3.47 framework whose change history follows after the separator. Only
ProtoBridge-visible changes are listed here.

### Fixed — h200-nvls-allgather.cfg run recipe left the GPU↔NVSwitch trunk 18× under-provisioned

The cfg's documented run command passed no `--linkDataRate`, so the switched
builder fell back to the profile's `bandwidthGbps=170` — a PER-LANE value
(375 GB/s aggregate ÷ 18 lanes; correct for ring/fullmesh, whose builders
instantiate 18 physical lanes) — for the ONE link the switched topology
gives each GPU. The NVSwitch's full-D multicast egress then serialized on a
single 170 Gbps link (16 MB allgather: 918 µs observed ≈ the 790 µs
single-link bound), the root cause of the ≈0.09–0.16× surrogate/ns-3 ratio
in the consistency regression. Header now recommends and the run example
passes `--linkDataRate=3060` (18×170 = the full trunk, the tree-calibration
convention). Comments only — config-vs-inline parity re-verified
bit-identical.

### Changed — surrogate tree preset recalibrated to the post-volume-fix ns-3 tree; 4-preset consistency regression added

The theory-surrogate tree constants were fitted against the pre-#37
under-transferring TreeAllReduce and undershot the rewritten injector by up
to ~10x at 256 MB. Recalibrated against a 27-point ns-3 sweep
(`configs/calibration/tree_ns3_sweep.py`, N∈{4,8,16} × 9 sizes,
`--linkDataRate=3060` = the full 18×170 Gbps GPU↔NVSwitch trunk): the old
serial term `D(N−1)/N/(log₂N·tree_bw)` *decreases* in N while measured
large-size cost grows (α = 2.505/3.894/3.993 trunk-loads at N=4/8/16), so
the formula changed, not just constants — per-step volume is now
`α(N)·D/steps` with the structural trunk-load `α(N)=1/2/3` (N≤2/N≤4/N≥5,
busiest GPU↔switch trunk of the heap-shaped chunk-pipelined tree), applied
in `_per_step` so predict/predict_tail/BER-packet paths all inherit it.
Fitted `tree_bw_per_level=270653 B/µs` (η=0.7076 vs the 382500 B/µs trunk)
and `t_min_switch=2.99 µs` (relative-error-weighted LS; MAPE 7.59% all /
5.02% SIMPLE). `make_surrogate_from_wire` tree derivation is now
`eta_tree·B_agg` with default `eta_tree=0.60` (270000, −0.24% vs fit).
`h200-tree-allreduce.cfg` header now recommends `--linkDataRate=3060` and
retires the pre-fix 80/100/50 Gbps guidance. Ring/nvls/fullmesh presets and
all from-wire non-tree numbers are byte-identical; optical/connectx7 tree
constants are untouched historical placeholders. New regression
`configs/calibration/surrogate_ns3_consistency.py` records all four presets
vs current ns-3 at N=8: ring MAPE 1.87%, tree 7.39%, fullmesh 0.12%; nvls
is convention-dependent (0.09–0.16× vs the cfg's single-170G-link default,
1.07–1.17× vs the full trunk — the historical "≈0.566× lower bound" note's
conditions were unrecorded and match neither; documented in
`doc/CALIBRATION.md`). 3 new unit tests (α values, ns-3 ground-truth
reproduction within 12%, monotonicity in N); 67 pytest green; config-vs-
inline parity unchanged.

### Fixed — RingBroadcast/RingReduce moved less than D per edge; rewritten as chunk-pipelined full-volume chains

The two rooted ring collectives beat the physical lower bound D/BW (NCCL
busbw factor for broadcast/reduce is 1: every ring edge must carry the full
D). Old RingBroadcast relayed a single D/N segment sequentially — each
non-root GPU received only 1/N of the array (wrong semantics on top of the
volume error); old RingReduce ran N−1 parallel steps of D/N per link
((N−1)/N·D per edge — the *reduce-scatter* volume, but the root needs the
whole result, and completion checked only the root's step counter). Both
rewritten in the TreeAllReduce (#37) style: chunk-pipelined chains carrying
full D per edge (`clamp(D/(32·(N−1)), 64 KiB, 8 MiB)` forwarding quanta,
byte-counted order-insensitive progress, uint64 `SendBulkWireTransferSize`).
Broadcast: root streams D around the ring, last-before-root does not
forward, done when all N−1 non-roots hold D. Reduce: the chain head (root's
ring predecessor) streams D toward the root; interiors forward the running
partial sum gated on received bytes; done when the root holds D. Post-fix
64 MB times sit just above the D/BW bound and are nearly N-independent
(n=4 751.6 µs / n=8 757.6 µs vs old 569.8/655.7 = (N−1)/N·bound); small
sizes get slightly *faster* (old paid full store-and-forward latency per
hop — two errors partially cancelled). Neither collective is in any
calibration or parity path; all pins unchanged, all gates green.

### Fixed — 2D AlltoAll relay keyed first-hop accounting on the wrong rank (latent)

`AlltoAllInjector::OnPacketReceived` keyed the 2D-mode (UB-Mesh
`--topology=2dfullmesh`) first-hop relay counter on the receive callback's
first argument — which is the *receiving* endpoint's own rank
(`FabricEndpoint` passes `m_rank`), not the sender. Every same-row sender's
bytes toward the same final destination pooled into one counter, and the
threshold-crossing `erase()` would discard overshoot bytes. The bug is
latent today: bulk-transfer deliveries arrive atomically (one callback per
completed transfer with `effectiveDataSize` = the full chunk), so pooled
crossings landed exactly on chunk boundaries and nothing was lost — verified
byte-identical pre/post fix on 2×2/3×3/4×4 grids at 4–64 MB including
forced fragmentation (`--sprayChunkSize=8192`). Any future fragmented
delivery (e.g. partial spans under loss) would have deadlocked the relay.
Now keyed on `header.GetSourceRank()` (same trap as the TreeAllReduce fix
below). All parity pins and gates unchanged.

### Fixed — TreeAllReduce carried only D/N per edge; rewritten as full-volume chunk-pipelined tree

The Demystifying-NCCL anchor (below) exposed a structural bug: the old
`TreeAllReduce` sent `m_chunkSize = dataSize/numGpus` per tree hop per step,
so the total per-edge volume was D/N instead of D — the sim's effective
per-edge bandwidth (193.7 GB/s) exceeded the 150 GB/s GH200 pair spec and the
sim beat the physical bound D/BW = 28.6 ms (22.2 ms at 2^32). The old
schedule also never sent GPU 3's reduce contribution at N=4
(`targetLevel = depth−1−k` skipped level 2). The model is rewritten as a
chunk-pipelined binary tree (heap arithmetic parent=(g−1)/2 kept): reduce
flow sends the full D up every edge (interior GPUs forward
`[sent, min-over-children-received)`), broadcast flow re-sends the full D
down, forwarding quantized to `clamp(D/(32·depth), 64 KiB, 8 MiB)` uniform
chunks (no per-hop re-fragmentation cascade), progress byte-counted per
(flow, edge) so spray reorder and GBN duplicates cannot regress it; sw
overhead knobs pace each hop (perStep at first chunk + perByte rate tax,
defaults 0 = zero extra events). Public API unchanged. Demystify tree arm
post-fix: MAPE LL 16.9→5.8%, LL128 15.2→8.1%, Simple 26.4→16.3%; 2^32 Simple
28.84 ms = 1.007× physical bound (was impossibly under it); 2^32 protocol
ratios within 1% of measured. Ring arm byte-identical; all parity pins
(RoCE triple, ETH, strict-GBN, config-vs-inline incl. the tree case)
unchanged. Note: `h200-tree-allreduce.cfg` per-protocol `linkDataRate`
80/100/50 was fitted against the old under-transferring model and needs
recalibration before its absolute numbers are reused.

### New — Demystifying-NCCL Fig. 6 anchor: Su protocol constants validated on third-party GH200 hardware

Third external measured anchor, first for the **Su (NVLink) protocol axis**:
*Demystifying NCCL* (arXiv:2507.04786) Fig. 6 intra-node panels — LL / LL128 /
Simple × Ring/Tree AllReduce on a 4× GH200 quad (CSCS Alps, NCCL 2.19.1,
2^14–2^32 B) — digitized by pixel extraction
(`configs/calibration/demystify_nccl_fig6_reference.csv`, provenance in
header) and compared against the stock `NcclProtocolModel` with only the
physical spec changed (ring: `--numLanes=18 --bandwidth=200` = 450 GB/s
per-GPU multi-ring aggregate; tree: fullmesh `--numLanes=6` = one 150 GB/s
pair per edge). Only 6 startup intercepts fitted (anchored at 2^14);
2^16–2^32 is a pure transfer test: per-curve MAPE 15–26% (one-sided
optimistic, worst in the 2^22–2^26 transition regime), and the
bandwidth-independent protocol-shape ratios at 2^32 transfer to within 3% on
the tree path (tree LL/Simple 1.95 vs 2.01 measured; LL128/Simple 1.04 vs
1.07) — independent confirmation of the LL=0.5 / LL128=0.9375 wire model.
Ring LL undershoots 21% (our LL efficiency is NCCL's theoretical bound; the
paper reports LL at 25–50% of peak) — documented, not tuned. Sweep:
`configs/calibration/demystify_nccl_sweep.py` (60 runs) + comparison figure;
write-up in `doc/CALIBRATION.md` §"NCCL protocol shapes (scale-up)". No
model-code changes — measurement-only round.

### New — `--soGbnStrict` strict So go-back-N receive semantics (supersedes the GBN numbers below)

The re-run below left the GBN arm optimistic at high loss (0.820 vs CX-6
0.467 at 1%, arms inverting at 2%): the receive path kept the bypass-mode
dedup, so out-of-order originals were accepted (and re-ACKed) and blanket
retransmits arrived as cheap duplicates. New `--soGbnStrict` (default 0 =
legacy, So-only — the gate requires a remote source rank, so the sprayed Su
NVLink fabric where OOO is normal is never touched) switches `llrMode=gobackn`
So receive to real IB responder behavior: in-order-only acceptance,
out-of-order arrivals discarded with no ACK, exactly one NAK per gap (lost
NAKs covered by the 500 µs retry-timer backstop), duplicates re-ACKed only,
and the drop site no longer emits the oracle per-drop NACK (which also kills
the R = window × plr blanket cascade that made large windows explode).
Under strict semantics the TX window IS the rewind span; the GBN arm uses
w=64, a calibrated back-fit against the CX-6 anchor within the physical
bounds [wire BDP = 16, testbed tx_depth = 128] (CX-6 reverse-solves to a
per-drop waste of 70–130 packets). SRNIC Fig. 11 sweep re-run (3-seed):
strict GBN tracks CX-6 within ~3 points up to 0.5% loss (0.919/0.933 at
0.1%, 0.725/0.744 at 0.5%), stays ~10 points optimistic at 1–2% (no
RTO-scale stalls modeled: 0.580/0.467, 0.383/0.278), and restores the
measured mode ordering at every loss rate (GBN below SACK — the legacy arms
inverted at 2%). SACK arm unchanged (w=16, within ~5 points everywhere).
Gates: build clean, C++ EXTENSIVE, surrogate 64, all 5 parity suites, new
`test/parity/test_so_gbn_strict.py` (default-off legacy pin 3185174 ns +
strict-on lossless baseline 2521509 ns + strict-on lossy pin 4276850 ns).

### New — `--soTxWindow` ACK-clocked TX window; SRNIC Fig. 11 re-run (supersedes the entry below)

Aligning against SRNIC Fig. 11 exposed three model bugs, all fixed and inert
at defaults (parity-pinned byte-identical):

1. **No TX-depth/BDP window**: `FabricEndpoint::SendBulkWireTransferSize`
   dispatched every chunk synchronously at t≈0, making the effective GBN
   window the whole remaining message. New `--soTxWindow=N` (default 0 =
   legacy): bounds in-flight So bulk chunks per endpoint, clocked by the
   per-packet LLR ACKs (requires `--llrEnabled`); the NIC send-queue /
   perftest `tx_depth` analog. Window slots release on ACK or permanent
   loss; duplicate ACKs from GBN blankets don't over-release.
2. **Retry timers armed at dispatch, not TX**: with the synchronous dispatch,
   LLR retry timers all started at t≈0, forcing whole-transfer-scale
   timeouts (3/20 ms). With the window, `StorePacket` happens at
   window-gated TX time, so RTT-scale timeouts (500 µs ≈ 100×RTT) are sound.
3. **NACK-of-NACK**: the endpoint receive path emitted a NACK for *control*
   packets dropped by `--packetLossRate` (loss sample ran before the
   protocol-control check). Control packets now drop silently, matching the
   switch path; lost control packets recover via the sender's retry timer.

Re-run results (3-seed, `--soTxWindow=16` = wire BDP, 500 µs timeout, both
arms): the SACK arm now tracks measured SRNIC within ~5 points at every loss
rate (0.823 vs 0.773 at 1%, 0.569 vs 0.619 at 2%); the GBN arm has the
correct low-loss shape (0.977 vs 0.933 at 0.1%) but is optimistic at high
loss (0.820 vs 0.467 at 1%) because the receiver keeps out-of-order
originals (no true-GBN discard) and there are no RTO-scale stalls — see
`doc/CALIBRATION.md` for the honest gap analysis and the blanket-cascade
subcriticality rule (R = window × plr < 1). The numbers and the
"whole-message GBN window" caveats in the entry below are superseded.

### New — SRNIC Fig. 11 loss-recovery comparison + `--llrTimeoutRetransmit` backstop

External measured anchor for the LLR recovery modes: SRNIC (NSDI'23) Figure 11
measures a ConnectX-6 (go-back-N) collapsing under FPGA-injected random loss
while SRNIC's selective repeat degrades gently. The ns-3 counterpart sweeps
`llrMode=gobackn` vs `llrMode=sack` (only that axis changes between arms) on
the RoCE tor 100G fabric with `--packetLossRate` — the recovery-mode spread
reproduces, and at 1% loss the GBN arm (0.510 normalized) sits within 5 points
of the measured CX-6 collapse (0.467). Digitized reference with provenance in
`configs/calibration/srnic_fig11_cx6_reference.csv`, sweep driver
`configs/calibration/eth_gbn_loss_sweep.py`, arms
`configs/protocol_configs/p2p-roce-lossy{,-sack}.cfg`, full write-up (incl. the
whole-message-GBN-window structural caveats) in `doc/CALIBRATION.md`.

The experiment exposed a production model gap: `LlrManager::OnRetryTimeout`
only invoked a callback that unit tests registered — the retry timeout was a
no-op in real runs, so a lost NACK deadlocked SACK mode forever (NACK emission
at receiver drop time was the only recovery trigger, and `--packetLossRate`
applies to control packets). New opt-in `--llrTimeoutRetransmit` (default 0 =
legacy no-op, parity-pinned byte-identical) wires `FabricEndpoint` to the
timeout: on fire it re-issues the retransmission through the mode's own path
(SACK single-seq / GBN blanket), re-arming timers and honoring
`--llrRetryLimit`.

### New — RTTCC (RTT-probe-based congestion control) for the plain-Ethernet So fabric

`--rttcc` (default off) adds a second congestion-control option beside DCQCN,
targeted at the **lossy plain-Ethernet fabric where DCQCN cannot operate**
(ETH has no ECN/CNP loop, and `EthernetProtocolModel` locks
`WireEfficiency=1.0`, which the DCQCN So gate keys on). RTTCC senses
congestion from delay instead (NVIDIA RTT-CC / Swift-style):

* **Probe/response loop.** At the `SendPacketOnDevice` egress chokepoint the
  sender emits at most one `RTT_PROBE` per `ProbeInterval` per flow alongside
  the data; the receiver reflects it as an `RTT_RESPONSE` echoing only the
  probe sequence (timestamps never go on the wire — the sender keeps a local
  seq→sentNs map). Both new packet types are CONTROL packets: they bypass BER
  sampling, VOQ tail-drop, ECN marking, LLR storage and both rate machines'
  shaping, but are still ENQUEUED into switch VOQs — so the measured RTT
  reflects real congestion queueing.
* **Swift-style rate law** (`RttccRateMachine`, one per So endpoint):
  `rtt<=TargetRtt` → rate += `AdditiveIncreaseGbps` (cap `LineRateGbps`);
  `rtt>TargetRtt` → rate ×= `1 − Beta·(rtt−target)/rtt` (floor
  `MinRateGbps`). The rate is enforced as a TRUE per-flow pacer (per-flow
  next-send bookkeeping, same mechanism as the DCQCN per-QP ceiling) and
  COMPOSES with DCQCN via `max(dcqcnDelay, rttccDelay)`.
* **Per-endpoint mount** (unlike DCQCN's shared machine): the flow key
  carries no source rank, so a shared machine would merge the pacer/probe
  state of two incast senders targeting the same (dest, vc, flow).
* **Parity.** Default off mounts nothing; the incast baseline
  (simTimeNs 68664 / drops 87 / retx 285 / permLoss 0) and the RoCE triple
  (su 4769616 / 2n 15307388 / 4n 13421212) stay byte-identical. Even when
  mounted, an uncongested flow sits at the line rate and the pacer is a
  0-delay no-op. On the long 4MB `p2p-eth-incast.cfg` run RTTCC reduces
  drops 16388→16317, retransmits 25151→24911 and permanent losses 280→272.
* CLI: `--rttcc`, `--rttccTargetRttNs` (default 10000), `--rttccProbeIntervalNs`
  (default 5000); RESULT block reports `rttccProbesSent/RttSamples/
  MinRttNs/MaxRttNs/MinRateGbps`. Gates: `RttccRateMachineTest` (C++) +
  `test/parity/test_rttcc_ethernet.py` (default-off byte-identity + loop-alive).
* Idealizations (see `doc/RISKS.md`): probes ride the control path so they are
  never tail-dropped (every probe returns a sample); the receiver reflects
  unconditionally (no receiver-side machine).

### New — multi-operation temporal contention modeling (multi-node contention wiring + background P2P injection)

ProtoBridge can now model **contention between a primary collective and a
time-offset background P2P flow sharing the same fabric**, for both scale-up
(single-node) and — newly — scale-out (multi-node) topologies. Two gaps closed:

1. **Multi-node contention wiring.** `ContentionModel` (WFQ over three traffic
   classes COLLECTIVE/MEMORY/P2P) was previously attached only on the
   single-node (Su) path (`topoHelper.SetContentionModel`); multi-node (So)
   endpoints never received one. The multi-node endpoint loop in
   `scratch/gpu-cluster-sim.cc` now creates an **independent** `ContentionModel`
   per endpoint (backlog local to that endpoint's injection side; bandwidth is
   the intra-node NVLink rate `bandwidthGbps`, also for So-bound packets) —
   this DIFFERS from the single-node `GpuClusterTopologyHelper`, whose
   `AttachContentionModelToEndpoints` creates ONE clone and shares it across all
   endpoints so the backlog aggregates node-wide (two layers, not a correctness
   difference; the helper's header comment previously said "cloned for each
   endpoint" and now states the sharing). WFQ penalty fires only at
   `numActive>1`, so pure-collective So traffic stays single-class and
   **byte-identical** (su 4769616 / 2n 15307388 / 4n 13421212 ns re-verified);
   the model engages only when a second class is concurrently active on the
   same endpoint.
2. **Background P2P injection entry.** New `--bgP2p*` CLI injects a self-driven
   `P2pTraffic` flow scheduled at `--bgP2pStartTimeNs` that shares the fabric
   with the primary collective — exercising endpoint WFQ (multi-class) and So
   switch VOQ arbitration. Unlimited (`maxPackets=0`); the sim stops via
   `OnCollectiveComplete` when the collective finishes. `--bgP2pIntervalNs=0`
   (back-to-back) means one packet serialization time at the intra-node
   bandwidth (clamped to ≥1 ns — integer division floors <100 B at 800G to 0,
   which would loop at a fixed sim-time and starve later events); a cross-node
   flow is thereby over-driven by the intra/inter rate ratio (2× at 800G/400G)
   and queues at the NIC — pass `--bgP2pIntervalNs` explicitly to pace at the
   inter-node rate. `bgP2pBytesSent`/`bgP2pPacketsSent` count injection
   (`SendP2p` calls), not delivery. Default off (parity-identical).

**What slows the collective, honestly.** Two mechanisms, which the gate checks
separately because each can regress on its own: (a) **link sharing** — the bg
packets FIFO-serialize on the same NetDevice / So switch VOQ as the collective;
(b) the **WFQ penalty** — an extra send-side delay when >1 class is active.
Smoke (2-node RoCE host, 16 MiB hierarchical allreduce): baseline 1248928 ns →
cross-node bg (rank 0→4) 1353663 ns (+8.4%, 129 × 1 MiB injected). That +8.4%
is **entirely link sharing**: with all three weights set to 1.0 (which makes
`ComputeServiceTime` return exactly 0 while the model stays mounted) the run
is byte-identical at 1353663 — the WFQ term fires (26 packets penalized) but
is off the critical path behind the over-driven 400G NIC queue. An intra-node
bg (rank 0→1) on the same 2-node run does discriminate: 1311031 (default) vs
1295971 (weights=1.0). Single-node Su: 319658 → 450982 ns (+41%), of which the
WFQ penalty on the P2P class is a −3.7% correction (468378 with weights=1.0).
WFQ reshuffles who waits; it is not monotone in latency. `--bgP2p*`:
`bgP2pEnabled`/`bgP2pSrc`/`bgP2pDst`/`bgP2pSizeBytes`/`bgP2pIntervalNs`/
`bgP2pStartTimeNs`. Gated by `test/parity/test_contention_interference.py`
(4 cases): interference `simTimeNs > baseline` + `complete` + injected bytes
for both regimes, a **WFQ discriminator** (default weights `!=` weights=1.0,
intra-node bg in both regimes), and all seven `simTimeNs` values pinned like
the RoCE parity triple. Design note: single-node shares one `ContentionModel`
clone across endpoints (backlog node-wide); multi-node mounts an independent
per-endpoint instance (backlog per endpoint) — different layers, magnitudes
not expected to match.

### Changed — scale-out (So) traffic bypasses credit flow control

`SendBulkWireTransferSize` and `OnPacketDelivered` now gate credit on
`IsLocalRank(destRank)`: scale-out (inter-node, So) packets neither consume nor
return per-packet credits. Previously So endpoints carried a credit structure
that never fired (the default `--vcCredits` depth ≫ any bulk chunk, so
`HasCredits` was always true) — a vestigial shell. So flow control is, and now
honestly is, the lossless/lossy fabric mechanism instead: ECN/DCQCN/per-QP
ceiling (RoCE) and VOQ tail-drop + LLR retransmit (Ethernet). Scale-up (Su,
NVLink) keeps full credit gating — unchanged. Single-node (`m_localRankCount
==1`) keeps all ranks local → unchanged. Su baseline (4769616 ns) and So RoCE
hierarchical parity (15307388 / 13421212 ns) re-verified byte-identical; the
bypass is behavior-equivalent at the old credit depth. `--vcCredits` now
documented as Su-only.

### New — config-primary selection of the scale-out (So) fabric

The config-primary path (`.cfg`/`.profile`) previously sourced only the
scale-**up** (Su, intra-node) fabric axes; the scale-**out** fabric type (RoCE
vs ETH) was CLI-only (`--interNodeFabricType`). The profile→CLI sourcing block
now also sources the four inter-node axes — `interNodeFabricType`,
`interNodeTopology`, `interNodeBandwidthGbps`, `interNodeLatencyNs` — from the
`.cfg`'s `[stack]` section (or the `.profile`), so **one config adapts across
protocols**: `interNodeFabricType = ETH` selects the lossy Ethernet So fabric,
`RoCE` the lossless variant, with no `--interNode*` CLI flag. Keys absent fall
back to the CLI default (backward-compatible — existing profiles unaffected).
`numNodes`/`gpusPerNode`/`dataSize` stay on the CLI (scale axes, like `numGpus`).
Demo `configs/protocol_configs/eth-hierarchical.cfg` (a hierarchical allreduce
over ETH, fully config-driven); bit-identical to the inline CLI path — gated by
the new `test/parity/test_config_vs_inline_eth.py` (2-/4-node, 256 MiB,
`simTimeNs` exactly equal). Closes the config-primary So gap noted in
`test_so_hierarchical_regression.py`.

### New — plain (non-RoCE) lossy Ethernet scale-out fabric (ns-3 + surrogate)

The scale-out (So) fabric was RoCEv2-only (lossless RDMA + DCQCN/PFC/ECN). It
now gains a PLAIN (non-RoCE) lossy Ethernet counterpart — the lossy contrast to
RoCE's losslessness, for a RoCE-vs-Ethernet loss-resilience comparison. Both
layers, on branch `feature/plain-ethernet-fabric`.

**ns-3 (C++):** a new `EthernetProtocolModel`
(`src/gpu-cluster/model/ethernet-protocol-model.{h,cc}`, mirroring
`roce-protocol-model`) — `WireEfficiency=1.0` (locked; plain L2 data carries no
RoCE IP/UDP/IB headers), `GetProtocolId()=0` (NONE — the non-bulk send path
stamps `SetProtocol` unconditionally), store-and-forward NIC driver
(startup 1500 ns / per-step 800 ns / 9 KiB jumbo chunk). The `EthernetSwitch`
(lossy) tail-drops at `VoqDropThreshold` and emits a NACK back to the source
(`NvSwitch::EmitNackBack`, mirroring the BER-NACK path); the sender retransmits
via the LLR/SACK path (`ProcessNackPacket` → `HandleRetryRequest`/
`HandleSackRequest`), with `LlrManager::OnRetryTimeout` as the backstop and
`--llrRetryLimit` bounding the loop (permanent-loss fallback under exhausted
retries = faithful severe-congestion lossy-Ethernet degradation). To wire the
NACK origin, `NvSwitch::OnPacketEnqueue` changed `void→bool` (+`inPort`) — base
returns `true` (no-op) so the RoCE/ECN path is byte-identical — and
`MultiNodeTopologyHelper` gained `SetInterNodeSwitchLlrEnabled` (default `false`
= RoCE parity) so the So switch carries `m_llrEnabled=true` on the ETH path (else
`EmitNackBack` early-returns and the drop is silent → stall). `scratch` lights
the previously-dead `FabricType::ETHERNET` enum + forces LLR on **both** endpoint
and switch for ETH; new `configs/protocol_configs/p2p-eth.cfg` (single P2P) +
`p2p-eth-incast.cfg` (2-flow incast: `--voqDropThreshold=2 --llrRetryLimit=64`
→ 87 drops → 285 retransmits → 0 permanent losses, completes 68.7 µs). 2 new C++
tests (`EthernetProtocolModelTest`, `EthernetSwitchDropTest`); `--ber=0` keeps
the lossy drop the only loss source. RoCE parity byte-identical
(su 4769616 / 2-node 15307388 / 4-node 13421212 ns).

**Surrogate:** `make_h200_ethernet_theory()` — the lossy counterpart to
`make_h200_roce_theory()`: same 400GbE wire (`ring_bw=160000`), store-and-forward
latency (`link_latency_us=3.0` / `t_min_switch_us=3.0` vs RoCE 1.0/1.5),
`wire_efficiency=1.0` (mirrors the ns-3 model's locked 1.0). It is a spec-derived
**lower bound anchored to ns-3** (no perftest ground truth, like RoCE); the lossy
drop/NACK/retransmit behavior is ns-3-only (same split as RoCE/DCQCN).
`make_h200_hierarchical_theory` gains a backward-compatible `so_factory` seam
(default `make_h200_roce_theory`) to swap in the Ethernet So stage. 3 new pytests
(61→64 green); parity test `test/parity/test_surrogate_vs_ns3_ethernet.py`
(lower-bound anchor, ratio ~0.2 = identical to the RoCE hierarchical anchor).
Doc: `doc/CALIBRATION.md` §"Plain Ethernet" + `USER_GUIDE.md` recipe N +
`WHITEBOX_SURROGATE.md` So-profile mention. (`eta_ethernet` skipped — Ethernet
differs from RoCE in switch *latency*, not wire *bandwidth*, so an eta would be a
redundant alias; the preset captures the store-and-forward latency.)

**Independent code review (3 read-only subagents, adversarial) — verdict
SHIP-WITH-FIXES, no blockers; RoCE parity gate re-confirmed byte-identical.**
Fixes applied: (1) `--voqDropThreshold=0` now genuinely means lossless/off
(early-return-true guard; the prior `voqDepth >= 0` would tail-drop every
packet — a total-stall foot-gun the doc called "off"); (2) the lossy-NACK
invariant `voqDropThreshold < switchVoqDepth` is now enforced
(`NS_ABORT_MSG_IF`) — the comment no longer claims the dead
`Config::SetDefault(VoqDepth,1024)` governs (the helper's
`SetVoqDepth(switchVoqDepth=10000)` supersedes it); (3) a stderr warning when
`ETH` is combined with `fullmesh`/`ring` (no switch in the inter-node path →
the lossy path is inert); (4) the RoCE parity numbers
(4769616/15307388/13421212) are now **pinned by `test_so_hierarchical_regression.py`**
(were a doc claim only); (5) new `test/parity/test_ethernet_lossy_recovery.py`
automates the incast NACK→retransmit→complete loop AND the
`--llrRetryLimit` permanent-loss fallback (were manual smoke only).

### New — full-mesh surrogate algorithm (ns-3-calibrated, the 4th shape)

The latency surrogate gains a 4th algorithm shape — `fullmesh` — calibrated
directly to ns-3 as ground truth (unlike ring/tree/nvls, which are wire-rate
derivations). `make_h200_fullmesh_theory()` ships the back-fit
`fullmesh_bw=449890 B/µs`, and a `fullmesh` branch was added at every
algo-dispatch site in `surrogate/theory/whitebox_surrogate_v2.py` (11 sites:
`__init__`/`_steps`/`_per_step`/`_bdp`/`_step_time`/`_credit_pressure`/
`predict`/`_shared_contention`/`make_surrogate`/`make_surrogate_from_wire` +
the new factory). Model: `T = startup + steps·(lat + (D/N)/fullmesh_bw)` (AR
steps=2 barrier-gated, AG steps=1) — structurally NVLS **without** the N-fold
serialization factor, because in `--topology=fullmesh` every GPU-pair gets a
dedicated `--numLanes=18` NVLink bundle (concurrent, not serialized). New
`configs/calibration/fullmesh_ns3_sweep.py` sweeps N∈{4,8,16} × {AR,AG} ×
1–256 MiB (54 pts) and back-fits the scalar on the SIMPLE regime → η=0.9998
vs the 18×25000=450000 physics, **MAPE 0.10%**. Verification: 7 new pytests
(`surrogate/test/test_theory.py`, 54→61 green) + a parity test
(`test/parity/test_surrogate_vs_ns3_fullmesh.py`, within 0.3% at N=8 AR+AG
{16,64,256} MiB) + `./ns3 build` green (no C++ change — the fullmesh injectors
already exist and are the ground truth). Doc: `doc/CALIBRATION.md` §"Full-mesh"
+ `surrogate/theory/USER_GUIDE.md` recipe M. Simple-regime scoped (sub-SIMPLE
LL/LL128 tiers out of scope, same as the ring preset); calibrated back-fit
(straddles ns-3 within ~0.1%, symmetric parity band — not a one-sided bound),
omitting per-packet credit/VOQ/barrier. No ns-3 `src/` change.

### New — document the hierarchical (Su→So) surrogate config in USER_GUIDE

`surrogate/theory/USER_GUIDE.md` gains §6 ("Scale-out (So) fabric + hierarchical
(Su→So) collectives") + recipes J/K/L. The hierarchical model is entered via the
model-agnostic `HierarchicalSurrogate` class (not the H200-only
`make_h200_hierarchical_theory` factory): it composes **any** Su sub-surrogate
(NVLink) with **any** So sub-surrogate (Ethernet RoCEv2) under a So topology, so
A800 / B200 / MI300 / any NVLink + any Ethernet fabric work the same way. §6
covers the three build levels for `su`/`so` (full-custom
`TheoryDerivedSurrogate` / wire-derived `make_surrogate_from_wire` / preset), the
Su-vs-So knob table, the So topology families (`so_fullmesh` non-blocking lower
bound / `so_spineleaf` / `so_fattree`), and the required-keyword-only
`collective=` phase decomposition (allreduce 4-phase / allgather 3-phase /
alltoall 2-phase / sendrecv So-only). Non-H200 worked examples J/K/L, all cited
numbers verified byte-exact (809.92 / 450.39 / 540.76 / 701.85 µs). Stale
`# 37 passed` → `# 54 passed` + a hierarchical-test clause fixed in §9. Doc-only;
no code or behavior change.

### New — So RoCEv2 fabric simplification pass (dead-code removal)

An independent multi-agent code review (27 review/verify agents, adversarially
verified) of the scale-out (So) RoCEv2 fabric concluded the implementation is
already lean — there is no bloat to cut, only ~49 LOC of zero-caller dead code,
provably-dead guards, and stale comments. This pass removes those; no datapath
logic changes, no behavioral change.

* Removed zero-caller accessors/getters: 6 dead `MultiNodeTopologyHelper`
  accessors (`GetInterNodeSwitchNodes`/`GetInterNodeSwitches`/etc. — never called;
  `multiTopo.` only uses `Set*`/`Build`/`GetAllEndpoints`/`GetNvSwitch`),
  `FabricEndpoint::GetInterNodeFabricType`/`GetInterNodeProtocolModel`
  (callers read the members directly via `GetFabricTypeForDest`/`GetProtocolModelForDest`),
  `SoHeader::GetChunkIndex` (only `SetChunkIndex` + serialize are used).
* Dropped the speculative `RoceProtocolModel::PerGpuStartupDelayNs` knob — the
  So startup delay is a flat `StartupDelayNs` (40 µs NCCL launch), not scaled
  per GPU; the 2-arg `GetStartupDelayNs(numGpus)` now returns it unchanged
  (the `numGpus` arg is retained only to satisfy the pure-virtual signature).
* Removed 2 provably-dead `DcqcnRateMachine` `if` wrappers (`rateFactor < 1.0`
  and `ceilingBps > 0.0`) whose predicates are always true when their outer
  guards hold (the inner `if (d > 0.0)` backstop still covers the rateFactor==1.0
  sub-epsilon corner). Behavior-identical; the ceiling pacer stays opt-in.
* `MultiNodeTopologyHelper`: `m_interNodeSwitchNodes`/`m_interNodeSwitches`
  instance members → function-locals in the `host`/`tor` branches (the members
  were write-only — the switch nodes survive via `m_allSwitchNodes`/`m_allNodes`).
* Comment-only fixes: `ethernet-switch.h`/`so-header.h` docs dropped speculative
  "subclass/extension" promises; 2 stale scratch ECN comments now say "live
  (Phase B)"; this file's earlier `GetInterNodeProtocolModel` mention trimmed to
  `SetInterNodeProtocolModel` (the getter was removed).

Parity re-verified byte-identical after the pass: `--dcqcn` off ==
pre-Phase-B (su 4769616, 2-node 15307388, 4-node 13421212 ns); `gpu-cluster`
C++ suite green at EXTENSIVE; 54 surrogate pytests green. Deferred
(do-not-touch, verified): `GetDataSize` (pure-virtual), the RocePayloadBuilder
identity overrides (pure-virtual safety rail), `GetDcqcnRateMachine` (sole public
read of a private member — conservatively kept despite zero callers), and 5
dedup refactors (left for a separate, behavior-risking pass).

### New — ns-3 RoCE P2P `ib_write_bw` per-QP ceiling pacer + validation

Opt-in per-QP sustained-throughput ceiling for the single-QP `ib_write_bw` curve
(closes #23/#24; the `ib_write_lat` single-transfer sweep + delivery-gated
completion landed in the entry below).

* `DcqcnRateMachine::PerQpCeilingGbps` (Gbps, 0 = off) is a TRUE per-flow rate
  pacer: a per-flow `nextSendNs` bookkeeper spaces consecutive chunks of one
  flowKey (one QP) so the aggregate rate settles to the ceiling
  (`chunkBytes/ceilingBps` per chunk). A constant per-chunk offset (the DCQCN
  alpha model) would only delay the burst start, leaving the NetDevice to
  serialize all chunks at the link rate — which does NOT cap sustained
  throughput; the pacer does. Resolves R-SoH-4 for the ceiling (the DCQCN
  congestion path stays per-chunk, acceptable for the lower-bound band).
* Parity-safe: default 0 = off → `ShapingDelay` byte-identical to pre-WU3
  (DCQCN path read-only, ceiling block skipped); 256 MB no-ceiling = 348.3 G
  line rate unchanged.
* `scratch/gpu-cluster-sim.cc` `--perQpCeilingGbps` CLI (mounts
  `DcqcnRateMachine` when RoCE + (`--dcqcn` or ceiling > 0)).
* `src/gpu-cluster/test/gpu-cluster-test.cc` `PerQpCeilingPacerTest`: off =
  parity (4 chunks delay 0); on = per-flow spacing (chunk i ≈ i·slot);
  independent flows; DCQCN composition (alpha binds chunk 0, ceiling binds 1).
* `configs/calibration/roce_p2p_bw_sweep.py` → converged **MAPE 3.20 %** for
  sizes ≥64 MB (64 MB 50.74 G +3.5 %, 256 MB 47.60 G −2.9 % ≈ 49 G per-QP);
  the pacer corrects the 7.1× no-ceiling overestimate (256 MB 348.3 G → 47.6 G).
  Transition zone <64 MB over-estimates (1–7 8 MB-coalesced chunks un-paced; the
  blog's sub-ceiling small-size regime is msg-issue-limited, not modeled — the
  same structural gap as `ib_write_lat`).

### New — ns-3 RoCE P2P `ib_write_lat` sweep harness + `tor` inter-node topology

Direct ns-3-vs-perftest comparison for the single-transfer `ib_write_lat` curve
(closes the "deferred ns-3 comparison" from the entry below; the inter-node
`RoceProtocolModel` was already wired, so no new C++ model was needed).

* `configs/calibration/roce_p2p_ns3_sweep.py` grid-searches
  `{startup, interNodeLatency, switchCutThroughDelay}` (interNodeBandwidth fixed
  at 400 Gbps — the GPU→switch→GPU path serializes on 2 P2P links, so 2×
  serialization at 400 Gbps yields the ~199 Gbps single-write rate) and reports
  MAPE vs `ib_write_lat`. Best fit: `startup=950 ns`, `interNodeLatency=100 ns`,
  `switchCutThroughDelay=200 ns` → **MAPE 5.26 %** (2 B +3.4 %, 64 B +1.4 %,
  1 KB −7.4 %, 4 KB −9.6 %, 64 KB +4.5 %). The floor + slope match within
  3–5 %; the 1–4 KB transition undershoots because `ib_write_lat` is sublinear
  (NIC pipeline, S^0.669) while ns-3 serialization is linear — the same
  structural divergence the analytical surrogate (2.6 % MAPE) fits via its
  sublinear term. This is the honest structural floor of the ns-3 wire model.
* New `--interNodeTopology=tor` in `MultiNodeTopologyHelper`: a single shared
  leaf switch every node's inter-node NIC hangs off (1 hop), matching
  `ib_write_lat`'s 2-NIC-into-1-leaf topology. The legacy `host` value builds
  N switches + full-mesh peer links (multi-hop, overshoots `ib_write_lat`).
* `configs/protocol_configs/p2p-roce.cfg` defaults updated to the sweep best fit
  (`startup=950`); the header documents the calibrated run CLI and the
  delivery-gated completion path.

### New — RoCE v2 P2P perftest calibration (soft, surrogate-anchored)

The scale-out (So) RoCEv2 fabric has no real-HW anchor in this release (the A800
calibration target is single-node NVLink). A third-party perftest blog
(`/share_data/zhuyu/RoCEv2_benchmark_data.md`, ConnectX-7 400G, RoCEv2 GID3 IPv4,
MTU 4096, RC, TX depth 128 — `ib_write_bw` / `ib_write_lat`) is used as a
**soft, externally-anchored sanity calibration** of the *raw-RDMA scalar
constants*, **not** a fidelity verdict on collective simTime. Perftest is raw RDMA
(one-sided write), the layer *below* NCCL — it does not exercise the 40 µs
NCCL-launch startup, so it calibrates the wire + NIC constants, not collective
timing.

* `configs/calibration/roce_p2p_calib.py` encodes the blog data and fits two
  models; extracted constants land in `configs/calibration/roce_p2p_calib.json`:
  per-QP single-flow ceiling 49.0 Gbps, wire efficiency 0.988 (8-QP saturation
  395/400), per-message fixed latency 1.45 µs (doorbell + prop + completion),
  single-write serialization ~199 Gbps (2B→64 KB slope; surrogate uses the round
  192 Gbps = 24 k B/µs), NIC message-issue ~8.2 Mmsg/s, 4 KB
  saturation knee, line rate 400 Gbps match.
* `surrogate/theory/whitebox_surrogate_v2.py::make_connectx7_p2p_perftest_theory()`
  returns a `TheoryDerivedSurrogate` whose `predict(D, 2, "ring",
  collective="sendrecv")` reproduces `ib_write_lat` at MAPE 2.6 % (2 B / 64 B /
  64 KB within 2 %). 2 pytests in `surrogate/test/test_theory.py` (defaults +
  fit-vs-blog) green.
* ns-3 **does** support P2P: `P2pTraffic` + `FabricEndpoint::SendP2p` (the raw
  single-packet send, no NCCL startup) + the `.cfg` `[op]` stencil `kind = P2P`
  (`P2P_TRANSFER`). Runnable demo `configs/protocol_configs/p2p-single.cfg`
  (`startup = 0` → raw transfer; verified: `simTimeUs` scales with size, 2 B →
  1.2 µs, 8 MiB → 836.8 µs).
* The perftest **validates** the ns-3 RoCE wire constants — `wire_efficiency`
  0.95 is slightly conservative vs the 0.988 best-case 8-QP saturation, and the
  400 Gbps line rate matches — so **no ns-3 model change is warranted** here.

Update — the deferred ns-3-vs-perftest comparison is now **done** for
`ib_write_lat` (the inter-node `RoceProtocolModel` was already wired out-of-band;
a `h200-roce.profile` PEX bundle is **not** required). The `.cfg` `SEND_P2P`
completion is **delivery-gated** — the prior "measures dispatch time, not
delivery" claim is **retracted** (`ProtocolConfigRunner` stamps `m_startNs` at
`Start()` and computes `durationNs = Now - m_startNs` at `OnComplete` after the
`PACKET_DELIVERED` event; `simTimeUs` scales with size, impossible for
fixed-dispatch — see `doc/CALIBRATION.md`). Remaining follow-up: the opt-in
per-QP ceiling (`PerQpCeilingGbps` ≈ 49 G) for `ib_write_bw` sustained-throughput
parity (not needed for the `ib_write_lat` single-transfer sweep). Detail in
`doc/CALIBRATION.md` §"RoCE v2 P2P (scale-out)".

### New — polymorphic SoHeader + ECN carrier for the scale-out fabric (WU-SoH)

The scale-out (So) RoCEv2 fabric needed an ECN bit, but the shared
`FabricHeader` (39 B, on every Su+So packet) has no spare flag bit and its
enums are full bytes — adding ECN to it would perturb the
`packet->GetSize()`-based egress serialization for ALL traffic and break Su
parity (the WU3 deferral rationale). Solved by stamping a **second, per-So
header** (`SoHeader`, 10 B: protocolId + flags[bit0=ECN] + dataSize +
chunkIndex) onto So packets only, inner to FabricHeader. Su packets never
carry it, so Su parity is untouched.

`SoHeader` is a real polymorphic `ns3::Header`: a `protocolId` discriminator
makes it protocol-pluggable (today `RoceProtocol::SIMPLE = 1`, the seam open
to other So protocols via the `ProtocolPayloadBuilder` factory). The old
8-byte raw `RoceRdmaHeader` memcpy prefix in `RocePayloadBuilder` is promoted
to this typed header (`AddHeader`/`RemoveHeader`/`PeekHeader`); the 0.95 wire
efficiency absorbs the 8→10 B change, so So `simTime`s are bit-identical
(verified: 2-node 15307388 ns, 4-node 13421212 ns, unchanged from WU2/WU4).
WU-SoH1 adds `so-header.{h,cc}` + the builder refactor + a round-trip/ECN
unit test; WU-SoH2 adds the **ECN-mark egress hook**:
`NvSwitch::OnPacketEnqueue(packet, outPort, voqDepth)` is a new `protected
virtual` (default no-op → NvSwitch/Su parity preserved) called in
`EnqueueToVoq` before the fast-path; `EthernetSwitch` overrides it and its
public `MarkEcnIfCongested` stamps the SoHeader ECN bit when
`voqDepth >= EcnThreshold` (Attribute, default 8), but only on RoCE DATA
packets — control packets (CREDIT/ACK/NACK) and non-RoCE packets bypass
(matching the BER bypass), since marking control feedback would deadlock
congestion control. Unit test `EthernetSwitchEcnTest` covers shallow
(no mark), at-threshold (mark), control (untouched), non-RoCE (untouched).

Deferred to Phase B (WU-SoH5, queued, own RISKS entry): the coupled
congestion *reaction* — receiver ECN peek → CNP control packet back to the
source (WU-SoH3), sender DCQCN Alpha rate-backoff + PFC priority pause
(WU-SoH5). Phase B is blocked on threading `FlowControlGate` (presently
called only from `SendMemoryRead`; the ~7 bulk-send paths bypass it) through
the parity-sensitive send paths, an invasive change separated from the
clean, parity-safe Phase A header+mark foundation. Phase A's hook is inert
without a CNP/rate consumer, so it ships timing-neutral.

### New — CNP control packet + ECN-echo trigger on the receive path (WU-SoH3)

Phase A step 3. The ECN *reaction* receiver half is added, mirroring the
WU-SoH2 *mark* half:

* `FabricPacketType::CNP` (= 16) is a new control packet type, classified by
  `FabricHeader::IsControlPacket()` alongside CREDIT/ACK/NACK/PERMANENT_LOSS.
  This is the critical invariant: CNP rides the same lossless control path
  and bypasses BER sampling + ECN marking (else CNP feedback would deadlock
  congestion control). `EthernetSwitch::MarkEcnIfCongested` already skips
  control packets, so a CNP is never re-marked.
* `FabricEndpoint::SendCnpPacket(markedHeader)` emits a CNP control packet
  back to the source of an ECN-marked segment, mirroring `SendCreditPacket`.
* In `ProcessProtocolDataPacket`, when the (already-FabricHeader-peeled)
  packet carries a RoCE SoHeader with the ECN bit set, `SoHeader::PeekEcn`
  triggers `SendCnpPacket` back to the source.

**Dormancy (important, by design — activation deferred to Phase B).** The
ECN trigger gates on `protocol == RoceProtocol::SIMPLE` (set by
`RocePayloadBuilder::BuildChunks`, which stamps a SoHeader). The production
So send path, however, routes inter-node traffic through
`SendBulkWireTransferSize`, which stamps `SetProtocol(0)` and emits **no
SoHeader** (the RoCE model only governs wire-size/efficiency, not the
on-wire header layout, and `RocePayloadBuilder` is never wired in the sim).
Consequently the WU-SoH2 mark guard *and* the WU-SoH3 CNP trigger never fire
on production So packets: the ECN mechanism is correct but **dormant**.
Parity stays bit-identical because the mechanism does not trigger, not
because it is wired into the live path. The C++ `EthernetSwitchEcnTest`
(unit-level) verifies the mark+peek logic is correct when a RoCE packet
*does* carry a SoHeader. **Activating** the mechanism requires a live So
send path that emits `protocol=RoCE::SIMPLE` packets carrying a SoHeader
(two candidate seams: mount `RocePayloadBuilder` on So endpoints, or stamp a
lightweight SoHeader in `SendBulkWireTransferSize`). Both touch
parity-sensitive send/wire-layout and are deferred to Phase B (WU-SoH5)
alongside the DCQCN rate consumer, so the whole ECN↔rate loop lands together.
Activation preconditions (R-SoH-1) are the two seams named above; revisit at
Phase B kickoff.

### New — --ecnThreshold CLI knob + Phase A closeout (WU-SoH4)

`scratch/gpu-cluster-sim` gains `--ecnThreshold` (uint32, default 8, 0=off),
wired to the `EthernetSwitch::EcnThreshold` Attribute via
`Config::SetDefault` before the So fabric is built (so every EthernetSwitch
in the multi-node path picks it up). This closes Phase A of the SoHeader/ECN
effort (WU-SoH1 header + WU-SoH2 mark + WU-SoH3 CNP + this knob): the full
ECN *carrier + mark + echo* seam exists, is unit-tested, and is gated
behind a CLI flag, with Phase B (DCQCN rate reaction + PFC + activation of
the live SoHeader-bearing send path) queued as WU-SoH5.

Dormancy re-confirmed end-to-end: the So hierarchical allreduce is
bit-identical across `--ecnThreshold` values (default 8, 0, and 1 all yield
2-node simTimeNs=15307388, 4-node 13421212, su 4769616) because the So send
path emits `protocol=0` packets with no SoHeader, so the ECN guard never
fires regardless of threshold. The knob is therefore a no-op on today's So
traffic and ships parity-safe; it becomes live once Phase B activates the
SoHeader-bearing send path.

### New — Phase B: activate the live ECN↔CNP↔rate loop + DCQCN rate machine (WU-SoH5)

Phase B closes the loop that Phase A (WU-SoH1..4) built dormant. The two
activation preconditions of R-SoH-1 are satisfied, the rate consumer is added,
and the whole ECN→CNP→rate→shaping chain is wired onto the live So send path.

**Activation (Seam B — resolves R-SoH-1).** The production inter-node send path
`FabricEndpoint::SendBulkWireTransferSize` now stamps a SoHeader (10 B,
`RoceProtocol::SIMPLE`, inner to FabricHeader) onto every RoCE chunk when
`efficiency < 1.0 && m_payloadBuilder == RocePayloadBuilder` — i.e. on So
endpoints only. Su endpoints keep `efficiency == 1.0` (NCCL SIMPLE) and emit no
SoHeader, so Su parity is untouched. This lifts the dormancy documented under
WU-SoH3: the WU-SoH2 mark guard (`fh.GetProtocol() == RoCE::SIMPLE`) and the
WU-SoH3 ECN-peek (`SoHeader::PeekEcn`) now fire on real So traffic.

**DCQCN rate machine (`model/dcqcn-rate-machine.{h,cc}`).** A per-flow Alpha
state machine keyed by `MakeSeqKey(destRank, vcId, flowId)` (the same 64-bit key
the reorder buffer uses):

* `OnCnp(flowKey)`: Alpha grows `alpha += (1 - alpha) * G` (G=0.5), capped at 1.
* Fast-recovery timer (`TFast`=50 µs, ≤5 cycles): each tick multiplicatively
  shrinks Alpha (`G *= GDecay` (0.5), `alpha -= alpha*G`), modeling the
  rate-halving→re-ramp of DCQCN.
* Additive-increase timer (`TAi`=100 µs): drives Alpha toward `AlphaFloor`
  (0.0) at rate `RAI`=0.01 (linear resume).
* `ShapingDelay(chunkBytes, flowKey)`: maps Alpha to an egress delay,
  `rateFactor = clamp(1 - alpha/2, 0.5, 1.0)`, `delay =
  (chunkBytes/MaxRateBps) * (1/rateFactor - 1)` (MaxRateBps=50 Gbps). At
  `alpha <= AlphaFloor` (the default, no congestion) the delay is exactly 0,
  so a mounted-but-idle machine is a pure no-op → mounting is parity-safe.

**Egress chokepoint.** `SendPacketOnDevice` is the sole egress path (three send
branches: WFQ-scheduled, FEC-only, and immediate). The shaping delay is folded
into all three, after the per-protocol efficiency already applied — so Su
packets (eff=1.0) are never shaped, only So packets (eff=0.95) with a charged
flow. Control/memory/retry packets are explicitly skipped (they bypass shaping
just as they bypass BER, per [[nvswitch-control-packet-bypass]]).

**CNP→rate dispatch.** `FabricEndpoint::ProcessCnpPacket` peels the CNP, recovers
the (source, vc, flow) tuple, and calls `m_dcqcnRate->OnCnp(flowKey)`. The CNP
itself is a `FabricPacketType::CNP` control packet, so it rides the lossless
control path and bypasses BER sampling and ECN marking (WU-SoH3 invariant
reconfirmed) — a CNP can never be dropped or re-marked, so the feedback loop
cannot deadlock.

**CLI + mounting.** `scratch/gpu-cluster-sim` gains `--dcqcn` (bool, default
false). When on AND `interNodeFabricType == ROCE`, a single shared
`DcqcnRateMachine` is constructed and mounted on every So endpoint via
`SetDcqcnRateMachine`. Off (the default) leaves `m_dcqcnRate` null on all
endpoints → every shaping fold short-circuits → bit-identical to pre-Phase-B.

**Tests (6 green, `./ns3 build` + `--fullness=EXTENSIVE`).** Existing
`EthernetSwitchEcnTest` (switch marks on backlog, skips control/non-RoCE) and
`CnpReactionTest` (ECN-marked segment → CNP at receiver). New:
`DcqcnRateMachineTest` (dormant alpha=0/delay=0; OnCnp→0.5; shaping math at
alpha=0.5, rateFactor=0.75; fast-timer→0.375 after 60 µs),
`CnpDispatchTest` (CNP over the real P2P wire → ProcessCnpPacket → alpha=0.5),
`SoOnlyPredicateTest` (Su packet egresses synchronously unshaped via MacTx;
So packet egress is deferred by the shaping delay), `DormantTest` (fresh
machine zero-delay; unmounted CNP is a no-op; a later mounted CNP then charges
alpha — the contrast proves the null early-return).

**Parity.** `--dcqcn` off is bit-identical to pre-Phase-B across the So
hierarchical smoke (su 4769616, 2-node 15307388, 4-node 13421212 ns) — the
default-off null path is a pure no-op.

**End-to-end dormancy under light load (faithful, not a bug — see R-SoH-3).**
With `--dcqcn` ON, the single-flow hierarchical allreduce smoke test is
*still* bit-identical (15307388). The mechanism is now fully live-wired —
SoHeader is stamped on the bulk send, `NvSwitch::EnqueueToVoq` calls
`OnPacketEnqueue` (the ECN-mark hook) on every transit packet, the receiver
peeks ECN, and the rate machine is mounted — so the chain *can* fire. It does
not fire because the So fabric is non-blocking cut-through:
`EnqueueToVoq` calls `OnPacketEnqueue` with the *arrival* VOQ depth *before*
the enqueue, and the empty-VOQ fast-path forwards immediately at depth 0, so
`MarkEcnIfCongested` sees `voqDepth=0 < threshold` → no mark. ECN marks only
packets arriving *behind a backlog*, exactly as real RoCE does. The single
hierarchical allreduce builds no sustained VOQ backlog, so no CNP, so Alpha
stays 0. This is correct RoCE semantics: DCQCN engages only under genuine
congestion (incast / oversubscription). Exercising the loop e2e requires a
multi-flow incast scenario (deferred, R-SoH-3); the 6 unit tests cover every
link of the chain in isolation.

**PFC deferred (R-SoH-2).** Priority Flow Control (per-VC pause on VOQ
overflow) is the lossless backpressure complement to DCQCN. It is deferred:
it requires threading `FlowControlGate` (presently called only from
`SendMemoryRead`; the ~7 bulk-send paths bypass it) through the
parity-sensitive send paths, an invasive change separated from this
parity-safe Phase B. The `FlowControlGate` virtual hook (F9) is the seam;
PFC's unqueue condition is documented in `doc/RISKS.md`.

### Fix — gate ECN mark/peek on the RoCE fabric, not the SIMPLE protocol byte (WU-SoH5 review)

`RoceProtocol::SIMPLE = 1` collides with `NcclProtocol::LL = 1` on the shared
protocol byte of `FabricHeader`. The two Phase-B ECN sites gated on
`GetProtocol() == RoceProtocol::SIMPLE` alone, so a Su LL packet (protocol=1,
**no** SoHeader) matched the guard:

* `EthernetSwitch::MarkEcnIfCongested` (switch-side): a Su LL packet
  traversing the host-topology EthernetSwitch (which routes both Su and So
  traffic) at a backlogged VOQ would pass the guard and `RemoveHeader(SoHeader)`
  would read 10 B of NCCL LL payload as a SoHeader — corrupting the packet
  (data-correctness, not just a spurious CNP).
* `FabricEndpoint::ProcessProtocolDataPacket` (receive-side): the same packet
  would reach `SoHeader::PeekEcn` and read 10 B of payload as a SoHeader,
  emitting a spurious CNP (and charging a never-shaped Su flow key).

Both sites now ALSO require `GetFabricType() == FabricType::ROCE`, since only
RoCE-fabric (So) packets carry a SoHeader (stamped by `SendBulkWireTransferSize`
which sets `SetFabricType(GetFabricTypeForDest(destRank)) = ROCE`). The Su ring
parity baseline uses `NcclProtocol::SIMPLE = 3` (no collision), so parity is
unchanged: su 4769616, 2-node 15307388, 4-node 13421212 ns, `--dcqcn` on/off
identical. `EthernetSwitchEcnTest` gains case (5): a Su LL packet
(NVLINK fabric, protocol=1, no SoHeader) at a deep queue is neither marked nor
mutated. Found by an independent subagent code review of `840f48b`.

### Fix — scale-out hierarchical surrogate volume overcount + review follow-ups (WU7)

A clean-context, 7-dimension code review of the scale-out RoCEv2 +
hierarchical-collectives work (WU1-6, commit cc50698) returned verdict
`ship-with-fixes`: 0 blockers, 1 MAJOR, the documented deferrals (PFC/DCQCN,
MTU, 2-tier spine-leaf, config-primary So parity) confirmed honest. The MAJOR
and follow-up fixes:

* **MAJOR — So-phase volume overcount.** `HierarchicalSurrogate.predict` fed
  the FULL tensor `data_bytes` to every phase. The allreduce inter-node
  So reduce-scatter / all-gather stages move only the per-node reduced chunk
  `D / n_su` (after the intra-node RS, each node holds `D/n_su`), matching
  `hierarchical-allreduce.cc`; feeding full `D` was an `n_su`-x overcount
  (8x at 8 GPUs/node). Fixed: `so_chunk = data_bytes // self.n_su` drives the
  two So phases; the Su RS/AG phases (full `D`) are unchanged. The fix
  STRENGTHENS the declared lower bound (the surrogate shrinks further below
  ns-3): the `test/parity/test_surrogate_vs_ns3_hierarchical.py` anchor ratios
  drop 0.30->0.20 (2-node) and 0.40->0.23 (4-node), both still >= the 0.05
  floor. Discovered as a consequence (the `allgather < allreduce` invariant is
  the oracle): allgather phase-1 (intra-node gather) must move the per-node
  share `D / n_so`, else allgather would exceed allreduce once the allreduce
  So-phase volume is corrected. Fixed with `node_share = data_bytes //
  self.n_so` on phase 1; phases 2 (So AG) + 3 (Su AG) + alltoall (both) keep
  the full `D`. The false docstring claim ("the per-phase bus-factor volume
  D*(N-1)/N then comes out correctly") is rewritten to state the true per-phase
  data sizes. 52 surrogate tests green (was 51; +1 new keyword-only test).
* **`collective=` is keyword-only on the hierarchical path too.** The
  `HierarchicalSurrogate.predict` signature gains a bare `*` before
  `collective`, matching the `oss-surrogate-collective-kwarg` rule (collective
  is a required keyword-only arg, never inferred from `algo`). A new test
  asserts positional `collective` raises `TypeError`; all existing callers use
  `collective=` so nothing breaks.
* **Doc nits.** `roce-protocol-model.h` and `.cc` described the So segment as
  "MTU-sized" / "segmentation granularity, <= MTU", but the builder coalesces
  into 512 KB `ChunkSize` segments (real 9 KB jumbo-MTU frame splitting is a
  documented WU3 deferral). Comments/help strings corrected to name `ChunkSize`
  and point at CHANGES.md WU3.
* **CI wires in `test/parity/`.** `.github/workflows/ci.yml` pytest line now
  runs `surrogate/test/ test/parity/ -q` so a RoCE-path regression (the So
  hierarchical regression + the surrogate-vs-ns3 anchor) cannot pass CI green
  silently. The `gpu-cluster-sim` binary the parity tests invoke is built in
  the preceding "Build" step.
* **Honest task ledger.** The WU3/WU4/WU5 task descriptions now split DELIVERED
  vs DEFERRED, cross-referencing the CHANGES.md deferral notes, so the ledger
  no longer reads as "PFC/DCQCN/MTU + 2-tier spine-leaf + config-primary So
  parity shipped" when those are scoped-out lower-bound choices. No code
  behavior changed.

Surrogate math correctness verified against ns-3 hierarchical-allreduce (the
anchor) and against the `allgather < allreduce` volume invariant. Not a
calibration change — the So term remains a declared uncalibrated lower bound.

### New — RoCE v2 protocol model + payload builder (scale-out WU1)

`model/roce-protocol-model.{h,cc}` (`: ProtocolModel`) and
`model/roce-payload-builder.{h,cc}` (`: ProtocolPayloadBuilder`) add the first
scale-out (Ethernet RoCEv2) protocol to the OTP layer. The model is
SIMPLE-only with spec/NIC-derived constants (40µs RDMA startup, 512KB
segment, ~95% wire efficiency for Eth+IP+UDP+BTH+aETH, 800ns per-switch-hop
serialization, RDMA memory-semantic latencies). The builder prepends an 8-byte
RDMA transport header (size + chunkIndex) per segment and strips it on extract.
Registered in `protocol-profile.cc` (RoCE -> RoCE builder) and added to
`CMakeLists.txt`. Two unit tests (`RoceProtocolModelTest`,
`RocePayloadBuilderTest`) verify wire-size/efficiency/startup defaults,
8B-header round-trip, and multi-segment chunking. Uncalibrated lower-bound
(anchors the So fabric timing to the ns-3 model, not measured HW). WU1 of the
scale-out RoCEv2 fabric + hierarchical SU->SO collectives effort; the Ethernet
switch (WU2), PFC/DCQCN + per-fabric model + MTU (WU3), So topology builders
(WU4), configs/parity (WU5), and surrogate two-stage model (WU6) follow.

### New — Ethernet (RoCEv2) switch fabric for scale-out (WU2)

`model/ethernet-switch.{h,cc}` adds the scale-out switch as a thin
`EthernetSwitch : public NvSwitch` — a distinct, pluggable TypeId
("ns3::EthernetSwitch") selected via `NvSwitchHelper::SetSwitchType`, reusing
the proven L2 MAC-learning + VOQ + crossbar-arbiter + cut-through datapath
inherited from NvSwitch, but carrying no NVLink in-network compute (SHARP
all-reduce / NVLS all-gather are scale-up concepts; they default off on
NvSwitch and stay off here). `GetVendorName()` returns "Ethernet-RoCEv2" so
the So fabric is distinguishable from the Su NVSwitch ("NVIDIA"). Added to
`CMakeLists.txt`. The coupled RoCEv2 lossless subsystem (PFC priority flow
control, ECN marking, DCQCN rate backoff, MTU packetization) is layered on
the inherited datapath in WU3 (deferred so the FabricHeader — which has no
ECN field — is touched only once, with its serialization-size impact analyzed
together, since `packet->GetSize()`-based egress timing is sensitive to a
header-byte change). One unit test (`EthernetSwitchSeamTest`) verifies the
seam: `SetSwitchType("ns3::EthernetSwitch")` builds an object that is-a
FabricSwitch and is-a NvSwitch with the Ethernet-RoCEv2 vendor name. WU2 of
the scale-out RoCEv2 effort; WU1 (RoCE model+builder) precedes, WU3+ follow.

### New — per-fabric protocol-model selection for scale-out (WU3)

`FabricEndpoint` now selects its protocol model per destination: a new
`m_interNodeProtocolModel` (e.g. `RoceProtocolModel`) is consulted for
remote-rank (inter-node / So) timing, while the existing `m_protocolModel`
(NCCL) stays for local-rank (Su) traffic. The new
`GetProtocolModelForDest(destRank)` mirrors the existing `GetFabricTypeForDest`
gate (active only when `!IsLocalRank && m_localRankCount > 1`), so when no So
model is wired the endpoint falls back to `m_protocolModel` everywhere and
single-node / Su behavior is byte-identical (parity-verified:
`test_config_vs_inline_parity.py` all PASS). The seven model-consultation sites
on the send path (`SendDataAutoProtocol`, `SendBulkWireTransferSize`,
`SendDataInternal`) and the receive path (`ProcessProtocolDataPacket`, using
the packet's source rank for symmetry) now route through
`GetProtocolModelForDest`. Because the hierarchical inter-node phase uses
`SendBulkWireTransfer(->Size)`, an So model's `GetWireSize` (ceil(data/0.95))
now governs the inter-node wire bytes, so the Ethernet+IP+UDP+IB overhead is
honored. `SetInterNodeProtocolModel` accessor is added; the model is set
programmatically (not a TypeId PointerValue attribute,
per the Construct-nulls-it seam constraint). One unit test
(`InterNodeProtocolModelSeamTest`) verifies local->NCCL / remote->RoCE
selection.

Deferred (out of WU3, by design): the coupled RoCEv2 congestion-control
subsystem (PFC priority flow control, ECN marking, DCQCN rate backoff) and
real MTU frame packetization are NOT added here. Rationale: (a) the
FabricHeader has no ECN field and no spare flag bit, so an ECN carrier would
require either a header-byte change (which perturbs the
`packet->GetSize()`-based egress serialization for ALL traffic, breaking Su
parity) or a side-band PacketTag; (b) real 9000B MTU frame splitting on large
bulk transfers would reintroduce the packet-explosion/memory-thrash that the
8MB chunk-coalescing was added to prevent, and the per-MTU header overhead is
already captured in the RoCE model's 0.95 wire efficiency; (c) DCQCN rate
backoff is quiescent for the non-congested single-collective latency target
(the lower-bound So model = lossless via the existing per-VC CREDIT flow
control + full line rate), and its Alpha/CNP state machine does not map cleanly
onto the credit-gated sender. Lossless + full-rate is the RoCE v2 latency
lower bound; congestion dynamics are a separable future work unit for
incast/multi-flow fidelity. WU3 of the scale-out effort.

### New — scale-out Ethernet RoCEv2 fabric wiring (WU4)

The multi-node path now builds a real switched Ethernet RoCEv2 So fabric
instead of the fake P2P-fullmesh + NCCL-timing stand-in.
`MultiNodeTopologyHelper` gains `SetInterNodeSwitchType(typeId)`: the switched
inter-node topology ("host" = one ToR switch per node + a full mesh of
switches, non-blocking) now selects the switch subtype, so
`scratch/gpu-cluster-sim` sets `ns3::EthernetSwitch` (WU2) when
`--interNodeFabricType=RoCE`. Because EthernetSwitch derives from NvSwitch,
the existing VOQ/arbiter/cut-through datapath + static-route wiring is reused
unchanged (default TypeId stays NvSwitch, so prior "host" behavior is
byte-identical). Each endpoint also receives a `RoceProtocolModel` via
`SetInterNodeProtocolModel` (WU3) so the inter-node phase honors the Ethernet
~0.95 wire efficiency; local-rank traffic keeps the scale-up NCCL model. No
new CLI flags — the existing `--interNodeTopology` (host/fullmesh/ring),
`--interNodeBandwidth`, `--interNodeLatency`, `--interNodeFabricType`,
`--interNodeStartup` drive the So fabric. Verified e2e: `--numNodes={2,4,8}
--gpusPerNode=8 --interNodeTopology=host --interNodeFabricType=RoCE
--collective=allreduce --algorithm=hierarchical --dataSize=268435456` completes
losslessly (switchDroppedPackets=0, no retransmits) at 15.3/13.4/13.1 ms —
multi-node latency >> single-node Su (4.1 ms), and the plateau with N is the
expected non-blocking-fullmesh property (the per-node NIC, not the
inter-switch fabric, is the bottleneck). The hierarchical injectors'
inter-node phase auto-routes on the So fabric via `GetFabricTypeForDest` — no
injector edits. WU4 of the scale-out effort.

Deferred: a true 2-tier spine-leaf So topology (which would show N-scaling via
oversubscription) is not added — the "host" fullmesh-of-switches is the
non-blocking lower-bound So fabric, sufficient for the
uncalibrated-lower-bound calibration; the surrogate's `so_spineleaf` family
covers the analytical case.

### New — scale-out So regression test (WU5)

`test/parity/test_so_hierarchical_regression.py` locks in the WU4 So fabric
behaviour: a hierarchical allreduce over the real EthernetSwitch RoCEv2 fabric
(2-node + 4-node `--interNodeTopology=host --interNodeFabricType=RoCE
--collective=allreduce --algorithm=hierarchical`) must (a) complete, (b) be
lossless (`switchDroppedPackets=0`, `permanentLosses=0`), and (c) cost more
than the single-node Su ring allreduce (the So inter-node phase adds
reduce-scatter + allgather across nodes). This is the So-path counterpart to
`test_config_vs_inline_parity.py`: where that test gates config-vs-inline
bit-identity of the Su calibrated path, this gates the existence + losslessness
+ latency ordering of the So RoCEv2 fabric.

Deferred: config-primary So parity (a `.cfg` reproducing the hierarchical inline
path bit-for-bit) is not added. The config-file path sources only single-node
fabric-hardware keys + the `collective`/`algorithm`/`topology` op axes into the
validated inline injectors — it does not source `numNodes`/`gpusPerNode`/
`interNode*`, so a `h200-hierarchical-allreduce.cfg` would fall through to the
inline hierarchical branch with `numNodes=1` and never exercise the So fabric,
and a `h200-roce.profile` would mis-apply RoCE wire-efficiency to single-node
Su (NVLink) traffic. The So fabric is therefore validated via the scratch
inline path (WU4 e2e + this regression test), not via config parity; config-
primary So support is a future extension that requires sourcing the multi-node
axes. WU5 of the scale-out effort.

### New — surrogate two-stage (Su+So) hierarchical model (WU6)

`surrogate/theory/whitebox_surrogate_v2.py` gains a hierarchical
scale-out model that mirrors the ns-3 WU1-5 work. (1) `wire_efficiency`
(default 1.0) folds RoCEv2's per-transfer Eth+IP+UDP+IB header overhead into
the serial bandwidth (`T_serial /= wire_efficiency`); 1.0 is byte-identical to
the NVLink scale-up model (no header), so all 37 prior surrogate tests stay
green. (2) `phase='rs'/'ag'` (default `'full'`) selects a single-pass
sub-collective: it halves the step count so the serial volume stays one
bus-factor (steps x per_step is invariant) and only the fixed per-step latency
halves -- the primitive the hierarchical decomposition needs. `'full'` is
byte-identical to prior behavior. (3) Three scale-out topology families
(`so_fullmesh` / `so_spineleaf` / `so_fattree`) join `_TOPO_FAMILIES`:
`N` is the NODE count, `links_per_gpu` is NICs-per-node, `per_link_gbps` is
the Ethernet wire rate (50 = 400GbE); `so_fullmesh` is the non-blocking lower
bound (full bisection, hop=1), `so_spineleaf` (hop=2) / `so_fattree` (hop=3)
are oversubscribed inter-tier fabrics whose bisection caps the So schedule
rate. (4) `make_h200_roce_theory` is the So profile (400GbE-derived ring bw
160000 B/us, 40 us RDMA startup, 0.95 wire efficiency, 8 NICs/node, ~1 us/hop,
1.5 us switch pipeline). (5) `HierarchicalSurrogate` composes the Su + So
sub-collectives per collective -- allreduce 4-phase (su_rs + so_rs + so_ag +
su_ag), allgather 3-phase, alltoall 2-phase, sendrecv So-only -- each phase
fed the full tensor + its participant count. `make_h200_hierarchical_theory`
is the factory; `collective=` stays required keyword-only (never inferred from
`algo`), consistent with `TheoryDerivedSurrogate`.

The So term is an UNCALIBRATED LOWER BOUND anchored to ns-3 (spec-derived RoCE
params, not real-HW calibration) -- like the NVLS band, the hierarchical
latency sits below the ns-3 packet-level simulation. Verified:
`test/parity/test_surrogate_vs_ns3_hierarchical.py` runs the ns-3 hierarchical
allreduce (2/4 node, 256 MiB, default params + RoCE So) and asserts the
surrogate is at/below ns-3 (2-node ratio 0.30, 4-node 0.40). The band is
deliberately loose: ns-3 runs with default fabric params (800 GB/s Su) while
the surrogate uses the HW-calibrated Su ring rate, AND ns-3's hierarchical Su
phase runs on the switched fabric (which `h200-ring-allreduce.cfg` warns is
~6x slower than the calibrated P2P ring the surrogate's Su term assumes) --
both make the surrogate a lower bound. A tight, param-matched anchor (back-
fitting the So startup/congestion term to ns-3) is deferred (uncalibrated-
lower-bound scope). `surrogate/test/test_theory.py` adds 14 hierarchical
tests (monotonicity in nodes, topology lower-bound ordering, 4-phase additive
decomposition, allgather<allreduce, sendrecv So-only, collective required/
validated, wire-efficiency + phase parity) -- 51 surrogate tests green. WU6
of the scale-out effort; the ns-3 + surrogate scale-out work is complete.

### Breaking — `[op]` declares the three CLI axes directly (22e8edc)

A `.cfg` `[op]` now declares the three real CLI axes directly —
`collective =` + `algorithm =` (+ optional `topology =`) — instead of the
opaque single-keyword selector that previously mapped the values
`ring`/`tree`/`sharp`/`hierarchical`/`nvls` to a preset. That selector, its
internal map, and the `Get*()` accessors behind it were removed (replaced by
`GetCollective()`/`GetAlgorithm()`/`GetTopology()`); the parity test was
renamed `test_config_builtin_parity.py` -> `test_config_vs_inline_parity.py`
(the case table `BUILTIN_CASES` -> `OP_CASES`). The old single-keyword form
is no longer accepted; config runs stay bit-identical to the inline path
(`simTimeNs` equality).

### New — surrogate `topology=` descriptor (98030aa, f3f6478)

`TheoryDerivedSurrogate.predict()` / `predict_tail()` accept an optional
`topology=` argument (a `Topology` dataclass) that applies a bisection-bandwidth
cap to the serialization term and `(hop-1) * link_latency` of per-step
propagation. Build it with `make_topology(bisection_gbps=, hop_count=)` or
`make_topology_from_family(family, N, per_link_gbps=)` (15 fabric families);
omitting it is byte-identical to the ideal-fabric default. See `surrogate/theory/USER_GUIDE.md`.

### New — surrogate `collective=` (AlltoAll + Send-Recv) is a required argument (4989fd1, amended over 4736942)

`TheoryDerivedSurrogate.predict()` / `predict_tail()` take a **required**
`collective=` keyword (`allreduce` / `allgather` / `alltoall` / `sendrecv`).
Together with `algorithm` it fixes the step count and per-step volume, so the
surrogate now models AlltoAll (`N-1` ring/collnetdirect permutation phases, or
`⌈log₂N⌉` tree rounds; per-GPU volume `D(N-1)/N` — the correction the DSE
analytical model describes in comments but does not apply) and point-to-point
Send-Recv (`steps=1`, full payload, no pipeline credit pressure). It is **not
inferred from `algorithm`** — the caller must name the operation explicitly;
omitting it raises `TypeError`, an invalid name raises `ValueError`. AlltoAll
cross-section contention is captured by the existing `topology=` bisection cap
(no separate bandwidth constant). Build with `make_h200_alltoall_theory()`
(46 µs SIMPLE startup) / `make_h200_sendrecv_theory()` (15 µs startup). See
`surrogate/theory/USER_GUIDE.md`.

See `README.md` and `doc/` for the full module overview.

---

# ns-3: API and model change history

ns-3 is an evolving system and there will be API or behavioral changes from time to time. Users who try to use scripts or models across versions of ns-3 may encounter problems at compile time, run time, or may see the simulation output change.

We have adopted the development policy that we are going to try to ease the impact of these changes on users by documenting these changes in a single place (this file), and not by providing a temporary or permanent backward-compatibility software layer.

A related file is the [RELEASE_NOTES.md](RELEASE_NOTES.md) file in the top level directory. This file complements [RELEASE_NOTES.md](RELEASE_NOTES.md) by focusing on API and behavioral changes that users upgrading from one release to the next may encounter. [RELEASE_NOTES.md](RELEASE_NOTES.md) attempts to comprehensively list all of the changes that were made. There is generally some overlap in the information contained in [RELEASE_NOTES.md](RELEASE_NOTES.md) and this file.

The goal is that users who encounter a problem when trying to use older code with newer code should be able to consult this file to find guidance as to how to fix the problem. For instance, if a method name or signature has changed, it should be stated what the new replacement name is.

Note that users who upgrade the simulator across versions, or who work directly out of the development tree, may find that simulation output changes even when the compilation doesn't break, such as when a simulator default value is changed. Therefore, it is good practice for _anyone_ using code across multiple ns-3 releases to consult this file, as well as the [RELEASE_NOTES.md](RELEASE_NOTES.md), to understand what has changed over time.

This file is a best-effort approach to solving this issue; we will do our best but can guarantee that there will be things that fall through the cracks, unfortunately. If you, as a user, can suggest improvements to this file based on your experience, please contribute a patch or drop us a note on ns-developers mailing list.

## Changes from ns-3.46.1 to ns-3.47

### New API

* (applications) New IEEE 802.11ax evaluation methodology traffic generator applications have been added: `TgaxVideoTraffic` (video streaming), `TgaxVoipTraffic` (VoIP), `TgaxVdiTraffic` (virtual desktop infrastructure), and `RtMobileGamingTraffic` (real-time mobile gaming).
* (applications) New trace sources `SourceApplication::ConnectionSucceeded` and `SourceApplication::ConnectionFailed` have been added to report connection success/failure events.
* (core) Added `SystemPath::FindSelf()` to retrieve the full path to the running executable.
* (core) Added `NormalRandomVariable::GetStdDev()` and `NormalRandomVariable::SetStdDev()` convenience functions.
* (internet) Added support for TCP FACK (Forward Acknowledgement).
* (internet) Added IPv6 support to Global Routing.
* (internet) Added `PrintRoute` functionality to `GlobalRouting` similar to `TraceRoute`.
* (internet) Added attribute `TcpLedbat::AllowedIncrease` to control the ALLOWED_INCREASE parameter defined in RFC 6817.
* (internet) A new static function `Ipv[4,6]Address::CheckCompatible()` has been added to safely check if a string can be parsed as an IPv4 or IPv6 address.
* (lr-wpan) The `McpsDataIndication` parameters now include rssi value.
* (network) Added `std::hash` specialization for `Ipv4Address` and `Ipv6Address`, enabling their use in unordered containers.
* (propagation) Added 3GPP outdoor-to-indoor (O2I) penetration losses for sub-6 GHz frequencies according to TS 38.901 Table 7.4.3-3, including vehicular O2I penetration losses.
* (visualizer) Add support to `LrWpanNetDevice` in the PyViz visualizer.

### Changes to existing API

* (applications) Common logic for starting/stopping, socket management, protocol TypeId, and TX/RX trace sources has been refactored into the `SourceApplication` and `SinkApplication` base classes.
* (applications) The trace source `UdpServer::Rx` has a changed trace signature as a result of the move of this trace to the `SinkApplication` class.  Client code using this trace can either update to the new `Rx` trace signature or instead use the same trace sink function or method with the `SinkApplication::RxWithoutAddress` trace.
* (core) The `nstime.h` header no longer includes `event-id.h`. It is possible that code might fail to compile due to this change. Adding an include to `event-id.h` fixes the issue.
* (internet) Initializing a Ipv[4,6]Address from a string using the constructor or the `Set` function will result in a crash if the string can not be parsed as an IPv4 or IPv6 address.
* (internet) The `Ipv[4,6]Address::IsInitialized()` function has been deprecated and returns always `true`. The default value of Ipv4Address created with the constructor that takes no arguments is 0.0.0.0 (previously, it was 102.102.102.102), and an Ipv4Address instance can be checked against that unspecified address value (or use std::optional to denote an address that has not been set yet).
* (internet) The function `Ipv4InterfaceAddress::SetBroadcast` has been removed from the codebase because the broadcast address must be built from the IP address and mask.
* (internet-apps) The DHCPv6 model has been refactored, and now supports all the DUID types.
* (lr-wpan) Renamed `lr-wpan-pd-plme-sap-test` to `lr-wpan-phy-test` to improve clarity. Also this testsuite now includes an additional test called `LrWpanPhyCollisionTestCase`.
* (lr-wpan) The PHY now includes the processing of the preamble as well as registering the RSSI at the moment of receiving the packet.
* (network) Address type 0 was previously used as a wildcard. Type 0 is now disallowd for any practical use. In order to create an Address from raw bytes, you must now set the Address type beforehand. An example is in ArpHeader::Deserialize.
* (network) The address class comparison is now based on std::strong_ordering operator<=> comparison operator.
* (network) An empty (uninitialized) Address is now printed as "00-00:00".
* (visualizer) The namespace `visualizer` is now used for PyViz related classes.
* (visualizer) `PyViz` has been reformatted and modernized to current ns-3 standards.

### Changes to build system

* A new `NS3_CLANG_TIDY_FIX`/`--enable-clang-tidy-fix` option was added to use clang-tidy
static analysis and automatically apply fixes during build. Not every clang-tidy warning/error
has automatic fixes, and checks are performed only at build time. Thus requiring rebuilding
things to re-run checks. As an alternative, one can use `./ns3 run clang-tidy`. And to apply
fixes, use `./ns3 run "clang-tidy -fix"`.

### Changed behavior

* (wifi) `CcaEdThreshold` can be changed at run-time.
* (internet) Updated `TcpLedbat::CongestionAvoidance()` to compute `max_cwnd` as `flightsize + AllowedIncrease * MSS` by adding `m_allowedIncrease * tcb->m_segmentSize`, in accordance with RFC 6817.
* (internet) The default value of Ipv4Address created with the constructor that takes no arguments is 0.0.0.0 (previously, it was 102.102.102.102).
* (spectrum) The `UpdatePeriod` attribute of `ThreeGppChannelModel` previously enabled periodic i.i.d. updates of the channel model, but is now used to control spatially-consistent updates of the channel.
* (propagation) Fixed 3GPP low and high O2I loss distributions where `NormalRandomVariable` variance was incorrectly set with standard deviation values from TS 38.901 Table 7.4.3-2, resulting in lower than expected losses.
* (wifi) Fixed spurious CCA-BUSY notification after channel width has changed.
* (wifi) Fixed misuse of TX power and power level in `WifiRemoteStationManager`.

## Changes from ns-3.46 to ns-3.46.1

The ns-3.46.1 contains some small build system fixes discovered after the ns-3.46 release, and two
new module documentation chapters (see [RELEASE_NOTES.md](RELEASE_NOTES.md)). There are no API
changes, changes to how the build system works, or changed behavior of the models, compared with
the ns-3.46 release.

## Changes from ns-3.45 to ns-3.46

### New API

* (spectrum) Added the base `WraparoundModel`, which is retrieved from the associated objects of a spectrum channel, and then used to wraparound the transmitter mobility model during transmissions.
* (spectrum) Added the `HexagonalWraparoundModel`, which implements the wraparound for a hexagonal cell deployment, typical of cellular networks.
* (wifi) Added a `StaticWifiSetupHelper` to (optionally) setup Wi-Fi devices (associations, block ack agreements, EMLSR mode) at simulation start time, without actually exchanging management frames over the air.
to DisableScanning` attribute to `StaWifiMac` to disable channel scanning; it can be useful to set this attribute to false when a static setup is performed.
* (wifi) Added a new `EarlyTxopEndDetect` attribute to `EhtFrameExchangeManager` to control whether the Duration/ID value of the frame being transmitted or received by a device shall be used to early detect the end of an ongoing TXOP (held by another device).
* (wifi) Added a new `DisableScanning` attribute to `StaWifiMac` to disable channel scanning; it can be useful to set this attribute to false when a static setup is performed.
* (tcp) Added new attributes ``TcpSocketBase::UseAbe``, ``TcpCubic::BetaEcn``, ``TcpNewReno::BetaEcn``, ``TcpLinuxReno::BetaLoss``, ``TcpNewReno::BetaLoss``, ``TcpLinuxReno::BetaLoss`` and  to implement the Alternative Backoff with ECN (ABE) mechanism for NewReno and CUBIC. This mechanism is specified by RFC 8511.

### Changes to existing API

The wimax module was removed and moved to the ns-3 App Store.

* (antenna) Reformatted documentation.
* (documentation) Improve models documentation look and feel.
* (internet) Added check for longest prefix match in GlobalRouting.
* (lr-wpan) Debloat MAC PD-DATA.indication and reduce packet copies.
* (zigbee) Added group table.
* (zigbee) Added Groupcast (Multicast) support.
* (zigbee) Added basic APS layer support.
* (mobility) Mobility models now implement a Copy() function.
* (propagation) MatrixPropagationLossModels are now associated per NodeId, and not per mobility model objects.
* (propagation) Removed ThreeGppPropagationLossModel prologue function, added temporarily for ns-3.45 wraparound support.
* (spectrum) MobilityModels associated to devices are not passed to receptor in SpectrumChannel. A virtual mobility model object, created from a copy and subject to wraparound is passed in its place.
* (spectrum) SpectrumSignalParameters now includes an entry for the transmitter virtual mobility model.
* (zigbee) Several callback type definitions were moved from global to class scope, for the ZigbeeAps and ZigbeeNwk classes.

### Changes to build system

* Raised minimum C++ version from C++20 to C++23.
* Raised minimum CMake version from 3.13 to 3.20.
* Raised minimum Clang version from 11 to 17.
* Raised minimum GCC version from 10.1 to 11.0.
* Added support for ClangCL compiler (part of MSVC tools).
* Added ``GENERATE_EXPORT_HEADER`` flag to the CMake ``build_lib`` macro. It will generate a ``ns3/module-export.h`` header file, which defines a C++ macro``MODULE_EXPORT``. The macro must be used when declaring a class with static member variables, or a global variable exported by a library.

### Changed behavior

* (internet) ArpCache::Flush() and NdiscCache::Flush() no longer remove autogenerated entries.
* (internet) The Ipv[4,6]RawSocket now reflects the Linux implementation, meaning that fragmented packets are reassembled (fragments are not anymore received by the socket), and packets that are simply forwarded are not received by the socket either (fixes #809).
* (wifi) WiFi backoffs are now reset only if PHY went to sleep or off for more than a threshold, improving performance and compliance with standards.

## Changes from ns-3.44 to ns-3.45

### New API

* (network) Added a function to detect IPv4 APIPA addresses (169.254.0.0/16).
* (wifi) Added a new `AssocType` attribute to `StaWifiMac` to configure the type of association performed by a device, provided that it is supported by the standard configured for the device. By using this attribute, it is possible for an EHT single-link device to perform ML setup with an AP MLD and for an EHT multi-link device to perform legacy association with an AP MLD.
* (wifi) Added a new attribute `Per20CcaSensitivityThreshold` to `EhtConfiguration` for tuning the Per 20MHz CCA threshold when 802.11be is used.
* (wifi) Added a new `MaxRadioBw` attribute to `WifiPhy` to configure the maximum width supported by the radio.
* (wifi) Added a new attribute (`KeepMainPhyAfterDlTxop`) to the `AdvancedEmlsrManager` to control whether, after the end of a DL TXOP carried out on an aux PHY link, the main PHY shall stay on that link (for a switch main PHY back delay) in the attempt to gain an UL TXOP. This attribute is applicable to the case in which aux PHYs are not TX capable and do not switch link.

### Changes to existing API

* (core) ``Object::GetInstanceTypeId()`` can no longer be specialized by subclasses and any such subclass API should be deleted (the base class will handle it).
* (dsr) Reformatted documentation and added a new concept figure.
* (flow-monitor) Reformatted documentation and added a new concept figure.
* (internet-apps) Added a parameter to the RADVD helper to announce a prefix without the autoconfiguration flag.
* (internet-apps) Added `DhcpV6` application support.
* (lr-wpan) - Renamed example ``lr-wpan\examples\lr-wpan-mlme.cc`` to ``lr-wpan\examples\lr-wpan-beacon-mode.cc``.
* (lr-wpan) - Update correct use of extended addresses in ``lr-wpan\examples\lr-wpan-data.cc``.
* (wifi) Callbacks connected to the `WifiMac::IcfDropReason` trace source are now passed a `struct IcfDropInfo` object that has three fields indicating the reason for dropping the ICF, the ID of the link on which the ICF was dropped and the MAC address of the sender of the ICF.
* (wifi) Obsoleted the `Support40MHzOperation` and `Support160MHzOperation` attributes from the HT/VHT configurations. These capabilities are now directly derived from the `ChannelSettings` attribute.
* (wifi) The `EmlsrSwitchMainPhyBackTrace` has been extended to provide the time elapsed since the switch main PHY back timer started, the reason why the main PHY switches back before the expiration of the switch main PHY back timer and whether the main PHY is switching while it is requested to switch back.

### Changes to build system

* The ns-allinone release has been redesigned; it no longer includes ``netanim`` or ``bake`` but instead includes ns-3 plus compatible contributed modules.

### Changed behavior

* (docs) Models documentation format guidelines have been updated.
* (zigbee) Adjust pedantic link cost requirement in ``NeighborTable::LookUpForBestParent``, a minimum link cost of 3 is not required now.
* (wifi) Normal Ack, BlockAck and BlockAckReq frames are transmitted, if appropriate, as non-HT duplicate PPDUs on a bandwidth matching that of the data frame transmitted in the same frame exchange sequence.

## Changes from ns-3.43 to ns-3.44

### New API

* (antenna) Add `SymmetricAdjacencyMatrix` utility class, used to track the necessity of channel update between every `PhasedArrayModel` pair.
* (applications) Added two new base classes for source and sink applications, `SourceApplication` and `SinkApplication`, respectively.
* (applications) Added an `OnOffState` trace source to `OnOffApplication`, to track whether the application is transmitting or not.
* (wifi) Added a new **RobustAVStreamingSupported** attribute to `WifiMac` to enable 802.11aa features (GCR).
* (wifi) Changes have been made to the `WifiRemoteStationManager` interface for what concerns the update of the frame retry count of the MPDUs and the decision of dropping MPDUs (possibly based on the max retry limit). The `NeedRetransmission` method has been replaced by the `GetMpdusToDropOnTxFailure` method and the `DoNeedRetransmission` method has been replaced by the `DoGetMpdusToDropOnTxFailure` method. Also, the `DoIncrementRetryCountOnTxFailure` method has been added to implement custom policies for the update of the frame retry count of MPDUs upon transmission failure.
* (zigbee) Added Zigbee module support. The module includes a NWK layer with joining and routing capabilities. No APS layer included.

### Changes to existing API

* (applications) Deprecated attributes `RemoteAddress` and `RemotePort` in UdpClient, UdpTraceClient and UdpEchoClient. They have been combined into a single `Remote` attribute.
* (applications) Deprecated attributes `ThreeGppHttpClient::RemoteServerAddress` and `ThreeGppHttpClient::RemoteServerPort`. They have been combined into a single `ThreeGppHttpClient::Remote` attribute.
* (core) Deprecated `SUPPORTED`, `DEPRECATED` and `OBSOLETE` in `TypeId` class. They have been replaced by `SupportLevel::{SUPPORTED,DEPRECATED,OBSOLETE}`, respectively.
* (lr-wpan) ``LrWpanMac`` is now also aggregated to ``LrWpanNetDevice``.
* (stats) Deprecated ns3::NaN and ns3::isNaN to use std::nan and std::isnan in their place
* (tap-bridge) Deprecated "Gateway" attribute.
* (tap-bridge) Removed unused gateway option from tap-creator.
* (wifi) Added a new **ProtectedIfResponded** attribute to `FrameExchangeManager` to disable RTS/CTS protection for stations that have already responded to a frame requiring acknowledgment in the same TXOP, even if such frame had not been protected by RTS/CTS. The default value is true, even though it represents a change with respect to the previous behavior, because it is likely a more realistic choice.
* (wifi) Deprecated setters/getters of the {Ht,Vht,He}Configuration classes that trivially set/get member variables, which have been made public and hence accessible to users.
* (wifi) Trace source `QosTxop::BaEstablished` is extended to support GCR block ack agreements.

### Changes to build system

* !2319 Add support for clang-tidy-19
* !2251 Add msbuild and Visual Studio generator support to the ns3 script
* !2251 Use ccache with ClangCL/MSVC
* !2251 Provide option to generate export header from build_lib
* !2260 Scan for modules in the external contrib directory ../ns-3-external-contrib
* !2255 Exclude external imported targets from missing libraries check
* !2238 Prevent Python bindings from being enabled along with MPI

### Changed behavior

* (lr-wpan) !2334 Association: Fix the handling of situations where the association response commands arrives before the data request command acknowledgment that is supposed to precede it.

## Changes from ns-3.42 to ns-3.43

### New API

* (applications) A new trace source `BulkSendApplication::TcpRetransmission` has been added for tracing TCP retransmissions.
* (core) Added `LaplacianRandomVariable` class implementing the Laplacian random variable, and `LargestExtremeValueRandomVariable` class implementing the Largest Extreme Value random variable.
* (lr-wpan) Added a new test to `lr-wpan-cca-test.cc` suite. The added test demonstrates a known CCA vulnerability window.
* (tcp) A new trace source `TcpSocketBase::LastRtt` has been added for tracing the last RTT sample observed. The existing trace source `TcpSocketBase::Rtt` is still providing the smoothed RTT, although it had been incorrectly documented as providing the last RTT.
* (wifi) Added a new trace source to `WifiPhy`: **PhyRxMacHeaderEnd**, which is fired when the reception of the MAC header of an MPDU is completed and provides the MAC header and the remaining PSDU duration. The trace source is actually fired when the new **NotifyMacHdrRxEnd** attribute of `WifiPhy` is set to true (it is set to false by default).
* (wifi) WifiHelper::SetStandard() method now accepts selected string values in addition to enum argument.
* (wifi) Added a new method **SetPcapCaptureType** to `WifiPhyHelper` to control how PCAPs are generated for MLD devices.
* (wifi) New trace helper `WifiTxStatsHelper` for providing Wi-Fi MAC-level transmission statistics.

### Changes to existing API

* (core) Deprecated struct `TypeTraits`. Functionality provided by the standard library header `<type_traits>` should be used instead.
* (core) Add `AddDeprecatedName` to TypeId. This allows for TypeIds to transition to name TypeIds that use namespaces while still supporting the old name.
* (energy) Energy module TypeId now uses the name that includes the namespace `ns3::energy`, the old name is now deprecated.
* (energy) Documentation was extended and reformatted.
* (lr-wpan) Lr-wpan module TypeId now uses the name that includes the namespace `ns3::lrwpan`, the old name is now deprecated.
* (lr-wpan) Attribute `macBeaconPayload` in `MacPibAttributes` is now a `std::vector<uint8_t>` instead of a packet pointer.
* (lr-wpan) Removes the word `address` from the MAC address prefix when `LOG_PREFIX_FUNC` is used.
* (lr-wpan) Removes the word `address` from the CSMA-CA logs prefix when `LOG_PREFIX_FUNC` is used.
* (lr-wpan) Added `AssignStreams` function to the MAC.
* (lr-wpan) Attribute `macRxOnWhenIdle` added to the supported attributes in `MacPibAttributes`.
* (lr-wpan) Attribute `macPromiscuousMode` added to the supported attributes in `MacPibAttributes`.
* (lr-wpan) Attribute `macAssociatePermit` added to the supported attributes in `MacPibAttributes`.
* (lr-wpan) Attribute `pCurrentChannel` added to the supported attributes in `MacPibAttributes`.
* (lr-wpan) Attribute `pCurrentPage` added to the supported attributes in `MacPibAttributes`.
* (lr-wpan) Documentation was extended and reformatted.
* (wifi) The `WifiHelper::AssignStreams()` method has been made static.
* (wifi) Attribute `ChannelSettings` has been changed to allow configuration of non-contiguous operating channels by specifying each 80 MHz segment. It has changed from TupleValue to AttributeContainerValue, but the configuration of contiguous channels using a StringValue still works as before.

### Changes to build system

* Module libraries targets names have their "lib" prefixes removed. This affects target selection within IDEs and ns-3 importing via CMake.

### Changed behavior

* (lr-wpan) Beacons are now transmitted using CSMA-CA when requested from a beacon request command.
* (lr-wpan) Upon a beacon request command, beacons are transmitted after a jitter to reduce the probability of collisions.
* (tcp) TCP Proportional Rate Reduction (PRR) recovery has been aligned to the updates in draft-ietf-tcpm-prr-rfc6937bis

## Changes from ns-3.41 to ns-3.42

### New API

* (antenna) Added `CircularApertureAntennaModel` class which characterizes the antenna gain pattern of the reflector antenna with circular aperture described in 3GPP TR 38.811 v15.4.0, Section 6.4.1
* (core) Objects now can be aggregated to multiple objects though the `Object::UnidirectionalAggregateObject` function. Objects aggregated in such a way can not use `GetObject` to access the objects they are aggregated to.
* (core) Added `TestVector` iterators and dot product operator for `Vector2D` and `Vector3D` types
* (mobility) Added a new mobility model `GeocentricConstantPositionMobilityModel` for orbital and/or aerial nodes, and coordinate conversion methods between geocentric and topocentric coordinate systems
* (propagation, spectrum)  Added 3GPP 38.811 Non-Terrestrial Networks (NTNs) channel model. Specifically, the large-scale phenomena have been implemented by extending `ThreeGppPropagationLossModel` with classes representing the various NTN propagation scenarios  (Dense Urban, Urban, Rural and Suburban), while the frequency-dependent phenomena have been implemented by defining the corresponding scenarios in `ThreeGppChannelModel`.
* (network) Added `ApplicationHelper` helper class to create and install applications, removing redundant code in existing helpers and reducing the burden to add yet another helper when a new application model is added.
* (wifi) Added a new **SingleRtsPerTxop** attribute to `WifiDefaultProtectionManager`, which, if set to true, prevents to use protection mechanisms (RTS or MU-RTS) more than once in a TXOP (unless required for specific purposes, such as transmitting an Initial Control Frame to an EMLSR client).
* (wifi) Added a new **RtsCtsTxDurationThresh** to `WifiRemoteStationManager` to enable RTS/CTS protection based on the TX duration of the data frame. Both the value of this attribute and the value of the existing **RtsCtsThreshold** attribute are evaluated: if either of the thresholds (or both) is exceeded, RTS/CTS is used.
* (wifi) New trace helper `WifiPhyRxTraceHelper` for detailed tracing of Wi-Fi Phy reception events
* (wifi) New trace sources `WifiPhy::SignalTransmission`, `SpectrumWifiPhy::SignalArrival`, and `YansWifiPhy::SignalArrival`
* (wifi) New trace sources `WifiPhyStateHelper::RxOutcome` and`WifiPhy::PhyRxPpduDrop`, to support additional tracing.

### Changes to existing API

* (applications) Applications have a new Attribute to set the IPv4 ToS field.
* (core) Deprecated enum `TestDuration` in `TestCase` class. It has been replaced by enum class `Duration`.
* (core) In `TestSuite` class, deprecated `ALL`, `UNIT`, `SYSTEM`, `EXAMPLE` and `PERFORMANCE`. They have been replaced by `Type::ALL`, `Type::UNIT`, `Type::SYSTEM`, `Type::EXAMPLE` and `Type::PERFORMANCE`, respectively.
* (core) Deprecated `EventId::IsRunning()`. It has been replaced with `EventId::IsPending()`.
* (energy) The model library code of the energy module now uses the nested namespace `energy`.
* (lr-wpan) `MacPibAttributeIdentifier` attribute ids are now standard compliant.
* (lr-wpan) Multiple new identifiers added to `MacPibAttributeIdentifier`.
* (lr-wpan) Adds standard version comments to `MLME-GET.request` function.
* (lr-wpan) In the MAC layer, renamed `m_selfExt` to the variable `m_macExtendedAddress` to make it consistent with the standard specification.
* (lr-wpan) The Lr-wpan module now uses the namespace `lrwpan`.
* (lr-wpan) The model library code of the lr-wpan module now uses the nested namespace `lrwpan`.
* (lr-wpan) The `LrWpan` prefix of variables, structs and enumerations in the PHY and MAC was shorten to reflect the recent namespace change.
* (wifi) Deprecated `WIFI_TID_TO_LINK_MAPPING_{NOT_SUPPORTED,SAME_LINK_SET,ANY_LINK_SET}`. They have been replaced by `WifiTidToLinkMappingNegSupport::{NOT_SUPPORTED,SAME_LINK_SET,ANY_LINK_SET}`, respectively.
* (wifi) Deprecated `{IDLE, CCA_BUSY, TX, RX, SWITCHING, SLEEP, OFF}`. They have been replaced by `WifiPhyState::{IDLE, CCA_BUSY, TX, RX, SWITCHING, SLEEP, OFF}`, respectively.
* (wifi) Obsoleted **Txop** attributes `MinCw`, `MaxCw`, `Aifsn` and `TxopLimit`. The corresponding attributes for multi-link devices (`MinCws`, `MaxCws`, `Aifsns` and `TxopLimits`) can be used instead.

### Changes to build system

* Removed support of the `experimental/filesystem` library, in favor of the official `filesystem` library.
* Fixed static and monolib builds when linking to a non ns-3 module library.

### Changed behavior

* (mobility) Fixed the corner rebound direction in `RandomWalk2d[Outdoor]MobilityModel` and the initial direction in case of node starting from a border or corner.
* (tcp) TcpCubic and TcpLinuxReno will no longer grow their congestion window when application-limited, now matching Linux behavior

## Changes from ns-3.40 to ns-3.41

### New API

* (core) Added `BernoulliRandomVariable` class implementing the bernoulli random variable, and `BinomialRandomVariable` class implementing the binomial random variable.
* (core) Added wrapper around `UniformRandomVariable` to meet the C++11 requirements of UniformRandomBitGenerator.
* (core) Added method to `GetStopEvent()` from `Simulator`
* (internet) It is now possible to set the TOS field for IPv4 ICMP Echo Requests/Responses, via a new `Tos` attribute.
* (spectrum) `SpectrumSignalParameters` is extended to include two new members called: `spectrumChannelMatrix` and `precodingMatrix` which are the key information needed to support MIMO simulations.
* (spectrum) `SpectrumChannel::AssignStreams()` is added, to allow random variable stream assignment for all propagation loss and delay models added to the channel.
* (wifi) Added new attribute `ChannelAccessManager:GenerateBackoffIfTxopWithoutTx` to invoke the backoff procedure when an AC gains the right to start a TXOP but it does not transmit any frame, provided that the queue is not actually empty. No transmission may occur,e.g., due to constraints associated with EMLSR operations. This possibility is specified by the current draft revision of the IEEE 802.11 standard.

### Changes to existing API

* (antenna) `UniformPlannarArray` has new attributes `NumVerticalPorts`, `NumHorizontalPorts`, and `IsDualPolarized`.
* (antenna) `GetNumberOfElements` is renamed to `GetNumElems` for the sake of simplifying the long lines of code that use complex mathematical expressions.
* (core) The EnumValue class now also supports enum class in addition to plain enums.  As a result of this change, attributes that wrap enums must update the syntax for the `MakeEnumAccessor` method call. Where you once were able to write, for example (from attribute-test-suite.cc):

  ```cpp
  MakeEnumAccessor(&AttributeObjectTest::m_enum),
  ```

  you must now write it with a template parameter such as:

  ```cpp
  MakeEnumAccessor<Test_e>(&AttributeObjectTest::m_enum),
  ```

* (internet) Deprecated `Ipv4::WeakEsModel` and `Ipv4::GetWeakEsModel()`, `Ipv4::SetWeakEsModel(bool)` methods. Moved `Ipv6L3Protocol::StrongEndSystemModel` to `Ipv6::StrongEndSystemModel` and added `Ipv4::StrongEndSystemModel` with corresponding `GetStrongEndSystemModel()` and `SetStrongEndSystemModel(bool)` methods to improve end system model configuration options.
* (lr-wpan) Change the CapabilityField parameter in `LrWpanMac::MlmeAssociateRequest` and `LrWpanMac::MlmeAssociateIndication` to a standard bitmap.
* (lr-wpan) Change the MAC SuperframeField usage to a standard bitmap, this change impact parameters in the `BeaconPayloadHeader`.
* (lr-wpan) Create a new abstract class that defines the form of any Lr-wpan MAC layers (`LrWpanMacBase`).
* (lr-wpan) Add the capability to see the enum values of the MAC transition states in log prints for easier debugging.
* (lr-wpan) Group MAC status enumerations into a single `LrWpanMacStatus` enumeration in `lr-wpan-mac-base.h.`
* The spelling of the following files, classes, functions, constants, defines and enumerated values was corrected; this will affect existing users who were using them with the misspelling.
  * (lte) Struct member `fdbetsFlowPerf_t::lastTtiBytesTrasmitted` in file `fdbet-ff-mac-scheduler.h` was renamed `fdbetsFlowPerf_t::lastTtiBytesTransmitted`.
  * (lte) Struct member `tdbetsFlowPerf_t::lastTtiBytesTrasmitted` in file `tdbet-ff-mac-scheduler.h` was renamed `fdbetsFlowPerf_t::lastTtiBytesTransmitted`.
  * (lte) Struct member `pfsFlowPerf_t::lastTtiBytesTrasmitted` in file `pf-ff-mac-scheduler.h` was renamed `fdbetsFlowPerf_t::lastTtiBytesTransmitted`.
* (sixlowpan) Remove `ForceEtherType` and `EtherType` attributes, and use RFC 7973 EtherType for interfaces supporting an EtherType.
* (spectrum) `PhasedArraySpectrumPropagationLossModel::CalcRxPowerSpectralDensity` return type is changed from `Ptr<SpectrumValue>` to `Ptr<SpectrumSignalParameters>` to support MIMO, because when multiple transmit and receive antenna ports are present, it is not enough to have a single PSD (represented by `Ptr<SpectrumValue>`) but also the 3D channel matrix is needed per receive and transmit antenna port. Notice that `CalcRxPowerSpectralDensity` is typically called from within `MultiModelSpectrumChannel`, but if some external ns-3 module is calling directly this function, it can still access to its original return value through `Ptr<SpectrumSignalParameters>` which contains `Ptr<SpectrumValue>`.
* (wifi) The `HeConfiguration::MpduBufferSize` attribute is now obsolete. Use the `WifiMac::MpduBufferSize` attribute instead.
* (wifi) The `LinkSetupCanceled` trace source of `StaWifiMac` has been obsoleted because disassociation does not occur at link level for non-AP MLDs.
* (wifi) The default value for `WifiRemoteStationManager::RtsCtsThreshold` has been increased from 65535 to 4692480.
* (wifi) `SpectrumWifiHelper::SpectrumChannelSwitched()` is now static

### Changes to build system

* In preparation to enable C++20, the following actions have been taken due to compiler issues:
  * Precompiled headers have been disabled in GCC versions >= 12.2.
  * The `restrict` warning has been disabled in GCC versions 12.1-12.3.1.
* Raised minimum CMake version to 3.13.
* Raised minimum C++ version to C++20.
* Added guard rails for scratch targets missing or containing more than one `main` function.

### Changed behavior

* (sixlowpan) Now uses RFC 7973 Ethertype by default
* (spectrum) SpectrumChannel objects and the loss/delay models attached are now automatically initialized (Object::Initialize) at time zero
* (tcp) TCP Cubic (the default congestion control in ns-3) now supports TCP-friendliness by default (see RFC 9438 Section 4.3), making the congestion window growth somewhat more aggressive.  This follows the default Linux behavior.
* (wifi) Increase the duration of the timer started when waiting for an ADDBA_RESPONSE from 1ms to 5ms to better account for the time required by the recipient to access the medium and complete the frame exchange (which may involve protection with (MU-)RTS/CTS).

## Changes from ns-3.39 to ns-3.40

### New API

* (energy) Added `GenericBatteryModel` to the energy module with working examples, and support for battery presets and cell packs.
* (lr-wpan) Added the functions to set or get the capability field via bitmap (a 8 bit int).
* (lr-wpan) Added the possibility to obtain the LQI from a received `MlmeAssociateIndicationParams`.
* (wifi) Added new helper methods to SpectrumWifiPhyHelper to allow flexible configuration for the mapping between spectrum PHY interfaces and PHY instances.
* (wifi) Added new trace sources to `WifiPhy`: **OperatingChannelChange**, which is fired when the operating channel of a PHY is changed.
* (wifi) The attribute `WifiPhy::Antennas` is extended to support up to 8 antennas.

### Changes to existing API

* (core) Removed private class `EmpiricalRandomVariable::ValueCDF` in favor of `std::map`.
* (lr-wpan) Removed unnecessary bcst filter from `LrWpanMac::PdDataIndication` which also blocked the correct reception of beacon request commands.
* (wifi) The attribute `WifiPhy::Antennas` is extended to support up to 8 antennas.
* (wifi) `StaWifiMac::MacState` enum is now public, and `WifiMacHeader` can be subclassed

### Changes to build system

* Added support for Vcpkg and CPM package managers

### Changed behavior

* (core) `EmpiricalRandomVariable` CDF pairs can now be added in any order.
* (core) `EmpiricalRandomVariable` no longer requires that a CDF pair with a range value exactly equal to 1.0 be added (see issue #922).
* (wifi) Upon ML setup, a non-AP MLD updates the IDs of the setup links to match the IDs used by the AP MLD.
* (wifi) Attribute **TrackSignalsFromInactiveInterfaces** in SpectrumWifiPhy has been defaulted to be enabled.

## Changes from ns-3.38 to ns-3.39

### New API

* (lr-wpan) Added support for orphan scans. Orphan scans can now be performed using the existing `LrWpanMac::MlmeScanRequest`; This orphan scan use the added orphan notification commands and coordinator realignment commands. Usage is shown in added `lr-wpan-orphan-scan.cc` example and in the `TestOrphanScan` included in `lr-wpan-mac-test.cc`.
* (network) Added `Mac64Address::ConvertToInt`. Converts a Mac64Address object to a uint64_t.
* (network) Added `Mac16Address::ConvertToInt`. Converts a Mac16Address object to a uint16_t.
* (network) Added `Mac16Address::Mac16Address(uint16t addr)` and `Mac16Address::Mac64Address(uint64t addr)` constructors.
* (lr-wpan) Added `LrwpanMac::MlmeGetRequest` function and the corresponding confirm callbacks as well as `LrwpanMac::SetMlmeGetConfirm` function.
* (applications) Added `Tx` and `TxWithAddresses` trace sources in `UdpClient`.
* (spectrum) Added `SpectrumTransmitFilter` class and the ability to add them to `SpectrumChannel` instances.
* (stats) Added `Histogram::Clear` function to clear the histogram contents.
* (wifi) Added `WifiBandwidthFilter` class to allow filtering of out-of-band Wi-Fi signals.
* (flow-monitor) Added `FlowMonitor::ResetAllStats` function to reset the FlowMonitor statistics.

### Changes to existing API

* The spelling of the following files, classes, functions, constants, defines and enumerated values was corrected; this will affect existing users who were using them with the misspelling.
  * (dsr) Class `DsrOptionRerrUnsupportHeader` from `dsr-option-header.h` was renamed `DsrOptionRerrUnsupportedHeader`.
  * (internet) Enumerated value `IPV6_EXT_AUTHENTIFICATION` from `ipv6-header.h` was renamed `IPV6_EXT_AUTHENTICATION`.
  * (lr-wpan) Constant `aMaxBeaconPayloadLenght` from `lr-wpan-constants.h` was renamed `aMaxBeaconPayloadLength`.
  * (lte) Enumeration `ControPduType_t` from `lte-rlc-am-header.h` was renamed `ControlPduType_t`.
  * (lte) Function `LteUeCphySapProvider::StartInSnycDetection()` from `lte-ue-cphy-sap.h` was renamed `LteUeCphySapProvider::StartInSyncDetection()`.
  * (lte) Function `MemberLteUeCphySapProvider::StartInSnycDetection()` from `lte-ue-cphy-sap.h` was renamed `MemberLteUeCphySapProvider::StartInSyncDetection()`.
  * (lte) Function `LteUePhy::StartInSnycDetection()` from `lte-ue-phy.h` was renamed `LteUePhy::StartInSyncDetection()`.
  * (lte) Function `DoUlInfoListElementHarqFeeback` from `lte-enb-phy-sap.h` and `lte-enb-mac.h` was renamed `DoUlInfoListElementHarqFeedback`.
  * (lte) Function `DoDlInfoListElementHarqFeeback` from `lte-enb-phy-sap.h` and `lte-enb-mac.h` was renamed `DoDlInfoListElementHarqFeedback`.
  * (mesh) Function `PeerManagementProtocolMac::SetPeerManagerProtcol` from `peer-management-protocol-mac.h` was renamed `PeerManagementProtocolMac::SetPeerManagerProtocol`.
  * (network) File `lollipop-comparisions.cc` was renamed `lollipop-comparisons.cc`.
  * (network) Attribute `currentTrimedFromStart` from `packet-metadata.h` was renamed `currentTrimmedFromStart`.
  * (network) Attribute `currentTrimedFromEnd` from `packet-metadata.h` was renamed `currentTrimmedFromEnd`.
  * (sixlowpan) Function `SixLowPanNetDevice::Fragments::GetFraments` from `sixlowpan-net-device.cc` was renamed `SixLowPanNetDevice::Fragments::GetFragments`.
  * (wave) Function `OcbWifiMac::CancleTx()` from `ocb-wifi-mac.h` was renamed `OcbWifiMac::CancelTx()`.
  * (wifi) Attribute `m_succesMax1` from `aparf-wifi-manager.h` was renamed `m_successMax1`.
  * (wifi) Attribute `m_succesMax2` from `aparf-wifi-manager.h` was renamed `m_successMax2`.
  * (wifi) Enumerated value `MDAOP_ADVERTISMENT_REQUEST` from `mgmt-headers.h` was renamed `MDAOP_ADVERTISEMENT_REQUEST`.
  * (wifi) Enumerated value `MDAOP_ADVERTISMENTS` from `mgmt-headers.h` was renamed `MDAOP_ADVERTISEMENTS`.
  * (wifi) Define `IE_BEAMLINK_MAINENANCE` from `wifi-information-element.h` was renamed `IE_BEAMLINK_MAINTENANCE`.
  * (wimax) Attribute `m_nrRecivedFecBlocks` from `simple-ofdm-wimax-phy.h` was renamed `m_nrReceivedFecBlocks`.
* (lr-wpan) Updated `LrWpanPhy::PlmeSetAttribute` and `LrWpanPhy::PlmeGetAttribute` (Request and Confirm) to use smart pointers.
* (lr-wpan) Modified `LrWpanPhy::PlmeGetAttributeRequest` to include support for a few attributes (none were supported before the change).
* (lr-wpan) Added `macShortAddress`, `macExtendedAddress` and `macPanId` to the attributes that can be use with MLME-GET and MLME-SET functions.
* (wifi) The QosBlockedDestinations class has been removed and its functionality is now provided via a new framework for blocking/unblocking packets that is based on the queue scheduler.
* (internet) The function signature of `Ipv4RoutingProtocol::RouteInput` and `Ipv6RoutingProtocol::RouteInput` have changed. The `UnicastForwardCallback` (ucb), `MulticastForwardCallback` (mcb), `LocalDeliverCallback` (lcb) and `ErrorCallback` (ecb) should now be passed as const references.
* (olsr) The defines `OLSR_WILL_*` have been replaced by enum `Willingness`.
* (olsr) The defines `OLSR_*_LINK` have been replaced by enum `LinkType`.
* (olsr) The defines `OLSR_*_NEIGH` have been replaced by enum `NeighborType`.
* (wifi) The `WifiCodeRate` typedef was converted to an enum.
* (internet) `InternetStackHelper` can be now used on nodes with an `InternetStack` already installed (it will not install IPv[4,6] twice).
* (lr-wpan) Block the reception of orphan notification commands to devices other than PAN coordinators or coordinators.
* (lr-wpan) Block the reception of broadcast messages in the same device that issues it. This is done in both cases when the src address is either short or extended address.
* (lr-wpan) Adds a new variable flag `m_coor` to the MAC to differentiate between coordinators and PAN coordinators.
* (lte) Add support for DC-GBR. The member `QosBearerType_e` of the structure `LogicalChannelConfigListElement_s` is extended to include DC-GBR resource type. Based on this change, the method **IsGbr** of `EpsBearer`, is renamed to **GetResourceType**. LTE code using this method, is updated according to this change.
* (lte) The `EpsBearer` is extended to include 3GPP Release 18 5QIs.
* (lte) Add PDCP discard timer. If enabled using the attribute `EnablePdcpDiscarding`, in case that the buffering time (head-of-line delay) of a packet is greater than the PDB or a value set by the user, it will perform discarding at the moment of passing the PDCP SDU to RLC.
* (lte) Centralize the constants `MIN_NO_CC` and `MAX_NO_CC`, declared in multiple header files, into the header `lte-common.h`.
* (wave) The Wave module was removed from the codebase due to lack of maintenance

### Changes to build system

### Changed behavior

* (core) The priority of `DEBUG` level logging has been lowered from just below `WARN` level to just below `LOGIC` level.
* (buildings) Calculation of the O2I Low/High Building Penetration Losses based on 3GPP 38.901 7.4.3.1 was missing. These losses are now included in the pathloss calculation when buildings are present.
* (network) The function `Buffer::Allocate` will over-provision `ALLOC_OVER_PROVISION` bytes when allocating buffers for packets. `ALLOC_OVER_PROVISION` is currently set to 100 bytes.
* (wifi) By default, the `SpectrumWifiHelper` now adds a `WifiBandwidthFilter` to discard out-of-band signals before scheduling them on the receiver.  This should not affect the simulated behavior of Wi-Fi but may speed up the execution of large Wi-Fi simulations.
* (wifi) Protection mechanisms (e.g., RTS/CTS) are not used if destinations have already received (MU-)RTS in the current TXOP
* (wifi) Protection mechanisms can be used for management frames as well (if needed)

## Changes from ns-3.37 to ns-3.38

### New API

* (core) Added new template classes `ValArray` and `MatrixArray` to represent efficiently 1D, 2D and 3D arrays. `ValArray` implements basic efficient storage of such structures and the basic operations, while `MatrixArray` allows to represent 3D arrays as arrays of mathematical matrices and invoke different mathematical operations on the arrays of matrices: multiplication, transpose, hermitian transpose, etc. `MatrixArray` can use Eigen to perform computationally complex operations.
* (core) Added several macros in **warnings.h** to silence compiler warnings in specific sections of code. Their use is discouraged, unless really necessary.
* (lr-wpan) Added beacon payload handle support (MLME-SET.request) in  **LrWpanMac**.
* (lr-wpan) `LrWpanPhy::SetRxSensitivity` now supports the setting of Rx sensitivity.
* (lr-wpan) `LrWpanNetDevice::SetPanAssociation` is introduced to create more complex topologies (multi-hop) using a manual association.
* (netanim) Added a helper function to update the size of a node
* (network) Added class `TimestampTag` for associating a timestamp with a packet.
* (spectrum) A new fast-fading model `TwoRaySpectrumPropagationLossModel` has been added. This model serves as a performance-oriented alternative to the `ThreeGppSpectrumPropagationLossModel` and `ThreeGppChannelModel` classes, and it has been designed with the goal of providing end-to-end channel samples which are statistically close to the ones generated by the latter.
* (wifi) Added a new attribute **NMaxInflights** to QosTxop to set the maximum number of links on which an MPDU can be simultaneously in-flight.
* (wifi) New API has been introduced to support 802.11be Multi-Link Operations (MLO)
* (wifi) New API has been introduced to support 802.11ax dual NAV, UL MU CS, and MU-RTS/CTS features
* (wifi) Added a new attribute **TrackSignalsFromInactiveInterfaces** to SpectrumWifiPhy to select whether it should track signals from inactive spectrum PHY interfaces.

### Changes to existing API

* (antenna, spectrum) `ComplexVector` definition has changed. Its API is implemented in `MatrixArray`. Some functions such as `push_back` and `resize` are not supported any more. On the other hand, the size initialization through constructor and access operator[] are maintained. Instead of `size ()` users can call `GetSize()`.
* (internet) TCP Westwood model has been removed due to a bug in BW estimation documented in <https://gitlab.com/nsnam/ns-3-dev/-/issues/579>. The TCP Westwood+ model is now named **TcpWestwoodPlus** and can be instantiated like all the other TCP flavors.
* (internet) `TcpCubic` attribute `HyStartDetect` changed from `int` to `enum HybridSSDetectionMode`.
* (internet-apps) Added class `Ping` for a ping model that works for both IPv4 and IPv6.  Classes `v4Ping` and `Ping6` will be deprecated and removed in the future, replaced by the new `Ping` class.
* (lr-wpan) Added file `src/lr-wpan/model/lr-wpan-constants.h` with common constants of the LR-WPAN module.
* (lr-wpan) Removed the functions `LrWpanCsmaCa::GetUnitBackoffPeriod()` and `LrWpanCsmaCa::SetUnitBackoffPeriod()`, and moved the constant `m_aUnitBackoffPeriod` to `src/lr-wpan/model/lr-wpan-constants.h`.
* (lr-wpan) `LrWpanHelper::CreateAssociatedPan` replace `LrWpanHelper::AssociateToPan` and is able to create an associated PAN of the devices with both short addresses (16-bits) and extended addresses (EUI-64 bits).
* (wifi) `SpectrumWifiPhy::SetChannel` has been renamed to `SpectrumWifiPhy::AddChannel` and has one additional parameter (optional) to indicate the frequency range that is covered by the provided spectrum channel. By default, the whole wifi spectrum channel is considered.
* The `WifiSpectrumHelper::SetChannel` functions used for MLO do no longer take a link ID parameter, but instead takes the frequency range covered by the spectrum channel and have been renamed to `WifiSpectrumHelper::AddChannel`. The remaining `WifiSpectrumHelper::SetChannel` functions assume the whole wifi spectrum range is used by the spectrum channel.

### Changes to build system

* Added NinjaTracing support.
* Check if the ccache version is equal or higher than 4.0 before enabling precompiled headers.
* Improved bindings search for linked libraries and their include directories.
* Added `./ns3 distclean` option. It removes the same build artifacts as `./ns3 clean`, along with documentation, python and test artifacts.

### Changed behavior

* (applications) **UdpClient** and **UdpEchoClient** MaxPackets attribute is aligned with other applications, in that the value zero means infinite packets.
* (network) **Ipv4Address** and **Ipv6Address** now do not raise an exception if built from an invalid string. Instead the address is marked as not initialized.
* (tests) The test runner test.py will exit if no TestSuite is specified.
* (wifi) Control frames (specifically, BlockAckRequest and MU-BAR Trigger Frames) are stored in the wifi MAC queue and no longer in a dedicated BlockAckManager queue
* (wifi) BSSIDs are no longer hashed by the ApInfo comparator because it may lead to different results on different platforms

## Changes from ns-3.36 to ns-3.37

### New API

* (internet) In `src/internet`, several changes were made to enable auto-generated neighbor caches:
  * A new helper (NeighborCacheHelper) was added to set up auto-generated neighbor cache.
  * New NUD_STATE `STATIC_AUTOGENERATED`  was added to help the user manage auto-generated entries in Arp cache and Ndisc cache.
  * Add new callbacks RemoveAddressCallback and AddAddressCallback to dynamically update neighbor cache during addresses are removed/added.
  * Add NeighborCacheTestSuite to test auto-generated neighbor cache.
* (lr-wpan) Adds support for **LrWpanMac** devices association.
* (lr-wpan) Adds support for **LrWpanMac** energy detection (ED) scan.
* (lr-wpan) Adds support for **LrWpanMac** active and passive scan.
* (lr-wpan) Adds support for channel paging to the **LrWpanPhy** (only placeholder, a single modulation/band is currently supported).
* (lr-wpan) Add **LrWpanMac** packet traces and queue limits to Tx queue and Ind Tx queue.
* (propagation) Add O2I Low/High Building Penetration Losses in 3GPP propagation loss model (`ThreeGppPropagationLossModel`) according to **3GPP TR 38.901 7.4.3.1**. Currently, UMa, UMi and RMa scenarios are supported.
* (wifi) Added a new attribute **MaxTbPpduDelay** in HeConfiguration for configuring the maximum delay with which a TB PPDU can arrive at the AP after the first TB PPDU in order to be decoded properly. If the delay is higher than **MaxTbPpduDelay**, the TB PPDU is discarded and treated as interference.
* (wifi) Added new methods (**ConfigHtOptions**, **ConfigVhtOptions**, **ConfigHeOptions** and **ConfigEhtOptions**) to `WifiHelper` to configure HT/VHT/HE/EHT options listed as attributes of the respective Configuration classes through the wifi helper.
* (wifi) Added new attributes (**AccessReqInterval**, **AccessReqAc** and **DelayAccessReqUponAccess**) to the MultiUserScheduler to allow a wifi AP to coordinate UL MU transmissions even without DL traffic.
* `(wifi) WifiNetDevice` has a new **Phys** attribute, which is primarily intended to select a specific PHY object of an 11be multi-link device when using path names.
* (wifi) `Txop` class has new attributes (**MinCws**, **MaxCws**, **Aifsns** and **TxopLimits**) to set minimum CW, maximum CW, AIFSN and TXOP limit for all the links of a multi-link device.
* (wifi) `WifiPhyListener::NotifyMaybeCcaBusyStart` has been renamed to `WifiPhyListener::NotifyCcaBusyStart` and has two additional parameters: the channel type that indicates for which subchannel the CCA-BUSY is reported and a vector of CCA-BUSY durations for each 20 MHz subchannel. A duration of zero indicates CCA is IDLE, and the vector of CCA-BUSY durations is not empty if the PHY supports 802.11ax and the operational channel width is larger than 20 MHz.
* (wifi) Added a new attribute **CcaSensitivity** in WifiPhy for configuring the threshold that corresponds to the minimum received power of a PPDU, that occupies the primary channel, should have to report a CCA-BUSY indication.
* (wifi) Added a new attribute **SecondaryCcaSensitivityThresholds** in VhtConfiguration for configuring the thresholds that corresponds to the minimum received power of a PPDU, that does not occupy the primary 20 MHz channel, should have to report a CCA-BUSY indication. This is made of a tuple, where the first threshold is used for 20 MHz PPDUs, the second one is used for 40 MHz PPDUs and the third one is used for 80 MHz PPDUs.
* (wifi) Added two new trace sources to `StaWifiMac`: **LinkSetupCompleted**, which is fired when a link is setup in the context of an 11be ML setup, and **LinkSetupCanceled**, which is fired when the setup of a link is terminated. Both sources provide the ID of the setup link and the MAC address of the corresponding AP.

### Changes to existing API

* (lr-wpan) Replace **LrWpanMac** Tx Queue and Ind Tx Queue pointers for smart pointers.
* (lr-wpan) Adds supporting structures used by **LrWpanMac** (PAN descriptor, Command Payload Header, Capability Field).
* (lr-wpan) Add supporting association structures: parameters, callbacks and the pending transaction list to **LrWpanMac**.
* (wifi) The **TxopTrace** trace source of wifi `QosTxop` now has an additional argument (the third one) indicating the ID of the link the TXOP refers to (for non-MLDs, this value is zero).
* (wifi) The maximum allowed channel width (in MHz) for a data transmission is passed to the **GetDataTxVector** method of the `WifiRemoteStationManager`.
* (wifi) The **WifiMacQueueItem** class has been renamed as **WifiMpdu**.
* (wifi) The **Assoc** and **DeAssoc** trace sources of `StaWifiMac` provide the AP MLD address in case (de)association takes place between a non-AP MLD and an AP MLD.

### Changes to build system

* Replaced the Pybindgen python bindings framework with Cppyy.
* Enabled precompiled headers (`NS3_PRECOMPILE_HEADERS`) by default when CCache is found.
* Added the `./ns3 show targets` option to list buildable/runnable targets.
* Added the `./ns3 show (all)` option to list a summary of the current settings.
* Replaced `./ns3 --check-config` with `./ns3 show config`.
* Replaced `./ns3 --check-profile` with `./ns3 show profile`.
* Replaced `./ns3 --check-version` with `./ns3 show version`.
* Added the `build_exec` macro to declare new executables.
* Replaced Python-based .ns3rc with a CMake-based version.
* Deprecated .ns3rc files will be updated to the new CMake-based format and a backup will be placed alongside it.
* Added the `./ns3 configure --filter-module-examples-and-tests='module1;module2'` option, which can be used to filter out examples and tests that do not use the listed modules.
* Deprecated symlinks in the build/ directory in favor of stub headers.
* Added support for faster linkers `lld` and `mold`. These will be used if found. The order of priority is: `mold` > `lld` > default linker.
* Added support for Windows using the Msys2/MinGW64 toolchain (supports both Unix-like Bash shell shipped with Msys2 and native shells such as CMD and PowerShell).
* Added new `./ns3 run` options for profilers: `--memray` and `--heaptrack` for memory profiling of Python scripts and C++ programs, respectively, and `--perf` for performance profiling on Linux.

### Changed behavior

* (internet) IPv6 Router Solicitations (RS) are now retransmitted up to 4 times, following RFC 5779.
* (lr-wpan) **LrWpanPhy** now change to TRX_OFF after a CSMA-CA failure when the RxOnWhenIdle flag is set to false in the **LrWpanMac**.
* (lr-wpan) Pan Id compression is now possible in **LrWpanMac** when transmitting data frames. i.e. When src and dst pan ID are the same, only one PanId is used, making the MAC header 2 bytes smaller. See IEEE 802.15.4-2006 (7.5.6.1).
* (lte) Support for four types of UE handover failure are now modeled:
  * A HO failure is triggered if eNB cannot allocate non-contention-based preamble.
  * Handover joining timeout is now handled.
  * Handover leaving timeout is now handled.
  * Upon RACH failure during HO, the UE will perform cell selection again.
* (network) `Mac(8|16|48|64)Address` address allocation pool is now reset between consecutive runs.
* (propagation) The O2I Low/High Building Penetration Losses will add losses in the pathloss calculation when buildings are present and a UE results to be in O2I state. In order to not consider these losses, they can be disabled by setting BuildingPenetrationLossesEnabled to false.
* (wifi) The **Channel** attribute of `WifiNetDevice` is deprecated because it became ambiguous with the introduction of multiple links per device. The **Channel** attribute of `WifiPhy` can be used instead.

## Changes from ns-3.36 to ns-3.36.1

### New API

None.

### Changes to existing API

* The PTHREAD-dependent classes (mutex, thread and condition variables) were removed and replaced with C++ STL libraries. The API of the STL libraries is very similar to the equivalent ns-3 classes. The main API differences are as follows:
  * `ns-3::SystemMutex` should be refactored to `std::mutex`.
  * `ns-3::CriticalSection cs (m_mutex)` should be refactored to `std::unique_lock lock {m_mutex}`.
  * `ns-3::SystemThread` should be refactored to `std::thread`, which, unlike SystemThread, starts the thread immediately.
  * `ns-3::SystemCondition` should be refactored to `std::condition_variable`, which relies on a companion `std::mutex`.
* The macro for optionally including sqlite3-dependent code has been changed from STATS_HAVE_SQLITE3 to HAVE_SQLITE3, and is now defined globally.

### Changes to build system

The build system API has not changed since ns-3.36.  Several bugs were fixed and behavioral improvements were made; see the RELEASE_NOTES for details.

### Changed behavior

Apart from the bugs fixed (listed in the RELEASE_NOTES), the simulation model behavior should not have changed since ns-3.36.

## Changes from ns-3.35 to ns-3.36

### New API

* The helpers of the NetDevices supporting flow control (`PointToPointHelper`, `CsmaHelper`, `SimpleNetDeviceHelper`, `WifiHelper`) now provide a `DisableFlowControl` method to disable flow control. If flow control is disabled, the Traffic Control layer forwards packets down to the NetDevice even if there is no room for them in the NetDevice queue(s)
* Added a new trace source `TcDrop` in `TrafficControlLayer` for tracing packets that have been dropped because no queue disc is installed on the device, the device supports flow control and the device queue is full.
* Added a new class `PhasedArraySpectrumPropagationLossModel`, and its `DoCalcRxPowerSpectralDensity` function has two additional parameters: TX and RX antenna arrays. Should be inherited by models that need to know antenna arrays in order to calculate RX PSD.
* It is now possible to detach a `SpectrumPhy` object from a `SpectrumChannel` by calling `SpectrumChannel::RemoveRx ()`.
* `PhasedArrayModel` has a new function GetId that returns a unique ID of each `PhasedArrayModel` instance.

### Changes to existing API

* Support for Network Simulation Cradle (NSC) TCP has been removed.
* Support for `PlanetLabFdNetDeviceHelper` has been removed.
* `ThreeGppSpectrumPropagationLossModel` now inherits `PhasedArraySpectrumPropagationLossModel`. The modules that use `ThreeGppSpectrumPropagationLossModel` should implement `SpectrumPhy::GetAntenna` that will return the instance of `PhasedArrayModel`.
* `AddDevice` function is removed from `ThreeGppSpectrumPropagationLossModel` to support multiple arrays per device.
* `SpectrumPhy` function `GetRxAntenna` is renamed to `GetAntenna`, and its return value is changed to `Ptr<Object>` instead of `Ptr<AntennaModel>` to support also `PhasedArrayModel` type of antenna.
* `vScatt` attribute moved from `ThreeGppSpectrumPropagationLossModel` to `ThreeGppChannelModel`.
* `ChannelCondition::IsEqual` now has LOS and O2I parameters instead of a pointer to `ChannelCondition`.
* `TcpWestwood::EstimatedBW` trace source changed from `TracedValueCallback::Double` to `TracedValueCallback::DataRate`.
* The API for making changes to channel number, band, standard, and primary channel has been changed to use a new `ChannelSettings` attribute.

### Changes to build system

* The Waf build system has been replaced by CMake and a Python program called `ns3` that provides a Waf-like API.
* g++ version 8 is now the minimum g++ compiler version supported.
* The default build profile has been changed from `debug` to a new `default`.  Two key differences are that the new default has optimizations enabled (`-O2` vs. previous `-O0`), and the `-Werror` flag is disabled.  Select the `debug` profile to disable optimizations and enable warnings as errors.

### Changed behavior

* Wi-Fi: The default Wi-Fi standard is changed from 802.11a to 802.11ax, and the default rate control is changed from `ArfWifiManager` to `IdealWifiManager`.
* Wi-Fi: EDCAFs (QosTxop objects) are no longer installed on non-QoS STAs and DCF (Txop object) is no longer installed on QoS STAs.
* Wi-Fi: Management frames (Probe Request/Response, Association Request/Response) are sent by QoS STAs according to the 802.11 specs.
* The `Frequency`, `ChannelNumber`, `ChannelWidth` and `Primary20MHzIndex` attributes of `WifiPhy` can now only be used to get the corresponding values. Channel settings can be now configured through the `ChannelSettings` attribute. See the wifi model documentation for information on how to set this new attribute.
* UE handover now works with and without enabled CA (carrier aggregation) in inter-eNB, intra-eNB, inter-frequency and intra-frequency scenarios. Previously only inter-eNB intra-frequency handover was supported and only in non-CA scenarios.
* NixVectorRouting: `NixVectorRouting` can now better cope with topology changes. In-flight packets are not anymore causing crashes, and the path is dynamically rebuilt by intermediate routers (this happens only to packets in-flight during the topology change).
* Mesh (Wi-Fi) forwarding hops now have a configurable random variable-based forwarding delay model, with a default mean of 350 us.

## Changes from ns-3.34 to ns-3.35

### New API

* In class `Ipv6Header`, new functions `SetSource ()`, `SetDestination ()`, `GetSource ()` and `GetDestination ()` are added, consistent with `Ipv4Header`. The existing functions had `Address` suffix in each of the function names, and are are deprecated now.
* In class `Ipv4InterfaceAddress`, new functions `SetAddress ()` and `GetAddress ()` are added, corresponding to `SetLocal ()` and `GetLocal ()` respectively. This is done to keep consistency with `Ipv4InterfaceAddress`.
* With the new support for IPv6 Nix-Vector routing, we have all the new APIs corresponding to IPv4 Nix-Vector routing. Specific to the user, there is an `Ipv6NixVectorHelper` class which can be set in the `InternetStackHelper`, and works similar to `Ipv4NixVectorHelper`.
* In class `Ipv4InterfaceAddress`, a new function `IsInSameSubnet ()` is added to check if both the IPv4 addresses are in the same subnet. Also, it is consistent with `Ipv6InterfaceAddress::IsInSameSubnet ()`.
* In class `ConfigStore`, a new Attribute `SaveDeprecated` allows to not save DEPRECATED Attributes. The default value is `false` (save DEPRECATED Attributes).
* In class `TracedCallback`, a new function `IsEmpty` allows to know if the TracedCallback will call any callback.
* A new specialization of `std::hash` for `Ptr` allows one to use Ptrs as keys in `unordered_map` and `unordered_set` containers.
* A new `GroupMobilityHelper` mobility helper has been added to ease the configuration of group mobility (a form of hierarchical mobility in which multiple child mobility models move with reference to an underlying parent mobility model). New example programs and animation scripts are also added to both the buildings and mobility modules.

### Changes to existing API

* In class `Ipv6Header`, the functions `SetSourceAddress ()`, `SetDestinationAddress ()`, `GetSourceAddress ()` and `GetDestinationAddress ()` are deprecated. New corresponding functions are added by removing the `Address` suffix. This change is made for having consistency with `Ipv4Header`.
* `ipv4-nix-vector-helper.h` and `ipv4-nix-vector-routing.h` have been deprecated in favour of `nix-vector-helper.h` and `nix-vector-routing.h` respectively.

### Changes to build system

* The default C++ standard is now C++17.
* The minimum compiler versions have been raised to g++-7 and clang-10 (Linux) and Xcode 11 (macOS).

### Changed behavior

* Nix-Vector routing supports topologies with multiple WiFi networks using the same WiFi channel object.
* ConfigStore no longer saves OBSOLETE attributes.
* The `Ipv4L3Protocol` Duplicate detection now accounts for transmitted packets, so a transmitting multicast node will not forward its own packets.
* Wi-Fi: A-MSDU aggregation now implies that constituent MSDUs are immediately dequeued from the EDCA queue and replaced by an MPDU containing the A-MSDU. Thus, aggregating N MSDUs triggers N dequeue operations and 1 enqueue operation on the EDCA queue.
* Wi-Fi: MPDUs being passed to the PHY layer for transmission are not dequeued, but are kept in the EDCA queue until they are acknowledged or discarded. Consequently, the BlockAckManager retransmit queue has been removed.

## Changes from ns-3.33 to ns-3.34

### New features and API

* Support for Wi-Fi **802.11ax downlink and uplink OFDMA**, including multi-user OFDMA and a **round-robin multi-user scheduler**.
* `FqCobalt` queue disc with L4S features and set associative hash.
* `FqPIE` queue disc with L4S mode
* `ThompsonSamplingWifiManager` Wi-Fi rate control algorithm.
* New `PhasedArrayModel`, providing a flexible interface for modeling a number of Phase Antenna Array (PAA) models.
* Added the ability to configure the **Wi-Fi primary 20 MHz channel** for 802.11 devices operating on channels of width greater than 20 MHz.
* A **TCP BBRv1** congestion control model.
* **Improved support for bit fields** in header serialization/deserialization.
* Support for **IPv6 stateless address auto-configuration (SLAAC)**.

### Changes to existing API

* The `WifiAckPolicySelector` class has been replaced by the `WifiAckManager` class. Correspondingly, the ConstantWifiAckPolicySelector has been replaced by the WifiDefaultAckManager class. A new WifiProtectionManager abstract base class and WifiDefaultProtectionManager concrete class have been added to implement different protection policies.
* The class `ThreeGppAntennaArrayModel` has been replaced by `UniformPlanarArray`, extending the PhasedArrayModel interface.
* The **`Angles` struct** is now a class, with robust setters and getters (public struct variables `phi` and `theta` are now private class variables `m_azimuth` and `m_inclination`), overloaded `operator<<` and `operator>>` and a number of utilities.
* `AntennaModel` child classes have been extended to produce 3D radiation patterns. Attributes such as Beamwidth have thus been separated into Vertical/HorizontalBeamwidth.
* The attribute `UseVhtOnly` in `MinstrelHtWifiManager` has been replaced by a new attribute called `UseLatestAmendmentOnly`.
* The wifi module has **removed HT Greenfield support, Holland (802.11a-like) PHY configuration, and Point Coordination Function (PCF)**
* The wifi ErrorRateModel API has been extended to support **link-to-system models**.
* **Nix-Vector routing** supports multiple interface addresses and can print out routing paths.
* The `TxOkHeader` and `TxErrHeader` trace sources of `RegularWifiMac` have been obsoleted and replaced by trace sources that better capture the result of a transmission: `AckedMpdu` (fired when an MPDU is successfully acknowledged, via either a Normal Ack or a Block Ack), `NAckedMpdu` (fired when an MPDU is negatively acknowledged via a Block Ack), `DroppedMpdu` (fired when an MPDU is dropped), `MpduResponseTimeout` (fired when a CTS is missing after an RTS or a Normal Ack is missing after an MPDU) and `PsduResponseTimeout` (fired when a BlockAck is missing after an A-MPDU or a BlockAckReq).

### Changes to build system

* The handling of **Boost library/header dependencies** has been improved.

### Changed behavior

* The default **TCP congestion control** has been changed from NewReno to CUBIC.
* The **PHY layer of the wifi module** has been refactored: the amendment-specific logic has been ported to `PhyEntity` classes and `WifiPpdu` classes.
* The **MAC layer of the wifi module** has been refactored. The MacLow class has been replaced by a hierarchy of FrameExchangeManager classes, each adding support for the frame exchange sequences introduced by a given amendment.
* The **wifi BCC AWGN error rate tables** have been aligned with the ones provided by MATLAB and users may note a few dB difference when using BCC at high SNR and high MCS.
* **`ThreeGppChannelModel` has been fixed**: cluster and sub-cluster angles could have been generated with inclination angles outside the inclination range `[0, pi]`, and have now been constrained to the correct range.
* The **LTE RLC Acknowledged Mode (AM) transmit buffer** is now limited by default to a size of (`1024 * 10`) bytes. Configuration of unlimited behavior can still be made by passing the value of zero to the new attribute `MaxTxBufferSize`.
* A **non-AP MLD loses association** when receiving no beacon an any link link for an interval of duration equal to the maximum number of missed beacons times the interval between two consecutive Beacon frames.

## Changes from ns-3.32 to ns-3.33

### New API

* A model for **TCP CUBIC** (RFC 8312) has been added.
* New **channel models based on 3GPP TR 37.885** have been added to support vehicular simulations.
* `Time::RoundTo (unit)` allows time to be rounded to the nearest integer multiple of unit
* `UdpClient` now can report both transmitted and received bytes.
* A new `MPI Enable()` variant was introduced that takes a user-supplied `MPI_Communicator`, allowing for partitioning of the MPI processes.
* A `Length` class has been introduced to allow users to replace the use of raw numbers (ints, doubles) that have implicit lengths with a class that represents lengths with an explicit unit.
* A flexible `CsvReader` class has been introduced to allow users to read in csv- or tab-delimited data.
* The `ListPositionAllocator` can now input positions from a csv file.
* A new trace source for DCTCP alpha value has been added to `TcpDctcp`.
* A new `TableBasedErrorRateModel` has been added for Wi-Fi, and the default values are aligned with link-simulation results from MATLAB WLAN Toolbox and IEEE 802.11 TGn.
* A new `LdpcSupported` attribute has been added for Wi-Fi in `HtConfiguration`, in order to select LDPC FEC encoding instead of the default BCC FEC encoding.

### Changes to existing API

* The signature of `WifiPhy::PsduTxBeginCallback` and `WifiPhy::PhyTxPsduBegin` have been changed to take a map of PSDUs instead of a single PSDU in order to support multi-users (MU) transmissions.
* The wifi trace `WifiPhy::PhyRxBegin` has been extended to report the received power for every band.
* The wifi trace `WifiPhy::PhyRxBegin` has been extended to report the received power for every band.
* New attributes `SpectrumWifiPhy::TxMaskInnerBandMinimumRejection`, `SpectrumWifiPhy::TxMaskOuterBandMinimumRejection` and `SpectrumWifiPhy::TxMaskOuterBandMaximumRejection` have been added to configure the OFDM transmit masks.

### Changes to build system

* Waf has been upgraded to git development version waf-2.0.21-6-g60e3f5f4

### Changed behavior

* The **default Wi-Fi ErrorRateModel** for the 802.11n/ac/ax standards has been changed from the NistErrorRateModel to a new TableBasedErrorRateModel. Users may experience a shift in Wi-Fi link range due to the new default error model, as **the new model is more optimistic** (the PER for a given MCS will degrade at a lower SNR value). The Wi-Fi module documentation provides plots that compare the performance of the NIST and new table-based model.
* The default value of the `BerThreshold` attribute in `IdealWifiManager` was changed from 1e-5 to 1e-6, so as to correct behavior with high order MCS.
* **Time values that are created from an `int64x64_t` value** are now rounded to the nearest integer multiple of the unit, rather than truncated. Issue #265 in the GitLab.com tracker describes the behavior that was fixed. Some Time values that rely on this conversion may have changed due to this fix.
* TCP now implements the Linux-like **congestion window reduced (CWR)** state when explicit congestion notification (ECN) is enabled.
* `TcpDctcp` now inherits from `TcpLinuxReno`, making its congestion avoidance track more closely to that of Linux.

## Changes from ns-3.31 to ns-3.32

### New API

* A new TCP congestion control, `TcpLinuxReno`, has been added.
* Added, to **PIE queue disc**, **queue delay calculation using timestamp** feature (Linux default behavior), **cap drop adjustment** feature (Section 5.5 of RFC 8033), **ECN** (Section 5.1 of RFC 8033) and **derandomization** feature (Section 5.4 of RFC 8033).
* Added **L4S Mode** to FqCoDel and CoDel queue discs
* A model for **dynamic pacing** has been added to TCP.
* Added **Active/Inactive feature** to PIE queue disc
* Added **netmap** and **DPDK** emulation device variants
* Added capability to configure **STL pair and containers as attributes**
* Added **CartesianToGeographic** coordinate conversion capability
* Added **LollipopCounter**, a sequence number counter type
* Added **6 GHz band** support for Wi-Fi 802.11ax

### Changes to existing API

* The `Sifs`, `Slot` and `Pifs` attributes have been moved from `WifiMac` to `WifiPhy` to better reflect that they are PHY characteristics, to decouple the MAC configuration from the PHY configuration and to ease the support for future standards.
* The Histogram class was moved from the flow-monitor module to the stats module to make it more easily accessed. If you previously used Histogram by by including flow-monitor.h you will need to change that to stats-module.h.
* The `WifiHelper::SetStandard (WifiPhyStandard standard)` method no longer takes a WifiPhyStandard enum, but instead takes a similarly named WifiStandard enum. If before you specified a value such as `WIFI_PHY_STANDARD_xxx`, now you must specify `WIFI_STANDARD_xxx`.
* The `YansWifiPhyHelper::Default` and `SpectrumWifiPhyHelper::Default` methods have been removed; the default constructors may instead by used.
* **PIE** queue disc now uses `Timestamp` for queue delay calculation as default instead of **Dequeue Rate Estimator**

### Changes to build system

* Added `--enable-asserts` and `--enable-logs` to waf configure, to selectively enable asserts and/or logs in release and optimized builds.
* A build version reporting system has been added by extracting data from the local git repository (or a standalone file if a git repository is not present).
* Added support for EditorConfig

### Changed behavior

* Support for **RIFS** has been dropped from wifi. RIFS has been obsoleted by the 802.11 standard and support for it was not implemented according to the standard.
* The default loss recovery algorithm for TCP has been changed from Classic Recovery to Proportional Rate Reduction (PRR).
* The behavior of `TcpPrrRecovery` algorithm was aligned to that of Linux.
* **PIE** queue disc now uses `Timestamp` for queue delay calculation as default instead of **Dequeue Rate Estimator**
* TCP pacing, when enabled, now adjusts the rate dynamically based on the window size, rather than just enforcing a constant rate.
* WifiPhy forwards up MPDUs from an A-MPDU under reception as long as they arrive at the PHY, instead of forwarding up the whole A-MPDU once its reception is completed.
* The ns-3 TCP model was changed to set the initial congestion window to 10 segments instead of 1 segment (to align with default Linux configuration).

## Changes from ns-3.30 to ns-3.31

### New API

* A **TCP DCTCP** model has been added.
* **3GPP TR 38.901** pathloss, channel condition, antenna array, and fast fading models have been added.
* New `...FailSafe ()` variants of the `Config` and `Config::MatchContainer` functions which set Attributes or connect TraceSources. These all return a boolean indicating if any attributes could be set (or trace sources connected). These are useful if you are not sure that the requested objects exist, for example in AnimationInterface.
* New attributes for `Ipv4L3Protocol` have been added to enable RFC 6621-based duplicate packet detection (DPD) (`EnableDuplicatePacketDetection`) and to control the cache expiration time (`DuplicateExpire`).
* `MakeConsistent` method of `BuildingsHelper` class is deprecated and moved to `MobilityBuildingInfo` class. `DoInitialize` method of the `MobilityBuildingInfo` class would be responsible for making the mobility model of a node consistent at the beginning of a simulation. Therefore, there is no need for an explicit call to `MakeConsistent` in a simulation script.
* The `IsInside` method of `MobilityBuildingInfo` class is extended to make the mobility model of a moving node consistent.
* The `IsOutside` method of `MobilityBuildingInfo` class is deprecated. The `IsInside` method should be use to check the position of a node.
* A new abstract base class, `WifiAckPolicySelector`, is introduced to implement different techniques for selecting the acknowledgment policy for PSDUs containing QoS Data frames. Wifi, mesh and wave helpers provide a SetAckPolicySelectorForAc method to configure a specific ack policy selector for a given Access Category.
* The default ack policy selector is named `ConstantWifiAckPolicySelector`, which allows to choose between Block Ack policy and Implicit Block Ack Request policy and allows to request an acknowledgment after a configurable number of MPDUs have been transmitted.
* The `MaxSize` attribute is removed from the `QueueBase` base class and moved to subclasses. A new MaxSize attribute is therefore added to the DropTailQueue class, while the MaxQueueSize attribute of the WifiMacQueue class is renamed as MaxSize for API consistency.
* Two new **Application sequence number and timestamp** variants have been added, to support packet delivery tracing.
  * A new sequence and timestamp header variant for applications has been added. The `SeqTsEchoHeader` contains an additional timestamp field for use in echoing a timestamp back to a sender.
  * TCP-based applications (OnOffApplication, BulkSendApplication, and PacketSink) support a new header, `SeqTsSizeHeader`, to convey sequence number, timestamp, and size data. Use is controlled by the `EnableSeqTsSizeHeader` attribute.
* Added a new trace source `PhyRxPayloadBegin` in WifiPhy for tracing begin of PSDU reception.
* Added the class `RandomWalk2dOutdoorMobilityModel` that models a random walk which does not enter any building.
* Added support for the **Cake set-associative hash** in the FqCoDel queue disc
* Added support for **ECN marking for CoDel and FqCoDel** queue discs

### Changes to existing API

* The API for **enabling and disabling ECN** in TCP sockets has been refactored.
* The **LTE HARQ** related methods in LteEnbPhy and LteUePhy have been renamed, and the LteHelper updated.
* Previously the `Config::Connect` and `Config::Set` families of functions would fail silently if the attribute or trace source didn't exist on the path given (typically due to spelling errors). Now those functions will throw a fatal error. If you need the old behavior use the new `...FailSafe ()` variants.
* The internal TCP API for `TcpCongestionOps` has been extended to support the `CongControl` method to allow for delivery rate estimation feedback to the congestion control mechanism.
* Functions `LteEnbPhy::ReceiveUlHarqFeedback` and `LteUePhy::ReceiveLteDlHarqFeedback` are renamed to `LteEnbPhy::ReportUlHarqFeedback` and `LteUePhy::EnqueueDlHarqFeedback`, respectively to avoid confusion about their functionality. `LteHelper` is updated accordingly.
* Now on, instead of `uint8_t`, `uint16_t` would be used to store a bandwidth value in LTE.
* The preferred way to declare instances of `CommandLine` is now through a macro: `COMMANDLINE (cmd)`. This enables us to add the `CommandLine::Usage()` message to the Doxygen for the program.
* New `...FailSafe ()` variants of the `Config` is used to connect PDCP TraceSources of eNB and UE in `RadioBearerStatsConnector` class. It is required for the simulations using RLC SM where PDCP objects are not created for data radio bearers.
* `T310` timer in `LteUeRrc` class is stopped if the UE receives `RRCConnectionReconfiguration` including the `mobilityControlInfo`. This change is introduced following the 3GPP standard TS36331 sec 5.3.5.4.
* The wifi `High Latency tags` have been removed. The only rate manager (Onoe) that was making use of them has been refactored.
* The wifi `MIMO diversity model` has been changed to better fit with MRC theory for AWGN channels when STBC is not used (since STBC is currently not supported).
* The `BuildingsHelper::MakeMobilityModelConsistent()` method is deprecated in favor of MobilityBuildingInfo::MakeConsistent
* The `MobilityBuildingInfo::IsOutdoor ()` method is deprecated; use the result of IsIndoor() method instead
* IsEqual() methods of class `Ipv4Address`, `Ipv4Mask`, `Ipv6Address`, and `Ipv6Prefix` are deprecated.
* The API around the **wifi Txop class** was refactored.

### Changes to build system

* The `--lcov-report` option to Waf was fixed, and a new `--lcov-zerocounters` option was added to improve support for lcov.
* Python bindings were enabled for `netanim`.

### Changed behavior

* The `EmpiricalRandomVariable` no longer linearly interpolates between values by default, but instead will default to treating the CDF as a histogram and return one of the specific inputs. The previous interpolation mode can be configured by an attribute.
* (as reported above) previously the `Config::Connect` and `Config::Set` families of functions would fail silently if the attribute or trace source didn't exist on the path given (typically due to spelling errors). Now those functions will throw a fatal error. If you need the old behavior use the new `...FailSafe ()` variants.
* Attempting to deserialize an enum name which wasn't registered with `MakeEnumChecker` now causes a fatal error, rather failing silently. (This can be triggered by setting an enum Attribute from a StringValue.)
* As a result of the above API changes in `MobilityBuildingInfo` and `BuildingsHelper` classes, a building aware pathloss models, e.g., `HybridBuildingsPropagationLossModel` is now able to accurately compute the pathloss for a node moving in and out of buildings in a simulation. See [issue 80](https://gitlab.com/nsnam/ns-3-dev/issues/80) for discussion.
* The implementation of the **Wi-Fi channel access** functions has been improved to make them more conformant to the IEEE 802.11-2016 standard. Concerning the DCF, the backoff procedure is no longer invoked when a packet is queued for transmission and the medium has not been idle for a DIFS, but it is invoked if the medium is busy or does not remain idle for a DIFS after the packet has been queued. Concerning the EDCAF, transmissions are now correctly aligned at slot boundaries.
* Various wifi physical layer behavior around channel occupancy calculation, phy state calculation, and handling different channel widths has been updated.

## Changes from ns-3.29 to ns-3.30

### New API

* Added the attribute `Release` to the class `EpsBearer`, to select the release (e.g., release 15)
* The attributes `RegularWifiMac::HtSupported`, `RegularWifiMac::VhtSupported`, `RegularWifiMac::HeSupported`, `RegularWifiMac::RifsSupported`, `WifiPhy::ShortGuardEnabled`, `WifiPhy::GuardInterval` and `WifiPhy::GreenfieldEnabled` have been deprecated. Instead, it is advised to use `WifiNetDevice::HtConfiguration`, `WifiNetDevice::VhtConfiguration` and `WifiNetDevice::HeConfiguration`.
* The attributes `{Ht,Vht,He}Configuration::{Vo,Vi,Be,Bk}MaxAmsduSize` and `{Ht,Vht,He}Configuration::{Vo,Vi,Be,Bk}MaxAmpduSize` have been removed. Instead, it is necessary to use `RegularWifiMac::{VO,VI,BE,BK}_MaxAmsduSize` and `RegularWifiMac::{VO,VI,BE,BK}_MaxAmpduSize`.
* A new attribute `WifiPhy::PostReceptionErrorModel` has been added to force specific packet drops.
* A new attribute `WifiPhy::PreambleDetectionModel` has been added to decide whether PHY preambles are successfully detected.
* New attributes `QosTxop::AddBaResponseTimeout` and `QosTxop::FailedAddBaTimeout` have been added to set the timeout to wait for an ADDBA response after the ACK to the ADDBA request is received and to set the timeout after a failed BA agreement, respectively.
* A new attribute `QosTxop::UseExplicitBarAfterMissedBlockAck` has been added to specify whether explicit Block Ack Request should be sent upon missed Block Ack Response.
* Added a new trace source `EndOfHePreamble` in WifiPhy for tracing end of preamble (after training fields) for received 802.11ax packets.
* Added a new helper method to SpectrumWifiPhyHelper and YansWifiPhyHelper to set the **frame capture model**.
* Added a new helper method to SpectrumWifiPhyHelper and YansWifiPhyHelper to set the **preamble detection model**.
* Added a new helper method to WifiPhyHelper to disable the preamble detection model.
* Added a method to ObjectFactory to check whether a TypeId has been configured on the factory.
* Added a new helper method to WifiHelper to set the **802.11ax OBSS PD spatial reuse algorithm**.
* Added the **Cobalt queuing discipline**.
* Added `Simulator::GetEventCount ()` to return the number of events executed.
* Added `ShowProgress` object to display simulation progress statistics.
* Add option to disable explicit Block Ack Request when a Block Ack Response is missed.
* Add API to be able to tag a subset of bytes in an ns3::Packet.
* New LTE helper API has been added to allow users to configure LTE backhaul links with any link technology, not just point-to-point links.

### Changes to existing API

* Added the possibility of setting the z coordinate for many position-allocation classes: `GridPositionAllocator`, `RandomRectanglePositionAllocator`, `RandomDiscPositionAllocator`, `UniformDiscPositionAllocator`.
* The WifiPhy attribute `CcaMode1Threshold` has been renamed to `CcaEdThreshold`, and the WifiPhy attribute `EnergyDetectionThreshold` has been replaced by a new attribute called `RxSensitivity`.
* It is now possible to know the size of the SpectrumValue underlying std::vector, as well as accessing read-only every element of it.
* The `GetClosestSide` method of the Rectangle class returns the correct closest side also for positions outside the rectangle.
* The trace sources `BackoffTrace` and `CwTrace` were moved from class QosTxop to base class Txop, allowing these values to be traced for DCF operation. In addition, the trace signature for BackoffTrace was changed from TracedValue to TracedCallback (callback taking one argument instead of two). Most users of CwTrace for QosTxop configurations will not need to change existing programs, but users of BackoffTrace will need to adjust the callback signature to match.
* New trace sources, namely `DrbCreated`, `Srb1Created` and `DrbCreated` have been implemented in LteEnbRrc and LteUeRrc classes respectively. These new traces are used to improve the connection of the RLC and PDCP stats in the RadioBearerStatsConnector API.
* `TraceFadingLossModel` has been moved from lte to spectrum module.

### Changes to build system

* **ns-3 now only supports Python 3**. Use of Python 2 can be forced using the `--with-python` option provided to `./waf configure`, and may still work for many cases, but is no longer supported. Waf does not default to Python 3 but the ns-3 wscript will default the build to Python 3.
* Waf upgraded from 2.0.9 to 2.0.18.
* Options to run a program through Waf without invoking a project rebuild have been added. The command `./waf --run-no-build` parallels the behavior of `./waf --run` and, likewise, the command `./waf --pyrun-no-build` parallels the behavior of `./waf --pyrun`.

### Changed behavior

* The wifi ADDBA handshake process is now protected with the use of two timeouts who makes sure we do not end up in a blocked situation. If the handshake process is not established, packets that are in the queue are sent as normal MPDUs. Once handshake is successfully established, A-MPDUs can be transmitted.
* In the wifi module, the default value of the `Margin` attribute in SimpleFrameCaptureModel was changed from 10 to 5 dB.
* A `ThresholdPreambleDetectionModel` is added by default to the WifiPhy. Using default values, this model will discard frames that fall below either -82 dBm RSSI or below 4 dB SNR. Users may notice that weak wifi signals that were successfully received based on the error model alone (in previous ns-3 releases) are no longer received. Previous behavior can be obtained by lowering both threshold values or by removing the preamble detection model (via WifiPhyHelper::DisablePreambleDetectionModel()).
* The PHY model for Wi-Fi has been extended to handle reception of L-SIG and reception of non-legacy header differently.
* LTE/EPC model has been enhanced to allow the simulation user to test more realistic topologies related to the core network:

* SGW, PGW and MME are full nodes.
* There are P2P links between core network nodes.
* New S5 interface between SGW and PGW nodes based on GTPv2-C protocol.
* Allow simulations with multiple SGWs and PGWs.

* LTE eNB RRC is extended to support:

* S1 signalling with the core network is initiated after the RRC connection establishment procedure is finished.
* New `ATTACH_REQUEST` state to wait for finalization of the S1 signalling with the core network.
* New InitialContextSetupRequest primitive of the S1 SAP that is received by the eNB RRC when the S1 signalling from the core network is finished.

* A new buffer has been introduced in the LteEnbRrc class. This buffer will be used by a target eNB during handover to buffer the packets coming from a source eNB on X2 interface. The target eNB will buffer this data until it receives RRC Connection Reconfiguration Complete from a UE.
* The default qdisc installed on single-queue devices (such as PointToPoint, Csma and Simple) is now `FqCoDel` (instead of PfifoFast). On multi-queue devices (such as Wifi), the default root qdisc is now `Mq` with as many FqCoDel child qdiscs as the number of device queues. The new defaults are motivated by the willingness to align with the behavior of major Linux distributions and by the need to preserve the effectiveness of Wifi EDCA Functions in differentiating Access Categories (see issue #35).
* LTE RLC TM mode does not report anymore the layer-to-layer delay, as it misses (by standard) an header to which attach the timestamp tag. Users can switch to the PDCP layer delay measurements, which must be the same.
* Token Bank Fair Queue Scheduler (`ns3::FdTbfqFfMacScheduler`) will not anymore schedule a UE, which does not have any RBG left after removing the RBG from its allocation map if the computed TB size is greater than the "budget" computed in the scheduler.
* LTE module now supports the **Radio Link Failure (RLF)** functionality. This implementation introduced following key behavioral changes:
  * The UE RRC state will not remain in `CONNECTED_NORMALLY` state if the DL control channel SINR is below a set threshold.
  * The LTE RRC protocol APIs of UE i.e., LteUeRrcProtocolIdeal, LteUeRrcProtocolReal have been extended to send an ideal (i.e., using SAPs instead to transmitting over the air) UE context remove request to the eNB. Similarly, the eNB RRC protocol APIs, i.e, LteEnbRrcProtocolIdeal and LteEnbRrcProtocolReal have been extended to receive this ideal UE context remove request.
  * The UE will not synchronize to a cell whose RSRP is less than -140 dBm.
  * The non-contention based preambles during a handover are re-assigning to an UE only if it has not been assign to another UE (An UE can be using the preamble even after the expiryTime duration).
  * The RachConfigCommon structure in LteRrcSap API has been extended to include `TxFailParam`. This new field would enable an eNB to indicate how many times T300 timer can expire at the UE. Upon reaching this count, the UE aborts the connection establishment, and performs the cell selection again. See TS 36.331 5.3.3.6.
* The timer T300 in LteUeRrc class is now bounded by the standard min and max values defined in 3GPP TS 36.331.

## Changes from ns-3.28 to ns-3.29

### New API

* CommandLine can now handle non-option (positional) arguments.
* Added `CommandLine::Parse (const std::vector<std::string> args)`
* `NS_LOG_FUNCTION` can now log the contents of vectors
* A new position allocator has been added to the buildings module, allowing nodes to be placed outside of buildings defined in the scenario.
* The `Hash()` method has been added to the `QueueDiscItem` class to compute the hash of various fields of the packet header (depending on the packet type).
* Added a priority queue disc (`PrioQueueDisc`).
* Added 3GPP HTTP model
* Added TCP PRR as recovery algorithm
* Added a new trace source in `StaWifiMac` for tracing beacon arrivals
* Added a new helper method to `ApplicationContainer` to start applications with some jitter around the start time
* (network) Add a method to check whether a node with a given ID is within a `NodeContainer`.

### Changes to existing API

* TrafficControlHelper::Install now only includes root queue discs in the returned QueueDiscContainer.
* Recovery algorithms are now in a different class, instead of being tied to TcpSocketBase. Take a look to TcpRecoveryOps for more information.
* The Mode, MaxPackets and MaxBytes attributes of the Queue class, that had been deprecated in favor of the MaxSize attribute in ns-3.28, have now been removed and cannot be used anymore. Likewise, the methods to get/set the old attributes have been removed as well. Commands such as:

  ```cpp
  Config::SetDefault ("ns3::QueueBase::MaxPackets", UintegerValue (4));
  ```

  should now be written as:

  ```cpp
  Config::SetDefault ("ns3::QueueBase::MaxSize", QueueSizeValue (QueueSize (QueueSizeUnit::PACKETS, 4)));
  ```

  or with a string value with `b` (bytes) or `p` (packets) suffix, such as:

  ```cpp
  Config::SetDefault ("ns3::QueueBase::MaxSize", StringValue ("4p"));
  ```

* The Limit attribute of the PfifoFastQueueDisc class, that had been deprecated in favor of the MaxSize attribute in ns-3.28, has now been removed and cannot be used anymore. Likewise, the methods to get/set the old Limit attribute have been removed as well. The GetMaxSize/SetMaxSize methods of the base QueueDisc class must be used instead.
* The Mode, MaxPackets and MaxBytes attributes of the CoDelQueueDisc class, that had been deprecated in favor of the MaxSize attribute in ns-3.28, have now been removed and cannot be used anymore. Likewise, the methods to get/set the old attributes have been removed as well. The GetMaxSize/SetMaxSize methods of the base QueueDisc class must be used instead.
* The PacketLimit attribute of the FqCoDelQueueDisc class, that had been deprecated in favor of the MaxSize attribute in ns-3.28, has now been removed and cannot be used anymore. Likewise, the methods to get/set the old PacketLimit attribute have been removed as well. The GetMaxSize/SetMaxSize methods of the base QueueDisc class must be used instead.
* The Mode and QueueLimit attributes of the PieQueueDisc class, that had been deprecated in favor of the MaxSize attribute in ns-3.28, have now been removed and cannot be used anymore. Likewise, the methods to get/set the old attributes have been removed as well. The GetMaxSize/SetMaxSize methods of the base QueueDisc class must be used instead.
* The Mode and QueueLimit attributes of the RedQueueDisc class, that had been deprecated in favor of the MaxSize attribute in ns-3.28, have now been removed and cannot be used anymore. Likewise, the methods to get/set the old attributes have been removed as well. The GetMaxSize/SetMaxSize methods of the base QueueDisc class must be used instead.
* Several traffic generating applications have additional trace sources that export not only the transmitted or received packet but also the source and destination addresses.
* The returned type of `GetNDevices` methods in `Channel` and subclasses derived from it were changed from `uint32_t` to `std::size_t`. Likewise, the input parameter type of `GetDevice` in `Channel` and its subclasses were changed from `uint32_t` to `std::size_t`.
* Wifi classes `DcfManager`, `DcaTxop` and `EdcaTxopN` were renamed to `ChannelAccessManager`, `Txop` and `QosTxop`, respectively.
* `QueueDisc::DequeuePeeked` has been merged into `QueueDisc::Dequeue` and hence no longer exists.
* The `QueueDisc` base class now provides a default implementation of the `DoPeek` private method based on the `QueueDisc::PeekDequeue` method, which is now no longer available.
* The `QueueDisc::SojournTime` trace source is changed from a `TracedValue` to a `TracedCallback`; callbacks that hook this trace must provide one `ns3::Time` argument, not two.
* To avoid the code duplication in `SingleModelSpectrumChannel` and `MultiModelSpectrumChannel` classes, the attributes `MaxLossDb` and `PropagationLossModel`, and the traces `PathLoss` and `TxSigParams` are moved to the base class `SpectrumChannel`. Similarly, the functions `AddPropagationLossModel`, `AddSpectrumPropagationLossModel`, `SetPropagationDelayModel` and `GetSpectrumPropagationLossModel` are now defined in `SpectrumChannel` class. Moreover, the `TracedCallback` signature of `LossTracedCallback` has been updated from:

  ```cpp
  typedef void (*LossTracedCallback) (Ptr<SpectrumPhy> txPhy, Ptr<SpectrumPhy> rxPhy, double lossDb);
  ```

  To:

  ```cpp
  typedef void (*LossTracedCallback) (Ptr<const SpectrumPhy> txPhy, Ptr<const SpectrumPhy> rxPhy, double lossDb);
  ```

* For the sake of LTE module API consistency the IPV6 related functions AssignUeIpv6Address and GetUeDefaultGatewayAddress6 are now declared in EpcHelper base class. Thus, these functions are now declared as virtual in the child classes, i.e., EmuEpcHelper and PointToPointEpcHelper.

### Changes to build system

* Waf upgraded from 1.8.19 to 2.0.9, and ns-3 build scripts aligned to the new API.
* The `--no32bit-scan` argument is removed from Waf apiscan; generation of ILP32 bindings is now automated from the LP64 bindings.
* When using on newer compilers, new warnings may trigger build failures. The --disable-werror flag can be passed to Waf at configuration time to turn off the Werror behavior.
* GTK+3 libraries (including PyGObject, GooCanvas2) are needed for the Pyviz visualizer, replacing GTK+2 libraries.

### Changed behavior

* FqCoDelQueueDisc now computes the hash of the packet's 5-tuple to determine the flow the packet belongs to, unless a packet filter has been configured. The previous behavior is simply obtained by not configuring any packet filter. Consequently, the FqCoDelIpv{4,6}PacketFilter classes have been removed.
* ARP packets now pass through the traffic control layer, as in Linux.
* The maximum size UDP packet of the UdpClient application is no longer limited to 1500 bytes.
* The default values of the `MaxSlrc` and `FragmentationThreshold` attributes in WifiRemoteStationManager were changed from 7 to 4 and from 2346 to 65535, respectively.

## Changes from ns-3.27 to ns-3.28

### New API

* When deserializing Packet contents, `Header::Deserialize (Buffer::Iterator start)` and `Trailer::Deserialize (Buffer::Iterator start)` can not successfully deserialize variable-length headers and trailers. New variants of these methods that also include an `end` parameter are now provided.
* `Ipv[4,6]AddressGenerator` can now check if an address is allocated (`Ipv[4,6]AddressGenerator::IsAddressAllocated`) or a network has some allocated address (`Ipv[4,6]AddressGenerator::IsNetworkAllocated`).
* LTE UEs can now use IPv6 to send and receive traffic.
* UAN module now supports an IP stack.
* Class `TcpSocketBase` trace source `CongestionWindowInflated` shows the values with the in-recovery inflation and the post-recovery deflation.
* Added a FIFO queue disc (`FifoQueueDisc`) and the Token Bucket Filter (`TbfQueueDisc`).

### Changes to existing API

* Class `LrWpanMac` now supports extended addressing mode. Both `McpsDataRequest` and `PdDataIndication` methods will now use extended addressing if `McpsDataRequestParams::m_srcAddrMode` or `McpsDataRequestParams::m_dstAddrMode` are set to `EXT_ADDR`.
* Class `LteUeNetDevice` MAC address is now a 64-bit address and can be set during construction.
* Class `TcpSocketBase` trace source `CongestionWindow` shows the values without the in-recovery inflation and the post-recovery deflation; the old behavior has been moved to the new trace source `CongestionWindowInflated`.
* The Mode, MaxPackets and MaxBytes attributes of the Queue class have been deprecated in favor of the MaxSize attribute. Old attributes can still be used, but using them will be no longer possible in one of the next releases. The methods to get/set the old attributes will be removed as well.
* The attributes of the QueueDisc subclasses that separately determine the mode and the limit of the QueueDisc have been deprecated in favor of the single MaxSize attribute.
* The GetQueueSize method of some QueueDisc subclasses (e.g., RED) has been removed and replaced by the GetCurrentSize method of the QueueDisc base class.

### Changes to build system

* The C++ standard used during compilation (default std=c++11) can be now be changed via the CXXFLAGS variable.

### Changed behavior

* All Wi-Fi management frames are now transmitted using the lowest basic rate.
* The Wi-Fi spectrum model now takes into account adjacent channels through OFDM transmit spectrum masks.
* The CsmaNetDevice::PhyTxBeginTrace will trace all attempts to transmit, even those that result in drops. Previously, eventual channel drops were excluded from this trace.
* The TCP congestion window trace now does not report on window inflation during fast recovery phase because it is no longer internally maintained as an inflated value (a separate trace called CongestionWindowInflated can be used to recover the old trace behavior).

## Changes from ns-3.26 to ns-3.27

### New API

* Added `Vector{2,3}D.GetLength ()`.
* Overloaded `operator+` and `operator-` for `Vector{2,3}D`.
* Added iterator version of WifiHelper::Install() to install Wi-Fi devices on range of nodes.
* Added a new attribute in TcpSocketBase to track the advertised window.
* Included the model of **TCP Ledbat**.
* Included the TCP SACK-based loss recovery algorithm outlined in RFC 6675.
* Added **TCP SACK** and the **SACK emulation**. Added an Attribute to `TcpSocketBase` class, called `Sack`, to enable or disable the SACK option usage.
* In `src/wifi`, several changes were made to enable partial **802.11ax** High Efficiency (HE) support:
* A new standard value has been added that enables the new 11ax data rates.
* A new 11ax preamble has been added.
* A new attribute was added to configure the guard interval duration for High Efficiency (HE) PHY entities. This attribute can be set using the YansWifiPhyHelper.
* A new information element has been added: HeCapabilities. This information element is added to the MAC frame header if the node is a HE node. This HeCapabilities information element is used to advertise the HE capabilities of the node to other nodes in the network.
* A new class were added for the RRPAA WiFi rate control mechanism.
* Included carrier aggregation feature in LTE module

* LTE model is extended to support carrier aggregation feature according to 3GPP Release 10, for up to 5 component carriers.
* InstallSingleEnbDevice and InstallSingleUeDevice functions of LteHelper are now constructing LteEnbDevice and LteUeDevice according to CA architecture. Each device, UE and eNodeB contains an instance of component carrier manager, and may have several component carrier instances.
* SAP interfaces are extended to include CA message exchange functionality.
* RRC connection procedure is extended to allow RRC connection reconfiguration for the configuration of the secondary carriers.
* RRC measurement reporting is extended to allow measurement reporting from the secondary carriers.
* LTE traces are extended to include component carrier id.

* Function `PrintRoutingTable` has been extended to add an optional Time::Units parameter to specify the time units used on the report. The new parameter is optional and if not specified defaults to the previous behavior (Time::S).
* `TxopTrace`: new trace source exported by EdcaTxopN.
* A `GetDscpCounts` method is added to `Ipv4FlowClassifier` and `Ipv6FlowClassifier` which returns a vector of pairs (dscp,count), each of which indicates how many packets with the associated dscp value have been classified for a given flow.
* MqQueueDisc, a multi-queue aware queue disc modelled after the mq qdisc in Linux, has been introduced.
* Two new methods, `QueueDisc::DropBeforeEnqueue()` and `QueueDisc::DropAfterDequeue()` have been introduced to replace `QueueDisc::Drop()`. These new methods require the caller to specify the reason why a packet was dropped. Correspondingly, two new trace sources (`DropBeforeEnqueue` and `DropAfterDequeue`) have been added to the QueueDisc class, providing both the items that were dropped and the reason why they were dropped.
* Added `QueueDisc::GetStats()` which returns detailed statistics about the operations of a queue disc. Statistics can be accessed through the member variables of the returned object and by calling the `GetNDroppedPackets()`, `GetNDroppedBytes()`, `GetNMarkedPackets()` and `GetNMarkedBytes()` methods on the returned object. Such methods return the number of packets/bytes dropped/marked for the specified reason (passed as argument). Consequently:
  * A number of methods of the QueueDisc class have been removed: `GetTotalReceivedPackets()`, `GetTotalReceivedBytes()`, `GetTotalDroppedPackets()`, `GetTotalDroppedBytes()`, `GetTotalRequeuedPackets()`, `GetTotalRequeuedBytes()`.
  * The `Stats` struct and the `GetStats()` method of `RedQueueDisc` and `PieQueueDisc` have been removed and replaced by those of the QueueDisc base class.
  * The `GetDropOverLimit` and `GetDropCount` methods of `CoDelQueueDisc` have been removed. The values they returned can be obtained by calling, respectively, GetStats ().`GetNDroppedPackets (CoDelQueueDisc::OVERLIMIT_DROP)` and `GetStats ().GetNDroppedPackets (CoDelQueueDisc::TARGET_EXCEEDED_DROP`). The `DropCount` trace of `CoDelQueueDisc` has been removed as well. Packets dropped because the target is exceeded can be obtained through the new `DropAfterDequeue` trace of the QueueDisc class.
* The new `QueueDisc::Mark()` method has been introduced to allow subclasses to request to mark a packet. The caller must specify the reason why the packet must be marked. Correspondingly, a new trace source (`Mark`) has been added to the QueueDisc class, providing both the items that were marked and the reason why they were marked.
* A new trace source, `SojournTime`, is exported by the QueueDisc base class to provide the sojourn time of every packet dequeued from a queue disc. This has been made possible by adding a timestamp to QueueDiscItem objects, which can be set/get through the new `GetTimeStamp()` and `SetTimeStamp()` methods of the QueueDiscItem class. The `CoDel` queue disc now makes use of such feature of the base class, hence its Sojourn trace source and the CoDelTimestampTag class have been removed.

### Changes to existing API

* `ParetoRandomVariable` `Mean` attribute has been deprecated, the `Scale` Attribute have to be used instead. Changing the Mean attribute has no more an effect on the distribution. See the documentation for the relationship between Mean, Scale and Shape.
* The default logging timestamp precision has been changed from 6 digits to 9 digits, with a fixed format to ensure that 9 digits to the right of the decimal point are always printed. Previously, default C++ iostream precision and formatting was used.
* Abstract base class `WifiChannel` has been removed. As a result, a Channel type instead of a WifiChannel type is now exported by WifiNetDevice.
* The `GetPacketSize` method of `QueueItem` has been renamed `GetSize`
* The `DequeueAll` method of `Queue` has been renamed `Flush`
* The attributes `WifiPhy::TxAntennas` and `WifiPhy::RxAntennas`, and the related accessor methods, were replaced by `WifiPhy::MaxSupportedTxSpatialStreams` and `WifiPhy::MaxSupportedRxSpatialStreams`. A new attribute `WifiPhy::Antennas` was added to allow users to define the number of physical antennas on the device.
* Sockets do not receive anymore broadcast packets, unless they are bound to an `Any` address (0.0.0.0) or to a subnet-directed broadcast packet (e.g., x.y.z.0 for a /24 noterok). As in Linux, the following rules are now enforced:

  * A socket bound to 0.0.0.0 will receive everything.
  * A socket bound to x.y.z.0/24 will receive subnet-directed broadcast (x.y.z.255) and unicast packets.
  * A socket bound to x.y.z.w will only receive unicast packets.

    **Previously, a socket bound to an unicast address received also subnet-directed broadcast packets. This is not anymore possible**.
* You can now Bind as many socket as you want to an address/port, provided that they are bound to different NetDevices. Moreover, BindToNetDevice does not anymore call Bind. In other terms, Bind and BindToNetDevice can be called in any order. However, it is suggested to use BindToNetDevice _before_ Bind in order to avoid conflicts.

### Changes to build system

* The API scanning process for Python bindings now relies on CastXML, and only 64-bit scans are presently supported (Linux 64-bit systems). Generation of 32-bit scans is documented in the Python chapter of the ns-3 manual.
* Modules can now be located in the `contrib/` directory in addition to `src/`
* Behavior for running Python programs was aligned with that of C++ programs; the list of modules built is no longer printed out.

### Changed behavior

* `MultiModelSpectrumChannel` does not call StartRx for receivers that operate on subbands orthogonal to transmitter subbands. Models that depend on receiving signals with zero power spectral density from orthogonal bands may change their behavior. See [bug 2467](https://www.nsnam.org/bugzilla/show_bug.cgi?id=2467) for discussion.
* **Packet Tag objects** are no longer constrained to fit within 21 bytes; a maximum size is no longer enforced.
* The default value of the `TxGain` and `RxGain` attributes in WifiPhy was changed from 1 dB to 0 dB.
* The reported SNR by WifiPhy::MonitorSnifferRx did not include the RxNoiseFigure, but now does; see [bug 2783](https://www.nsnam.org/bugzilla/show_bug.cgi?id=2783) for discussion.
* `Queue` has been redesigned as a template class object, where the type parameter specifies the type of items to be stored in the queue. As a consequence:
  * Being a subclass of Queue, `DropTailQueue` is a template class as well.
  * Network devices such as SimpleNetDevice, PointToPointNetDevice and CsmaNetDevice use a queue of type `Queue<Packet>` to store the packets to transmit. The SetQueue method of their helpers, however, can still be invoked as, e.g., `SetQueue ("ns3::DropTailQueue")` instead of, e.g., `SetQueue ("ns3::DropTailQueue<Packet>")`.
  * The attributes `Mode`, `MaxPackets` and `MaxBytes` are now defined by the QueueBase class (which Queue is derived from).
* Queue discs that can operate both in packet mode and byte mode (Red, CoDel, Pie) define their own enum QueueDiscMode instead of using QueueBase::QueueMode.
* The CoDel, PIE and RED queue discs require that the size of the internal queue is the same as the queue disc limit (previously, it was allowed to be greater than or equal).
* The default value of the `EnableBeaconJitter` attribute in ApWifiMac was changed from false to true.
* The NormalClose() callback of a TcpSocket object used to fire upon leaving `TIME_WAIT` state (`2*MSL` after FINs have been exchanged). It now fires upon entering `TIME_WAIT` state. Timing of the callback for the other path to state CLOSED (through `LAST_ACK`) has not been changed.

## Changes from ns-3.25 to ns-3.26

### New API

* A `SocketPriorityTag` is introduced to carry the packet priority. Such a tag is added to packets by sockets that support this mechanism (UdpSocketImpl, TcpSocketBase and PacketSocket). The base class Socket has a new SetPriority method to set the socket priority. When the IPv4 protocol is used, the priority is set based on the ToS. See the Socket options section of the Network model for more information.
* A `WifiNetDevice::SelectQueue` method has been added to determine the user priority of an MSDU. This method is called by the traffic control layer before enqueuing a packet in the queue disc, if a queue disc is installed on the outgoing device, or passing a packet to the device, otherwise. The user priority is set to the three most significant bits of the DS field (TOS field in case of IPv4 and Traffic Class field in case of IPv6). The packet priority carried by the SocketPriorityTag is set to the user priority.
* The `PfifoFastQueueDisc` classifies packets into bands based on their priority. See the `pfifo_fast` queue disc section of the Traffic Control Layer model for more information.
* A new class `SpectrumWifiPhy` has been introduced that makes use of the Spectrum module. Its functionality and API is currently very similar to that of the `YansWifiPhy`, especially because it reuses the same `InterferenceHelper` and `ErrorModel` classes (for this release). Some example programs in the `examples/wireless/` directory, such as `wifi-spectrum-per-example.cc`, illustrate how the SpectrumWifiPhy class can be substituted for the default `YansWifiPhy` PHY model.
* We have added support for generating traces for the [DES Metrics](https://wilseypa.github.io/desMetrics) project. These can be enabled by adding `--enable-des-metrics` at configuration; you must also use `CommandLine` in your script. See the API docs for class `DesMetrics` for more details.
* The traffic control module now includes the `FQ-CoDel` and `PIE` queue disc models, and behavior corresponding to Linux `Byte Queue Limits (BQL)`.
* Several new TCP congestion control variants were introduced, including `TCP Vegas`, `Scalable`, `Veno`, `Illinois`, `Bic`, `YeAH`, and `H-TCP` congestion control algorithms.

### Changes to existing API

* `SocketAddressTag` was a long-standing approach to approximate the POSIX socket recvfrom behavior (i.e., to know the source address of a packet) without actually calling RecvFrom. Experience with this revealed that this option was difficult to use with tunnels (the new tag has to replace the old one). Moreover, there is no real need to create a new API when there is a an existing one (i.e., RecvFrom). As a consequence, SocketAddressTag has been completely removed from ns-3. Users can use RecvFrom (for UDP), GetPeerName (for TCP), or similar.
* `InetSockAddress` can now store a ToS value, which can be set through its SetTos method. The Bind and Connect methods of UDP (UdpSocketImpl) and TCP (TcpSocketBase) sockets set the socket ToS value to the value provided through the address input parameter (of type InetSockAddress). See the Socket options section of the Network model for more information.
* The `QosTag` is removed as it has been superseded by the SocketPriorityTag.
* The `Ipv4L3Protocol::DefaultTos` attribute is removed.
* The attributes `YansWifiPhy::Frequency`, `YansWifiPhy::ChannelNumber`, and `YansWifiPhy::ChannelWidth`, and the related accessor methods, were moved to base class `WifiPhy`. `YansWifiPhy::GetChannelFrequencyMhz()` was deleted. A new method `WifiPhy::DefineChannelNumber ()` was added to allow users to define relationships between channel number, standard, frequency, and channel width.
* The class `WifiSpectrumValueHelper` has been refactored; previously it was an abstract base class supporting the WifiSpectrumValue5MhzFactory spectrum model. It now contains various static member methods supporting the creation of power spectral densities with the granularity of a Wi-Fi OFDM subcarrier bandwidth. The class `WifiSpectrumValue5MhzFactory` and its API remain but it is not subclassed.
* A new Wifi method `InterferenceHelper::AddForeignSignal` has been introduced to support use of the SpectrumWifiPhy (so that non-Wi-Fi signals may be handled as noise power).
* A new Wifi attribute `Dcf::TxopLimit` has been introduced to add support for 802.11e TXOP.

### Changes to build system

* A new waf build option, `--check-config`, was added to allow users to print the current configuration summary, as appears at the end of `./waf configure`. See bug 2459 for discussion.
* The configure summary is now sorted, to make it easier to check the status of optional features.

### Changed behavior

This section is for behavioral changes to the models that were not due to a bug fix.

* The relationship between Wi-Fi channel number, frequency, channel width, and Wi-Fi standard has been revised (see bug 2412). Previously, ChannelNumber and Frequency were attributes of class YansWifiPhy, and the frequency was defined as the start of the band. Now, Frequency has been redefined to be the center frequency of the channel, and the underlying device relies on the pair of frequency and channel width to control behavior; the channel number and Wi-Fi standard are used as attributes to configure frequency and channel width. The wifi module documentation discusses this change and the new behavior.
* AODV now honors the TTL in RREQ/RREP and it uses a method compliant with [RFC 3561](http://www.ietf.org/rfc/rfc3561.txt). The node search radius is increased progressively. This could increase slightly the node search time, but it also decreases the network congestion.

## Changes from ns-3.24 to ns-3.25

### New API

* In `src/internet/test`, a new environment is created to test TCP properties.
* The `src/traffic-control` module has been added, with new API for adding and configuring queue discs and packet filters.
* Related to traffic control, a new interface has been added to the NetDevice to provide a queue interface to access device queue state and register callbacks used for flow control.
* In `src/wifi`, a new rate control (MinstrelHT) has been added for 802.11n/ac modes.
* In `src/wifi`, a new helper (WifiMacHelper) is added and is a merged helper from all previously existing MAC helpers (NqosWifiMacHelper, QosWifiMacHelper, HtWifiMacHelper and VhtWifiMacHelper).
* It is now possible to use RIPv2 in IPv4 network simulations.

### Changes to existing API

* TCP-related changes:
  * Classes TcpRfc793, TcpTahoe, and TcpReno were removed.
  * The `TcpNewReno` log component was effectively replaced by `TcpCongestionOps`
* TCP Hybla and HighSpeed have been added.
* Added the concept of Congestion State Machine inside TcpSocketBase.
* Merged Fast Recovery and Fast Retransmit inside TcpSocketBase.
* Some member variables have been moved from TcpSocketBase inside TcpSocketState. Attributes are not touched.
* Congestion control split from TcpSocketBase as subclass of TcpCongestionOps.
* Added Rx and Tx callbacks on TcpSocketBase.
* Added BytesInFlight trace source on TcpSocketBase. The trace is updated when the implementation requests the value.
* Added attributes about the number of connection and data retransmission attempts.
* ns-3 is now capable of serializing SLL (a.k.a. cooked) headers. This is used in DCE to allow the generation of pcap directly readable by wireshark.
* In the WifiHelper class in the wifi module, Default has been declared deprecated. This is now immediately handled by the constructor of the class.
* The API for configuring 802.11n/ac aggregation has been modified to be more user friendly. As any MAC layer attributes, aggregation parameters can now also be configured through WifiMacHelper::SetType.
* The class Queue and subclasses derived from it have been changed in two ways:
  * Queues no longer enqueue simple Packets but instead enqueue QueueItem objects, which include Packet but possibly other information such as headers.
  * The attributes governing the mode of operation (packets or bytes) and the maximum size have been moved to base class Queue.
* Users of advanced queues (RED, CoDel) who have been using them directly in the NetDevice will need to adjust to the following changes:
  * RED and CoDel are no longer specializations of the Queue class, but are now specializations of the new QueueDisc class. This means that RED and CoDel can now be installed in the context of the new Traffic Control layer instead of as queues in (some) NetDevices. The reason for such a change is to make the ns-3 stack much more similar to that of real operating systems (Linux has been taken as a reference). Queuing disciplines such as RED and CoDel can now be tested with all the NetDevices, including WifiNetDevices.
  * NetDevices still use queues to buffer packets. The only subclass of Queue currently available for this purpose is DropTailQueue. If one wants to approximate the old behavior, one needs to set the DropTailQueue MaxPackets attribute to very low values, e.g., 1.
  * The Traffic Control layer features a mechanism by which packets dropped by the NetDevice are requeued in the queue disc (more precisely: if NetDevice::Send returns false, the packet is requeued), so that they are retransmitted later. This means that the MAC drop traces may include packets that have not been actually lost, because they have been dropped by the device, requeued by the traffic control layer and successfully retransmitted. To get the correct number of packets that have been actually lost, one has to subtract the number of packets requeued from the number of packets dropped as reported by the MAC drop trace.

### Changes to build system

* Waf was upgraded to 1.8.19
* A new waf build option, --check-profile, was added to allow users to check the currently active build profile. It is discussed in bug 2202 in the tracker.

### Changed behavior

This section is for behavioral changes to the models that were not due to a bug fix.

* TCP behavioral changes:
  * TCP closes connection after a number of failed segment retries, rather than trying indefinitely. The maximum number of retries, for both SYN attempts and data attempts, is controlled by attributes.
  * Congestion algorithms not compliant with Fast Retransmit and Fast Recovery (TCP 793, Reno, Tahoe) have been removed.
* 802.11n/ac MPDU aggregation is now enabled by default for both `AC_BE` and `AC_VI`.
* The introduction of the traffic control layer leads to some additional buffering by default in the stack; when a device queue fills up, additional packets become enqueued at the traffic control layer.

## Changes from ns-3.23 to ns-3.24

### New API

* In `src/wifi`, several changes were made to enable partial 802.11ac support:
* A new helper (VhtWifiMacHelper) was added to set up a Very high throughput (VHT) MAC entity.
* A new standard value has been added that enables the new 11ac data rates.
* A new 11ac preamble has been added.
* A new information element has been added: VhtCapabilities. This information element is added to the MAC frame header if the node is a VHT node. This VhtCapabilities information element is used to advertise the VHT capabilities of the node to other nodes in the network.
* The ArpCache API was extended to allow the manual removal of ArpCache entries and the addition of permanent (static) entries for IPv4.
* The SimpleChannel in the `network` module now allows per-NetDevice blacklists, in order to do hidden terminal testcases.

### Changes to existing API

* The signatures on several TcpHeader methods were changed to take const arguments.
* class TcpL4Protocol replaces Send() methods with SendPacket(), and adds new methods to AddSocket() and RemoveSocket() from a node. Also, a new PacketReceived() method was introduced to get the TCP header of an incoming packet and check its checksum.
* The CongestionWindow and SlowStartThreshold trace sources have been moved from the TCP subclasses such as NewReno, Reno, Tahoe, and Westwood to the TcpSocketBase class.
* The WifiMode object has been refactored:
  * 11n data rates are now renamed according to their MCS value. E.g. OfdmRate65MbpsBW20MHz has been renamed into HtMcs7. 11ac data rates have been defined according to this new renaming.
  * HtWifiMacHelper and VhtWifiMacHelper provide a helper to convert a MCS value into a data rate value.
  * The channel width is no longer tied to the wifimode. It is now included in the TXVECTOR.
  * The physical bitrate is no longer tied to the wifimode. It is computed based on the selected wifimode and on the TXVECTOR parameters (channel width, guard interval and number of spatial streams).

### Changes to build system

* Waf was upgraded to 1.8.12
* Waf scripts and test.py test runner program were made compatible with Python 3

### Changed behavior

This section is for behavioral changes to the models that were not due to a bug fix.

## Changes from ns-3.22 to ns-3.23

### New API

* The mobility module includes a GeographicPositions class used to convert geographic to cartesian coordinates, and to generate randomly distributed geographic coordinates.
* The spectrum module includes new TvSpectrumTransmitter classes and helpers to create television transmitter(s) that transmit PSD spectrums customized by attributes such as modulation type, power, antenna type, channel frequency, etc.

### Changes to existing API

* In LteSpectrumPhy, LtePhyTxEndCallback and the corresponding methods have been removed, since they were unused.
* In the DataRate class in the network module, CalculateTxTime has been declared deprecated. CalculateBytesTxTime and CalculateBitsTxTime are to be used instead. The return value is a Time, instead of a double.
* In the Wi-Fi InterferenceHelper, the interference event now takes the WifiTxVector as an input parameter, instead of the WifiMode. A similar change was made to the WifiPhy::RxOkCallback signature.

### Changes to build system

* None

### Changed behavior

This section is for behavioral changes to the models that were not due to a bug fix.

* In Wi-Fi, HT stations (802.11n) now support two-level aggregation. The InterferenceHelper now distinguishes between the PLCP and regular payload reception, for higher fidelity modeling. ACKs are now sent using legacy rates and preambles. Access points now establish BSSBasicRateSet for control frame transmissions. PLCP header and PLCP payload reception have been decoupled to improve PHY layer modeling accuracy. RTS/CTS with A-MPDU is now fully supported.
* The mesh module was made more compliant to the IEEE 802.11s-2012 standard and packet traces are now parseable by Wireshark.

## Changes from ns-3.21 to ns-3.22

### New API

* New classes were added for the PARF and APARF WiFi power and rate control mechanisms.
* Support for WiFi 802.11n MPDU aggregation has been added.
* Additional support for modeling of vehicular WiFi networks has been added, including the channel-access coordination feature of IEEE 1609.4. In addition, a Basic Safety Message (BSM) packet generator and related statistics-gathering classes have been added to the wave module.
* A complete LTE release bearer procedure is now implemented which can be invoked by calling the new helper method LteHelper::DeActivateDedicatedEpsBearer ().
* It is now possible to print the Neighbor Cache (ARP and NDISC) by using the RoutingProtocolHelper
* A TimeProbe class has been added to the data collection framework in the stats module, enabling TracedValues emitting values of type ns3::Time to be handled by the framework.
* A new attribute `ClockGranularity` was added to the TcpSocketBase class, to control modeling of RTO calculation.

### Changes to existing API

* Several deprecated classes and class methods were removed, including `EmuNetDevice`, `RandomVariable` and derived classes, `Packet::PeekData()`, `Ipv6AddressHelper::NewNetwork(Ipv6Address, Ipv6Prefix)`, `Ipv6InterfaceContainer::SetRouter()`, `Ipv4Route::GetOutputTtl()`, `TestCase::AddTestCase(TestCase*)`, and `TestCase::GetErrorStatus()`.
* Print methods involving routing tables and neighbor caches, in classes `Ipv4RoutingHelper` and `Ipv6RoutingHelper`, were converted to static methods.
* `PointerValue` attribute types in class `UanChannel (NoiseModel)`, `UanPhyGen (PerModel and SinrModel)`, `UanPhyDual (PerModelPhy1, PerModelPhy2, SinrModelPhy1, and SinrModelPhy2)`, and `SimpleNetDevice (TxQueue)`, were changed from `PointerValue` type to `StringValue` type, making them configurable via the `Config` subsystem.
* `WifiPhy::CalculateTxDuration()` and `WifiPhy::GetPayloadDurationMicroSeconds ()` now take an additional frequency parameter.
* The attribute `Recievers` in class `YansWifiPhy` was misspelled, so this has been corrected to `Receivers`.
* We have now documented the callback function signatures for all `TracedSources`, using an extra (fourth) argument to `TypeId::AddTraceSource` to pass the fully-qualified name of the signature typedef. To ensure that future TraceSources are similarly documented, the three argument version of AddTraceSource has been deprecated.
* The `MinRTO` attribute of the RttEstimator class was moved to the TcpSocketBase class. The `Gain` attribute of the RttMeanDeviation class was replaced by new `Alpha` and `Beta` attributes.
* Attributes of the TcpTxBuffer and TcpRxBuffer class are now accessible through the TcpSocketBase class.
* The LrWpanHelper class has a new constructor allowing users to configure a MultiModelSpectrumChannel as an option, and also provides Set/Get API to allow users to access the underlying channel object.

### Changes to build system

* waf was upgraded to version 1.7.16

### Changed behavior

This section is for behavioral changes to the models that were not due to a bug fix.

* The default value of the `Speed` attribute of `ConstantSpeedPropagationDelayModel` was changed from `300,000,000 m/s` to `299,792,458 m/s` (speed of light in a vacuum), causing propagation delays using this model to vary slightly.
* The `LrWpanHelper` object was previously instantiating only a `LogDistancePropagationLossModel` on a `SingleModelSpectrumChannel`, but no `PropagationDelayModel`. The constructor now adds by default a `ConstantSpeedPropagationDelayModel`.
* The Nix-vector routing implementation now uses a lazy flush mechanism, which dramatically speeds up the creation of large topologies.

## Changes from ns-3.20 to ns-3.21

### New API

* New `const double& SpectrumValue::operator[] (size_t index) const`.
* A new TraceSource has been added to TCP sockets: SlowStartThreshold.
* New method CommandLine::AddValue (name, attributePath) to provide a shorthand argument `name` for the Attribute `path`. This also has the effect of including the help string for the Attribute in the Usage message.
* The GSoC 2014 project in the LTE module has brought some additional APIs:
  * a new abstract class LteFfrAlgorithm, which every future implementation of frequency reuse algorithm should inherit from
  * a new SAPs: one between MAC Scheduler and FrAlgorithm, one between RRC and FrAlgorithm
  * new attribute to enable Uplink Power Control in LteUePhy
  * new LteUePowerControl class, an implementation of Uplink Power Control, which is configurable by attributes. ReferenceSignalPower is sent by eNB in SIB2. Uplink Power Control in Closed Loop Accumulative Mode is enabled by default
  * seven different Frequency Reuse Algorithms (each has its own attributes):
    * LteFrNoOpAlgorithm
    * LteFrHardAlgorithm
    * LteFrStrictAlgorithm
    * LteFrSoftAlgorithm
    * LteFfrSoftAlgorithm
    * LteFfrEnhancedAlgorithm
    * LteFfrDistributedAlgorithm
  * attribute in LteFfrAlgorithm to set FrCellTypeId which is used in automatic Frequency Reuse algorithm configuration
  * LteHelper has been updated with new methods related to frequency reuse algorithm: SetFfrAlgorithmType and SetFfrAlgorithmAttribute
* A new SimpleNetDeviceHelper can now be used to install SimpleNetDevices.
* New PacketSocketServer and PacketSocketClient apps, meant to be used in tests.
* Tcp Timestamps and Window Scale options have been added and are enabled by default (controllable by attribute).
* A new CoDel queue model has been added to the `internet` module.
* New test macros `NS_TEST_ASSERT_MSG_GT_OR_EQ()` and `NS_TEST_EXPECT_MSG_GT_OR_EQ()` have been added.

### Changes to existing API

* `Icmpv6L4Protocol::ForgeEchoRequest` is now returning a packet with the proper IPv6 header.
* The TCP socket Attribute `SlowStartThreshold` has been renamed `InitialSlowStartThreshold` to clarify that the effect is only on the initial value.
* all schedulers were updated to interact with FR entity via FFR-SAP. Only PF, PSS, CQA, FD-TBFQ, TD-TBFQ schedulers supports Frequency Reuse functionality. In the beginning of scheduling process, schedulers ask FR entity for available RBGs and then ask if UE can be scheduled on RB
* eNB RRC interacts with FFR entity via RRC-FFR SAP
* new DL-CQI generation approach was implemented. Now DL-CQI is computed from control channel as signal and data channel (if received) as interference. New attribute in LteHelper was added to specify DL-CQI generation approach. New approach is default one in LteHelper
* RadioEnvironmentMap can be generated for Data or Control channel and for specified RbId; Data or Control channel and RbId can be configured by new attributes in RadioEnvironmentMapHelper
* lte-sinr-chunk-processor refactored to lte-chunk-processor. Removed all lte-xxx-chunk-processor implementations
* BindToNetDevice affects also sockets using IPv6.
* BindToNetDevice now calls implicitly Bind (). To bind a socket to a NetDevice and to a specific address, the correct sequence is Bind (address) - BindToNetDevice (device). The opposite will raise an error.

### Changes to build system

* None for this release.

### Changed behavior

* Behavior will be changed due to the list of bugs fixed (listed in [RELEASE_NOTES.md](RELEASE_NOTES.md)); users are requested to review that list as well.

## Changes from ns-3.19 to ns-3.20

### New API

* Models have been added for low-rate, wireless personal area networks (LR-WPAN) as specified by IEEE standard 802.15.4 (2006). The current emphasis is on the unslotted mode of 802.15.4 operation for use in Zigbee, and the scope is limited to enabling a single mode (CSMA/CA) with basic data transfer capabilities. Association with PAN coordinators is not yet supported, nor the use of extended addressing. Interference is modeled as AWGN but this is currently not thoroughly tested. The NetDevice Tx queue is not limited, i.e., packets are never dropped due to queue becoming full. They may be dropped due to excessive transmission retries or channel access failure.
* A new IPv6 routing protocol has been added: RIPng. This protocol is an Interior Gateway Protocol and it is available in the Internet module.
* A new LTE MAC downlink scheduling algorithm named Channel and QoS Aware (CQA) Scheduler is provided by the new `ns3::CqaFfMacScheduler` object.
* Units may be attached to Time objects, to facilitate specific output formats (see Time::As())
* FlowMonitor `SerializeToXml` functions are now directly available from the helper.
* Access to OLSR's HNA table has been enabled

### Changes to existing API

* The SixLowPan model can now use uncompressed IPv6 headers. An option to define the minimum compressed packet size has been added.
* MinDistance wsa replaced by MinLoss in FriisPropagationLossModel, to better handle conditions outside of the assumed far field region.
* In the DSR model, the attribute `DsrOptionRerrHeader::ErrorType` has been removed.

### Changes to build system

* Python 3.3 is now supported for Python bindings for ns-3. Python 3.3 support for API scanning is not supported. Python 3.2 is not supported.
* Enable selection of high precision `int64x64_t` implementation at configure time, for debugging purposes.
* Optimized builds are now enabling signed overflow optimization (`-fstrict-overflow`) and for gcc 4.8.2 and greater, also warning for cases where an optimizization may occur due to compiler assumption that overflow will not occur.

### Changed behavior

* The Internet FlowMonitor can now track IPv6 packets.
* `Ipv6Extension::m_dropTrace` has been removed. `Ipv6L3Protocol::m_dropTrace` is now fired when appropriate.
* IPv4 identification field value is now dependent on the protocol field.
* Point-to-point trace sources now contain PPP headers

## Changes from ns-3.18.1 to ns-3.19

### New API

* A new wifi extension for vehicular simulation support is available in the src/wave directory. The current code represents an interim capability to realize an IEEE 802.11p-compliant device, but without the WAVE extensions (which are planned for a later patch). The WaveNetDevice modelled herein enforces that a WAVE-compliant physical layer (at 5.9 GHz) is selected, and does not require any association between devices (similar to an adhoc WiFi MAC), but is otherwise similar (at this time) to a WifiNetDevice. WAVE capabililties of switching between control and service channels, or using multiple radios, are not yet modelled.
* New SixLowPanNetDevice class providing a shim between IPv6 and real NetDevices. The new module implements 6LoWPAN: "Transmission of IPv6 Packets over IEEE 802.15.4 Networks" (see [RFC 4944](http://www.ietf.org/rfc/rfc4944.txt) and [RFC 6262](http://www.ietf.org/rfc/rfc6262.txt)), resulting in a heavy header compression for IPv6 packets. The module is intended to be used on 802.15.4 NetDevices, but it can be used over other NetDevices. See the manual for further discussion.
* LteHelper has been updated with some new APIs:
  * new overloaded Attach methods to enable UE to automatically determine the eNodeB to attach to (using initial cell selection);
  * new methods related to handover algorithm: SetHandoverAlgorithmType and SetHandoverAlgorithmAttribute;
  * a new attribute AnrEnabled to activate/deactivate Automatic Neighbour Relation (ANR) function; and
  * a new method SetUeDeviceAttribute for configuring LteUeNetDevice.
* The GSoC 2013 project in the LTE module has brought some additional APIs:
  * a new abstract class LteHandoverAlgorithm, which every future implementation of automatic handover trigger should inherit from;
  * new classes LteHandoverAlgorithm and LteAnr as sub-modules of LteEnbNetDevice class; both interfacing with the LteEnbRrc sub-module through Handover Management SAP and ANR SAP;
  * new attributes in LteEnbNetDevice and LteUeNetDevice classes related to Closed Subscriber Group (CSG) functionality in initial cell selection;
  * new attributes in LteEnbRrc for configuring UE measurements' filtering coefficient (i.e., quantity configuration);
  * a new public method AddUeMeasReportConfig in LteEnbRrc for setting up custom UE measurements' reporting configuration; measurement reports can then be captured from the RecvMeasurementReport trace source; and
  * new trace sources in LteUeRrc to capture more events, such as System Information messages (MIB, SIB1, SIB2), initial cell selection, random access, and handover.
* A new parallel scheduling algorithm based on null messages, a common parallel DES scheduling algorithm, has been added. The null message scheduler has better scaling properties when running on some scenarios with large numbers of nodes since it does not require a global communication.

### Changes to existing API

* It is now possible to use `Ipv6PacketInfoTag` from UDP applications in the same way as with Ipv4PacketInfoTag. See Doxygen for current limitations in using `Ipv[4,6]PacketInfoTag` to set IP properties.
* A change is introduced for the usage of the `EpcHelper` class. Previously, the `EpcHelper` class included both the API definition and its (only) implementation; as such, users would instantiate and use the `EpcHelper` class directly in their simulation programs. From now on, `EpcHelper` is just the base class defining the API, and the implementation has been moved to derived classes; as such, users are now expected to use one of the derived classes in their simulation program. The implementation previously provided by the `EpcHelper` class has been moved to the new derived class `PointToPointEpcHelper`.
* The automatic handover trigger and ANR functions in LTE module have been moved from `LteEnbRrc` class to separate classes. As a result, the related attributes, e.g., `ServingCellHandoverThreshold`, `NeighbourCellHandoverOffset`, `EventA2Threshold`, and `EventA4Threshold` have been removed from `LteEnbRrc` class. The equivalent attributes are now in `A2A4RsrqHandoverAlgorithm` and `LteAnr` classes.
* Master Information Block (MIB) and System Information Block Type 1 (SIB1) are now transmitted as LTE control messages, so they are no longer part of RRC protocol.
* UE RRC state model in LTE module has been considerably modified and is not backward compatible with the previous state model.
* Additional time units (Year, Day, Hour, Minute) were added to the time value class that represents simulation time; the largest unit prior to this addition was Second.
* SimpleNetDevice and SimpleChannel are not so simple anymore. SimpleNetDevice can be now a Broadcast or PointToPoint NetDevice, it can have a limited bandwidth and it uses an output queue.

### Changes to build system

### Changed behavior

* For the TapBridge device, in UseLocal mode there is a MAC learning function. TapBridge has been waiting for the first packet received from tap interface to set the address of the bridged device to the source address of the first packet. This has caused problems with WiFi. The new behavior is that after connection to the tap interface, ns-3 learns the MAC address of that interface with a system call and immediately sets the address of the bridged device to the learned one. See [bug 1777](https://www.nsnam.org/bugzilla/show_bug.cgi?id=1777) for more details.
* TapBridge device now correctly implements IsLinkUp() method.
* IPv6 addresses and routing tables are printed like in Linux `route -A inet6` command.
* A change in `Ipv[4,6]Interface` enforces the correct behaviour of IP when a device do not support the minimum MTU requirements. This is set to 68 and 1280 octets respectively. IP simulations that may have run over devices with smaller MTUs than 68 or 1280, respectively, will no longer be able to use such devices.

## Changes from ns-3.18 to ns-3.18.1

### New API

* It is now possible to randomize the time of the first beacon from an access point. Use an attribute `EnableBeaconJitter` to enable/disable this feature.
* A new FixedRoomPositionAllocator helper class is available; it allows one to generate a random position uniformly distributed in the volume of a chosen room inside a chosen building.

### Changes to existing API

* Logging wildcards: allow `***` as synonym for `*=**` to turn on all logging.
* The log component list (`NS_LOG=print-list`) is now printed alphabetically.
* Some deprecated IEEE 802.11p code has been removed from the wifi module

### Changes to build system

* The Python API scanning system (`./waf --apiscan`) has been fixed (bug 1622)
* Waf has been upgraded from 1.7.11 to 1.7.13

### Changed behavior

* Wifi simulations have additional jitter on AP beaconing (see above) and some bug fixes have been applied to wifi module (see [RELEASE_NOTES.md](RELEASE_NOTES.md))

## Changes from ns-3.17 to ns-3.18

### New API

* New features have been added to the LTE module:
  * PHY support for UE measurements (RSRP and RSRQ)
  * RRC support for UE measurements (configuration, execution, reporting)
  * Automatic Handover trigger based on RRC UE measurement reports
* Data collection components have been added in the `src/stats` module. Data collection includes a Probe class that attaches to ns-3 trace sources to filter their output, and two Aggregator classes for marshaling probed data into text files or gnuplot plots. The ns-3 tutorial has been extended to illustrate basic functionality.
* In `src/wifi`, several changes were made to enable partial 802.11n support:
  * A new helper (HtWifiMacHelper) was added to set up a High Throughput (HT) MAC entity
  * New attributes were added to help the user setup a High Throughput (HT) PHY entity. These attributes can be set using the YansWifiPhyHelper
  * A new standard value has been added that enables the new 11n data rates.
  * New 11n preambles has been added (Mixed format and greenfield). To be able to change Tx duration according to the preamble used, a new class TxVector has been added to carry the transmission parameters (mode, preamble, stbc,..). Several functions have been updated to allow the passage of TxVector instead of WifiMode in MacLow, WifiRemoteStationManager, WifiPhy, YansWifiPhy,..
  * A new information element has been added: HTCapabilities. This information element is added to the MAC frame header if the node is an HT node. This HTCapabilities information element is used to advertise the HT capabilities of the node to other nodes in the network
* InternetStackHelper has two new functions:SetIpv4ArpJitter (bool enable) and SetIpv6NsRsJitter (bool enable) to enable/disable the random jitter on the transmission of IPv4 ARP Request and IPv6 NS/RS.
* Bounds on valid time inputs for time attributes can now be enabled. See attribute-test-suite.cc for an example.
* New generic hash function interface provided in the simulation core. Two hash functions are provided: murmur3 (default), and the venerable FNV1a. See the Hash Functions section in the ns-3 manual.
* New Mac16Address has been added. It can be used with IPv6 to make an Autoconfigured address.
* Mac64Address support has been extended. It can now be used with IPv6 to make an Autoconfigured address.
* IPv6 can now detect and use Path-MTU. See examples/ipv6/fragmentation-ipv6-two-MTU.cc for an example.
* Radvd application has a new Helper. See the updated examples/ipv6/radvd.cc for an example.

### Changes to existing API

* The Ipv6InterfaceContainer functions to set a node in forwarding state (i.e., a router) and to install a default router in a group of nodes have been extensively changed. The old function `void Ipv6InterfaceContainer::SetRouter (uint32_t i, bool router)` is now DEPRECATED.
* The documentation's IPv6 addresses (2001:db8::/32, RFC 3849) are now dropped by routers.
* The `src/tools` module has been removed, and most files migrated to `src/stats`. For users of these programs (the statistics-processing in average.h, or the gnuplot support), the main change is likely to be replacing the inclusion of `tools-module.h` with `stats-module.h`. Users of the event garbage collector, previously in tools, will now include it from the core module.
* The Ipv6 UnicastForwardCallback and MulticastForwardCallback have a new parameter, the NetDevice the packet has been received from. Existing Ipv6RoutingProtocols should update their RouteInput function accordingly, e.g., from ucb (rtentry, p, header); to ucb (idev, rtentry, p, header);
* The previous buildings module relied on a specific MobilityModel called BuildingsMobilityModel, which supported buildings but only allowed static positions. This mobility model has been removed. Now, the Buildings module instead relies on a new class called MobilityBuildingInfo which can be aggregated to any MobilityModel. This allows having moving nodes in presence of buildings with any of the existing MobilityModels.
* All functions in WifiRemoteStationManager named GetXxxMode have been changed to GetXxxTxVector

### Changes to build system

* Make references to bug id's in doxygen comments with `\bugid{num}`, where num is the bug id number. This form will generate a link to the bug in the bug database.

### Changed behavior

* Now it is possible to request printing command line arguments to the desired output stream using `PrintHelp` or `operator <<`

  ```cpp
  CommandLine cmd;
  cmd.Parse (argc, argv);
  ...

  std::cerr << cmd;
  ```

  or

  ```cpp
  cmd.PrintHelp (std::cerr);
  ```

* Command line boolean arguments specified with no integer value (e.g. `--boolArg`) will toggle the value from the default, instead of always setting the value to true.
* IPv4's ARP Request and IPv6's NS/RS are now transmitted with a random delay. The delay is, by default, a uniform random variable in time between 0 and 10ms. This is aimed at preventing reception errors due to collisions during wifi broadcasts when the sending behavior is synchronized (e.g. due to applications starting at the same time on several different nodes). This behaviour can be modified by using ArpL3Protocol's `RequestJitter` and Icmpv6L4Protocol's `SolicitationJitter` attributes or by using the new `InternetStackHelper` functions.
* AODV Hellos are disabled by default. The performance with Hellos enabled and disabled are almost identical. With Hellos enabled, AODV will suppress hellos from transmission, if any recent broadcast such as RREQ was transmitted. The attribute n`s3::aodv::RoutingProtocol::EnableHello` can be used to enable/disable Hellos.

## Changes from ns-3.16 to ns-3.17

### New API

* New TCP Westwood and Westwood+ models
* New FdNetDevice class providing a special NetDevice that is able to read and write traffic from a file descriptor. Three helpers are provided to associate the file descriptor with different underlying devices:
  * `EmuFdNetDeviceHelper` (to associate the ns/3 device with a physical device in the host machine). This helper is intended to eventually replace the EmuNetDevice in src/emu.
  * `TapFdNetDeviceHelper` (to associate the ns-3 device with the file descriptor from a tap device in the host machine)
  * `PlanteLabFdNetDeviceHelper` (to automate the creation of tap devices in PlanetLab nodes, enabling ns/3 simulations that can send and receive traffic though the Internet using PlanetLab resource.
* In `Ipv4ClickRouting`, the following APIs were added:
  * `Ipv4ClickRouting::SetDefines()`, accessible through `ClickInternetStackHelper::SetDefines()`, for the user to set Click defines from the ns-3 simulation file.
  * `SIMCLICK_GET_RANDOM_INT` click-to-simulator command for ns-3 to drive Click's random number generation.
* LTE module
  * New user-visible LTE API
    * Two new methods have been added to LteHelper to enable the X2-based handover functionality: AddX2Interface, which setups the X2 interface between two eNBs, and HandoverRequest, which is a convenience method that schedules an explicit handover event to be executed at a given point in the simulation.
    * the new LteHelper method EnablePhyTraces can now be used to enable the new PHY traces
  * New internal LTE API
    * New LTE control message classes DlHarqFeedbackLteControlMessage, RachPreambleLteControlMessage, RarLteControlMessage, MibLteControlMessage
    * New class UeManager
    * New LteRadioBearerInfo subclasses LteSignalingRadioBearerInfo, LteDataRadioBearerInfo
    * New LteSinrChunkProcessor subclasses LteRsReceivedPowerChunkProcessor, LteInterferencePowerChunkProcessor
* New DSR API
  * Added PassiveBuffer class to save maintenance packet entry for passive acknowledgment option
  * Added FindSourceEntry function in RreqTable class to keep track of route request entry received from same source node
  * Added NotifyDataReceipt function in DsrRouting class to notify the data receipt of the next hop from link layer. This is used for the link layer acknowledgment.
* New Tag, PacketSocketTag, to carry the destination address of a packet and the packet type
* New Tag, DeviceNameTag, to carry the ns-3 device name from where a packet is coming
* New Error Model, BurstError model, to determine which bursts of packets are errored corresponding to an underlying distribution, burst rate, and burst size

### Changes to existing API

* ns3::Object and subclasses DoStart has been renamed to DoInitialize
* ns3::Object and subclasses Start has been renamed to Initialize
* EnergySource StartDeviceModels renamed to InitializeDeviceModels
* A typo was fixed in an LTE variable name. The variable ns3::AllocationRetentionPriority::preemprionVulnerability was changed to preemptionVulnerability.
* Changes in TestCase API
  * TestCase has new enumeration TestDuration containing `QUICK`, `EXTENSIVE`, `TAKES_FOREVER`
* TestCase constructor now requires TestDuration, old constructor marked deprecated
* Changes in LTE API
  * User-visible LTE API
    * The previous LteHelper method ActivateEpsBearer has been now replaced by two alternative methods: ActivateDataRadioBearer (to be used when the EPC model is not used) and ActivateDedicatedEpsBearer (to be used when the EPC model is used). In the case where the EPC model is used, the default EPS bearer is not automatically activated without the need for a specific method to be called.
  * Internal LTE API
    * EpcHelper added methods AddUe, AddX2Interface. Method AddEnb now requires a cellId. Signature of ActivateEpsBearer changed to `void ActivateEpsBearer (Ptr ueLteDevice, uint64_t imsi, Ptr tft, EpsBearer bearer)`
    * LteHelper added methods EnableDlPhyTraces, EnableUlPhyTraces, EnableDlTxPhyTraces, EnableUlTxPhyTraces, EnableDlRxPhyTraces, EnableUlRxPhyTraces
    * LteHelper removed methods EnableDlRlcTraces, EnableUlRlcTraces, EnableDlPdcpTraces, EnableUlPdcpTraces
    * RadioBearerStatsCalculator added methods (Set/Get)StartTime, (Set/Get)Epoch, RescheduleEndEpoch, EndEpoch
    * RadioBearerStatsCalculator removed methods StartEpoch, CheckEpoch
    * RadioBearerStatsCalculator methods UlTxPdu, DlRxPdu now require a cellId
    * EpcEnbApplication constructor now requires Ipv4Addresses enbS1uAddress and sgwS1uAddress as well as cellId
    * EpcEnbApplication added methods SetS1SapUser, GetS1SapProvider, SetS1apSapMme and GetS1apSapEnb
    * EpcEnbApplication removed method ErabSetupRequest
    * EpcSgwPgwApplication added methods SetS11SapMme, GetS11SapSgw, AddEnb, AddUe, SetUeAddress
    * lte-common.h new structs PhyTransmissionStatParameters and PhyReceptionStatParameters used in TracedCallbacks
    * LteControlMessage new message types `DL_HARQ`, `RACH_PREAMBLE`, `RAR`, `MIB`
    * LteEnbCmacSapProvider new methods RemoveUe, GetRachConfig, AllocateNcRaPreamble, AllocateTemporaryCellRnti
    * LteEnbPhy new methods GetLteEnbCphySapProvider, SetLteEnbCphySapUser, GetDlSpectrumPhy, GetUlSpectrumPhy, CreateSrsReport
    * LteEnbPhy methods DoSendMacPdu, DoSetTransmissionMode, DoSetSrsConfigurationIndex, DoGetMacChTtiDelay, DoSendLteControlMessage, AddUePhy, DeleteUePhy made private
    * LteEnbPhySapProvider removed methods SetBandwidth, SetTransmissionMode, SetSrsConfigurationIndex, SetCellId
    * LteEnbPhySapUser added methods ReceiveRachPreamble, UlInfoListElementHarqFeedback, DlInfoListElementHarqFeedback
    * LtePdcp added methods (Set/Get)Status
    * LtePdcp DoTransmitRrcPdu renamed DoTransmitPdcpSdu
    * LteUeRrc new enum State. New methods SetLteUeCphySapProvider, GetLteUeCphySapUser, SetLteUeRrcSapUser, GetLteUeRrcSapProvider, GetState, GetDlEarfcn, GetDlBandwidth, GetUlBandwidth, GetCellId, SetUseRlcSm . GetRnti made const.
    * LteUeRrc removed methods ReleaseRadioBearer, GetLcIdVector, SetForwardUpCallback, DoRrcConfigurationUpdateInd
    * LtePdcpSapProvider struct TransmitRrcPduParameters renamed TransmitPdcpSduParameters. Method TransmitRrcPdu renamed TransmitPdcpSdu
    * LtePdcpSapUser struct ReceiveRrcPduParameters renamed ReceivePdcpSduParameters. Method ReceiveRrcPdu renamed TransmitPdcpSdu
    * LtePdcpSpecificLtePdcpSapProvider method TransmitRrcPdu renamed TransmitPdcpSdu
    * LtePdcpSpecificLtePdcpSapUser method ReceiveRrcPdu renamed ReceivePdcpSdu. Method ReceiveRrcPdu renamed ReceivePdcpSdu
    * LtePhy removed methods DoSetBandwidth and DoSetEarfcn
    * LtePhy added methods ReportInterference and ReportRsReceivedPower
    * LteSpectrumPhy added methods SetHarqPhyModule, Reset, SetLtePhyDlHarqFeedbackCallback, SetLtePhyUlHarqFeedbackCallback, AddRsPowerChunkProcessor, AddInterferenceChunkProcessor
    * LteUeCphySapProvider removed methods ConfigureRach, StartContentionBasedRandomAccessProcedure, StartNonContentionBasedRandomAccessProcedure
    * LteUeMac added method AssignStreams
    * LteUeNetDevice methods GetMac, GetRrc, GetImsi made const
    * LteUeNetDevice new method GetNas
    * LteUePhy new methods GetLteUeCphySapProvider, SetLteUeCphySapUser, GetDlSpectrumPhy, GetUlSpectrumPhy, ReportInterference, ReportRsReceivedPower, ReceiveLteDlHarqFeedback
    * LteUePhy DoSendMacPdu, DoSendLteControlMessage, DoSetTransmissionMode, DoSetSrsConfigurationIndex made private
    * LteUePhySapProvider removed methods SetBandwidth, SetTransmissionMode, SetSrsConfigurationIndex
    * LteUePhySapProvider added method SendRachPreamble

* AnimationInterface method EnableIpv4RouteTracking returns reference to calling AnimationInterface object
* To make the API more uniform across the various PropagationLossModel classes, the Set/GetLambda methods of the FriisPropagationLossModel and TwoRayGroundPropagationLossModel classes have been changed to Set/GetFrequency, and now a Frequency attribute is exported which replaces the pre-existing Lambda attribute. Any previous user code setting a value for Lambda should be changed to set instead a value of Frequency = C / Lambda, with C = 299792458.0.

### Changes to build system

* Waf shipped with ns-3 has been upgraded to version 1.7.10 and custom pkg-config generator has been replaced by Waf's builtin tool.

### Changed behavior

* DSR link layer notification has changed. The model originally used `TxErrHeader` in Ptr to indicate the transmission error of a specific packet in link layer; however, it was not working correctly. The model now uses a different path to implement the link layer notification mechanism; specifically, looking into the trace file to find packet receive events. If the model finds one receive event for the data packet, it is used as the indicator for successful data delivery.

## Changes from ns-3.15 to ns-3.16

### New API

* In the Socket class, the following functions were added:
  * (Set/Get)IpTos - sets IP Type of Service field in the IP headers.
  * (Set/Is)IpRecvTos - tells the socket to pass information about IP ToS up the stack (by adding SocketIpTosTag to the packet).
  * (Set/Get)IpTtl - sets IP Time to live field in the IP headers.
  * (Set/Is)RecvIpTtl - tells the socket to pass information about IP TTL up the stack (by adding SocketIpTtlTag to the packet).
  * (Set/Is)Ipv6Tclass - sets Traffic Class field in the IPv6 headers.
  * (Set/Is)Ipv6RecvTclass - tells the socket to pass information about IPv6 TCLASS up the stack (by adding SocketIpv6TclassTag to the packet).
  * (Set/Get)Ipv6HopLimit - sets Hop Limit field in the IPv6 headers.
  * (Set/Is)Ipv6RecvHopLimit - tells the socket to pass information about IPv6 HOPLIMIT up the stack (by adding SocketIpv6HoplimitTag to the packet).A user can call these functions to set/get the corresponding socket option. See examples/socket/socket-options-ipv4.cc and examples/socket/socket-options-ipv6.cc for examples.

### Changes to existing API

* In the MobilityHelper class, the functions EnableAscii () and EnableAsciiAll () were changed to use output stream wrappers rather than standard C++ ostreams. The purpose of this change was to make them behave analogously to other helpers in ns-3 that generate ascii traces. Now, the file stream that is open in MobilityHelper is closed nicely upon asserts and program exits.

### Changes to build system

* It's now possible to use distcc when building ns-3. See tutorial for details.

### Changed behavior

* Sending a packet through Ipv4RawSocket now supports checksum in the Ipv4Header. It is still not possible to manually put in arbitrary checksum as the checksum is automatically calculated at Ipv4L3Protocol. The user has to enable checksum globally for this to work. Simply calling Ipv4Header::EnableChecksum() for a single Ipv4Header will not work.
* Now MultiModelSpectrumChannel allows a SpectrumPhy instance to change SpectrumModel at runtime by issuing a call to MultiModelSpectrumChannel::AddRx (). Previously, MultiModelSpectrumChannel required each SpectrumPhy instance to stick with the same SpectrumModel for the whole simulation.

## Changes from ns-3.14 to ns-3.15

### New API

* A RandomVariableStreamHelper has been introduced to assist with using the Config subsystem path names to assign fixed stream numbers to RandomVariableStream objects.

### Changes to existing API

* Derived classes of RandomVariable (i.e. the random variable implementations) have been ported to a new RandomVariableStream base class.
* For a given distribution DistributionVariable (such as UniformVariable), the new class name is DistributionRandomVariable (such as UniformRandomVariable).
* The new implementations are also derived from class ns3::Object and are handled using the ns-3 smart pointer (`Ptr`) class.
* The new variable classes also have a new attributed called `Stream` which allows them to be assigned to a fix stream index when assigned to the underlying pseudo-random stream of numbers.

### Changes to build system

### Changed behavior

* Programs using random variables or models that include random variables may exhibit changed output for a given run number or seed, due to a possible change in the order in which random variables are assigned to underlying pseudo-random sequences. Consult the manual for more information regarding this.

## Changes from ns-3.13 to ns-3.14

### New API

* The new class AntennaModel provides an API for modeling the radiation pattern of antennas.
* The new buildings module introduces an API (classes, helpers, etc) to model the presence of buildings in a wireless network topology.
* The LENA project's implementation of the LTE Mac Scheduler Interface Specification standardized by the Small Cell Forum (formerly Femto Forum) is now available for use with the LTE module.

### Changes to existing API

* The Ipv6RawSocketImpl `IcmpFilter` attribute has been removed. Six new member functions have been added to enable the same functionality.
* IPv6 support for TCP and UDP has been implemented. Socket functions that take an address (e.g. `Send ()`, `Connect ()`, `Bind ()`() can accept an `ns3::Ipv6Address` or a `ns3::Address` in addition to taking an `ns3::Ipv4Address`. (Note that the `ns3::Address` must contain a `ns3::Ipv6Address` or a `ns3::Ipv4Address`, otherwise these functions will return an error). Internally, the socket now stores the remote address as a type `ns3::Address` instead of a type `ns3::Ipv4Address`. The IPv6 Routing Header extension is not currently supported in ns-3 and will not be reflected in the TCP and UDP checksum calculations per RFC 2460. Also note that UDP checksums for IPv6 packets are required per RFC, but remain optional and disabled by default in ns-3 (in the interest of performance).
* When calling `Bind ()` on a socket without an address, the behavior remains the same: it will bind to the IPv4 "any" address (0.0.0.0). In order to `Bind ()` to the IPv6 "any" address in a similar fashion, use `Bind6 ()`.
* The prototype for the `RxCallback` function in the `Ipv6EndPoint` was changed. It now includes the destination IPv6 address of the end point which was needed for TCP. This lead to a small change in the UDP and ICMPv6 L4 protocols as well.
* `Ipv6RoutingHelper` can now print the IPv6 Routing Tables at specific intervals or time. Exactly like `Ipv4RoutingHelper` do.
* New `SendIcmpv6Redirect` attribute (and getter/setter functions) to `Ipv6L3Protocol`. The behavior is similar to Linux's conf `send_redirects`, i.e., enable/disable the ICMPv6 Redirect sending.
* The `SpectrumPhy` abstract class now has a new method

  ```cpp
  virtual Ptr<AntennaModel> GetRxAntenna () = 0;
  ```

  that all derived classes need to implement in order to integrate properly with the newly added antenna model. In addition, a new member variable `Ptr<AntennaModel> txAntenna` has been added to SpectrumSignalParameters in order to allow derived SpectrumPhy classes to provide information about the antenna model used for the transmission of a waveform.

* The Ns2CalendarScheduler event scheduler has been removed.
* `ErrorUnit` enum has been moved into `RateErrorModel` class, and symbols `EU_BIT`, `EU_BYTE` and `EU_PKT` have been renamed to `RateErrorModel::ERROR_UNIT_BIT`, `RateErrorModel::ERROR_UNIT_BYTE` and `RateErrorModel::ERROR_UNIT_PACKET`. `RateErrorModel` class attribute `ErrorUnit` values have also been renamed for consistency, and are now `ERROR_UNIT_BIT`, `ERROR_UNIT_BYTE`, `ERROR_UNIT_PACKET`.
* `QueueMode` enum from `DropTailQueue` and `RedQueue` classes has been unified and moved to `Queue` class. Symbols `DropTailQueue::PACKETS` and `DropTailQueue::BYTES` are now named `Queue::QUEUE_MODE_PACKETS` and `DropTailQueue::QUEUE_MODE_BYTES`. In addition, `DropTailQueue` and `RedQueue` class attributes `Mode` have been renamed for consistency from `Packets` and `Bytes` to `QUEUE_MODE_PACKETS` and `QUEUE_MODE_BYTES`.
* The API of the LTE module has undergone a significant redesign with the merge of the code from the LENA project. The new API is not backwards compatible with the previous version of the LTE module.
* The `Ipv6AddressHelper` API has been aligned with the `Ipv4AddressHelper` API. The helper can be set with a call to `Ipv6AddressHelper::SetBase (Ipv6Address network, Ipv6Prefix prefix)` instead of `NewNetwork (Ipv6Address network, Ipv6Prefix prefix)`. A new `NewAddress (void)` method has been added. Typical usage will involve calls to `SetBase ()`, `NewNetwork ()`, and `NewAddress ()`, as in class `Ipv4AddressHelper`.

### Changes to build system

* The following files are removed:
  * `src/internet/model/ipv4-l4-protocol.cc`
  * `src/internet/model/ipv4-l4-protocol.h`
  * `src/internet/model/ipv6-l4-protocol.cc`
  * `src/internet/model/ipv6-l4-protocol.h`

  and replaced with:

  * `src/internet/model/ip-l4-protocol.cc`
  * `src/internet/model/ip-l4-protocol.h`

### Changed behavior

* Dual-stacked IPv6 sockets are implemented. An IPv6 socket can accept an IPv4 connection, returning the senders address as an IPv4-mapped address (`IPV6_V6ONLY` socket option is not implemented).
* The following `examples/application/helpers` were modified to support IPv6:

  ```text
  csma-layout/examples/csma-star [*]
  netanim/examples/star-animation [*]
  point-to-point-layout/model/point-to-point-star.cc
  point-to-point-layout/model/point-to-point-grid.cc
  point-to-point-layout/model/point-to-point-dumbbell.cc
  examples/udp/udp-echo [*]
  examples/udp-client-server/udp-client-server [*]
  examples/udp-client-server/udp-trace-client-server [*]
  applications/helper/udp-echo-helper
  applications/model/udp-client
  applications/model/udp-echo-client
  applications/model/udp-echo-server
  applications/model/udp-server
  applications/model/udp-trace-client

  [*] Added --useIpv6 flag to switch between IPv4 and IPv6
  ```

## Changes from ns-3.12 to ns-3.13

### Changes to build system

* The underlying version of waf used by ns-3 was upgraded to 1.6.7. This has a few changes for users and developers:
  * by default, `build` no longer has a subdirectory debug or optimized. To get different build directories for different build types, you can use the waf configure `-o` option, e.g.:

  ```shell
  ./waf configure -o shared
  ./waf configure --enable-static -o static
  ```

  * (for developers) the `ns3headers` taskgen needs to be created with a features parameter name:

  ```python
  - headers = bld.new_task_gen('ns3header')
  + headers = bld.new_task_gen(features=['ns3header'])
  ```

  * no longer need to edit `src/wscript` to add a module, just create your module directory inside src and ns-3 will pick it up
  * In WAF 1.6, adding `-Dxxx` options is done via the DEFINES env. var. instead of CXXDEFINES
  * waf env values are always lists now, e.g. `env['PYTHON']` returns `['/usr/bin/python']`, so you may need to add `[0]` to the value in some places

### New API

* In the mobility module, there is a new `MobilityModel::GetRelativeSpeed()` method returning the relative speed of two objects.
* A new `Ipv6AddressGenerator` class was added to generate sequential addresses from a provided base prefix and interfaceId. It also will detect duplicate address assignments.

### Changes to existing API

* In the spectrum module, the parameters to `SpectrumChannel::StartTx ()` and `SpectrumPhy::StartRx ()` methods are now passed using the new struct `SpectrumSignalParameters`. This new struct supports inheritance, hence it allows technology-specific PHY implementations to provide technology-specific parameters in `SpectrumChannel::StartTx()` and `SpectrumPhy::StartRx()`, while at the same time keeping a set of technology-independent parameters common across all spectrum-enabled PHY implementations (i.e., the duration and the power spectral density which are needed for interference calculation). Additionally, the `SpectrumType` class has been removed, since now the type of a spectrum signal can be inferred by doing a dynamic cast on SpectrumSignalParameters. See the [Spectrum API change discussion on ns-developers](http://mailman.isi.edu/pipermail/ns-developers/2011-October/009495.html) for the motivation behind this API change.
* The `WifiPhyStandard` enumerators for specifying half- and quarter-channel width standards has had a change in capitalization:
  * `WIFI_PHY_STANDARD_80211_10Mhz` was changed to `WIFI_PHY_STANDARD_80211_10MHZ`
  * `WIFI_PHY_STANDARD_80211_5Mhz` was changed to `WIFI_PHY_STANDARD_80211_5MHZ`
* In the SpectrumPhy base class, the methods to get/set the `MobilityModel` and the `NetDevice` were previously working with opaque `Ptr<Object>`. Now all these methods have been changed so that they work with `Ptr<NetDevice>` and `Ptr<MobilityModel>` as appropriate. See [Bug 1271](https://www.nsnam.org/bugzilla/show_bug.cgi?id=1271) on bugzilla for the motivation.

### Changed behavior

* TCP bug fixes
  * Connection retries count is a separate variable with the retries limit, so cloned sockets can reset the count
  * Fix bug on RTO that may halt the data flow
  * Make TCP endpoints always holds the accurate address:port info
  * RST packet is sent on closed sockets
  * Fix congestion window sizing problem upon partial ACK in TcpNewReno
  * Acknowledgement is sent, rather than staying silent, upon arrival of unacceptable packets
  * Advance `TcpSocketBase::m_nextTxSequence` after RTO
* TCP enhancements
  * Latest RTT value now stored in variable `TcpSocketBase::m_lastRtt`
  * The list variable `TcpL4Protocol::m_sockets` now always holds all the created, running `TcpSocketBase` objects
  * Maximum announced window size now an attribute, `ns3::TcpSocketBase::MaxWindowSize`
  * `TcpHeader` now recognizes ECE and CWR flags (c.f. RFC3168)
  * Added TCP option handling call in `TcpSocketBase` for future extension
  * Data out of range (i.e. outsize acceptable range of receive window) now computed on bytes, not packets
  * TCP moves from time-wait state to closed state after twice the time specified by attribute `ns3:TcpSocketBase::MaxSegLifeTime`
  * TcpNewReno supports limited transmit (RFC3042) if asserting boolean attribute `ns3::TcpNewReno::LimitedTransmit`
  * Nagle's algorithm supported. Default off, turn on by calling `TcpSocket::SetTcpNoDelay(true)`

## Changes from ns-3.11 to ns-3.12

### Changes to build system

* None for this release

### New API

* New method, `RegularWifiMac::SetPromisc (void)`, to set the interface to promiscuous mode.

### Changes to existing API

* The spelling of the attribute `IntialCellVoltage` from `LiIonEnergySource` was corrected to `InitialCellVoltage`; this will affect existing users who were using the attribute with the misspelling.
* Two trace sources in class WifiPhy have had their names changed:
  * `PromiscSnifferRx` is now `MonitorSnifferRx`
  * `PromiscSnifferTx` is now `MonitorSnifferTx`

### Changed behavior

* IPv4 fragmentation is now supported.

## Changes from ns-3.10 to ns-3.11

### Changes to build system

* **Examples and tests are no longer built by default in ns-3**

  You can now make examples and tests be built in ns-3 in two ways.

  * Using `build.py` when ns-3 is built for the first time:

    ```shell
    ./build.py --enable-examples --enable-tests
    ```

  * Using `waf` once ns-3 has been built:

    ```shell
    ./waf configure --enable-examples --enable-tests
    ```

* **Subsets of modules can be enabled using the ns-3 configuration file**

  A new configuration file, `.ns3rc`, has been added to ns-3 that specifies the modules that should be enabled during the ns-3 build. See the documentation for details.

### New API

* **`int64x64_t`**

  The `int64x64_t` type implements all the C++ arithmetic operators to behave like one of the C++ native types. It is a 64.64 integer type which means that it is a 128bit integer type with 64 bits of fractional precision. The existing `Time` type is now automatically convertible to `int64x64_t` to allow arbitrarily complex arithmetic operations on the content of `Time` objects. The implementation of `int64x64_t` is based on the previously-existing `HighPrecision` type and supersedes it.

### Changes to existing API

* **Wifi TX duration calculation moved from InterferenceHelper to WifiPhy**

  The following static methods have been moved from the InterferenceHelper class to the WifiPhy class:

  ```cpp
  static Time CalculateTxDuration (uint32_t size, WifiMode payloadMode, enum WifiPreamble preamble);
  static WifiMode GetPlcpHeaderMode (WifiMode payloadMode, WifiPreamble preamble);
  static uint32_t GetPlcpHeaderDurationMicroSeconds (WifiMode payloadMode, WifiPreamble preamble);
  static uint32_t GetPlcpPreambleDurationMicroSeconds (WifiMode payloadMode, WifiPreamble preamble);
  static uint32_t GetPayloadDurationMicroSeconds (uint32_t size, WifiMode payloadMode);
  ```

* **Test cases no longer return a boolean value**

  Unit test case `DoRun()` functions no longer return a bool value. Now, they don't return a value at all. The motivation for this change was to disallow users from merely returning `true` from a test case to force an error to be recorded. Instead, test case macros should be used.

* **PhyMac renamed to GenericPhy**

  The `PhyMac` interface previously defined in phy-mac.h has been renamed to `GenericPhy` interface and moved to a new file `generic-phy.h`. The related variables and methods have been renamed accordingly.

* **Scalar**

  The Scalar type has been removed. Typical code such as:

  ```cpp
  Time tmp = ...;
  Time result = tmp * Scalar (5);
  ```

  Can now be rewritten as:

  ```cpp
  Time tmp = ...;
  Time result = Time (tmp * 5);
  ```

* **Multicast GetOutputTtl() commands**

  As part of bug 1047 rework to enable multicast routes on nodes with more than 16 interfaces, the methods `Ipv4MulticastRoute::GetOutputTtl ()` and `Ipv6MulticastRoute::GetOutputTtl ()` have been modified to return a `std::map` of interface IDs and TTLs for the route.

### Changed behavior

* If the data inside the TCP buffer is less than the available window, TCP tries to ask for more data to the application, in the hope of filling the usable transmission window. In some cases, this change allows sending bigger packets than the previous versions, optimizing the transmission.
* In TCP, the ACK is now processed before invoking any routine that deals with the segment sending, except in case of retransmissions.

## Changes from ns-3.9 to ns-3.10

### Changes to build system

* **Regression tests are no longer run using waf**

  All regression testing is now being done in `test.py`. As a result, a separate reference trace repository is no longer needed to perform regression tests. Tests that require comparison against known good traces can still be run from test.py. The `--regression` option for `waf` has been removed. However, the `-r` option to `download.py` has been kept to allow users to fetch older revisions of ns-3 that contain these traces.

* **Documentation converted to Sphinx**

  Project documentation (manual, tutorial, and testing) have been converted to Sphinx from the GNU Texinfo markup format.

### New API

* **Pyviz visualizer**

  A Python-based visualizer called pyviz is now integrated with ns-3. For Python simulations, there is an API to start the visualizer. You have to import the visualizer module, and call `visualizer.start()` instead of `ns3.Simulator.Run()`. For C++ simulations, there is no API. For C++ simulations (but also works for Python ones) you need to set the GlobalValue `SimulatorImplementationType` to `ns3::VisualSimulatorImpl`. This can be set from the command-line, for example (add the `--SimulatorImplementationType=ns3::VisualSimulatorImpl` option), or via the waf option `--visualizer`, in addition to the usual `--run` option to run programs.

* **WaypointMobility attributes**

  Two attributes were added to `WaypointMobility` model: `LazyNotify` and `InitialPositionIs` Waypoint. See [RELEASE_NOTES.md](RELEASE_NOTES.md) for details.

* **802.11g rates for ERP-OFDM added**

  New WifiModes of the form ErpOfdmRatexxMbps, where xx is the rate in Mbps (6, 9, 12, 18, 24, 36, 48, 54), are available for 802.11g. More details are in the [RELEASE_NOTES.md](RELEASE_NOTES.md).

* **Socket::GetSocketType ()**

  This is analogous to `getsockopt(SO_TYPE)`. `ipv4-raw-socket`, `ipv6-raw-socket`, and `packet-socket` return `NS3_SOCK_RAW`. tcp-socket and nsc-tcp-socket return `NS3_SOCK_STREAM`. `udp-socket` returns `NS3_SOCK_DGRAM`.

* **BulkSendApplication**

  Sends data as fast as possible up to MaxBytes or unlimited if MaxBytes is zero. Think OnOff, but without the `off` and without the variable data rate. This application only works with `NS3_SOCK_STREAM` and `NS3_SOCK_SEQPACKET` sockets, for example TCP sockets and not UDP sockets. A helper class exists to facilitate creating `BulkSendApplications`. The API for the helper class is similar to existing application helper classes, for example, OnOff.

* **Rakhmatov Vrudhula non-linear battery model**

  New class and helper for this battery model.

* **Print IPv4 routing tables**

  New class methods and helpers for printing IPv4 routing tables to an output stream.

* **Destination-Sequenced Distance Vector (DSDV) routing protocol**

  Derives from `Ipv4RoutingProtocol` and contains a `DsdvHelper` class.

* **3GPP Long Term Evolution (LTE) models**

  More details are in the [RELEASE_NOTES.md](RELEASE_NOTES.md).

### Changes to existing API

* **Consolidation of Wi-Fi MAC high functionality**

  Wi-Fi MAC high classes have been reorganised in attempt to consolidate shared functionality into a single class. This new class is `RegularWifiMac`, and it derives from the abstract `WifiMac`, and is parent of `AdhocWifiMac`, `StaWifiMac`, `ApWifiMac`, and `MeshWifiInterfaceMac`. The QoS and non-QoS class variants are no longer, with a `RegularWifiMac` attribute `QosSupported` allowing selection between these two modes of operation. `QosWifiMacHelper` and `NqosWifiMacHelper` continue to work as previously, with a behind-the-scenes manipulation of the afore-mentioned attribute.

* **New TCP architecture**

  `TcpSocketImpl` was replaced by a new base class `TcpSocketBase` and several subclasses implementing different congestion control. From a user-level API perspective, the main change is that a new attribute `SocketType` is available in `TcpL4Protocol`, to which a `TypeIdValue` of a specific Tcp variant can be passed. In the same class, the attribute `RttEstimatorFactory` was also renamed `RttEstimatorType` since it now takes a `TypeIdValue` instead of an `ObjectFactoryValue`. In most cases, however, no change to existing user programs should be needed.

### Changed behavior

* **EmuNetDevice uses DIX instead of LLC encapsulation by default**

  bug 984 in ns-3 tracker: real devices don't usually understand LLC/SNAP so the default of DIX makes more sense.

* **TCP defaults to NewReno congestion control**

  As part of the TCP socket refactoring, a new TCP implementation provides slightly different behavior than the previous TcpSocketImpl that provided only fast retransmit. The default behavior now is NewReno which provides fast retransmit and fast recovery with window inflation during recovery.

## Changes from ns-3.8 to ns-3.9

### Changes to build system

* None for this release.

### New API

* **Wifi set block ack threshold:** Two methods for setting block ack parameters for a specific access class:

  ```cpp
  void QosWifiMacHelper::SetBlockAckThresholdForAc (enum AccessClass accessClass, uint8_t threshold);
  void QosWifiMacHelper::SetBlockAckInactivityTimeoutForAc (enum AccessClass accessClass, uint16_t timeout);
  ```

* **Receive List Error Model:** Another basic error model that allows the user to specify a list of received packets that should be errored. The list corresponds not to the packet UID but to the sequence of received packets as observed by the error model. See src/common/error-model.h
* **Respond to interface events:** New attribute for Ipv4GlobalRouting, `RespondToInterfaceEvents`, which when enabled, will cause global routes to be recomputed upon any interface or address notification event from IPv4.
* **Generic sequence number:** New generic sequence number class to easily handle comparison, subtraction, etc. for sequence numbers. To use it you need to supply two fundamental types as template parameters: `NUMERIC_TYPE` and `SIGNED_TYPE`. For instance, `SequenceNumber<uint32_t, int32_t>` gives you a 32-bit sequence number, while `SequenceNumber<uint16_t, int16_t>` is a 16-bit one. For your convenience, these are typedef'ed as SequenceNumber32 and SequenceNumber16, respectively.
* **Broadcast socket option:** New Socket methods `SetAllowBroadcast` and `GetAllowBroadcast` add to NS-3 Socket's the equivalent to the POSIX `SO_BROADCAST` socket option (setsockopt/getsockopt). Starting from this NS-3 version, IPv4 sockets do not allow us to send packets to broadcast destinations by default; `SetAllowBroadcast` must be called beforehand if we wish to send broadcast packets.
* **Deliver of packet ancillary information to sockets:** A method to deliver ancillary information to the socket interface (fixed in bug 671):

  ```cpp
  void Socket::SetRecvPktInfo (bool flag);
  ```

### Changes to existing API

* **Changes to construction and naming of Wi-Fi transmit rates:** A reorganisation of the construction of Wi-Fi transmit rates has been undertaken with the aim of simplifying the task of supporting further IEEE 802.11 PHYs. This work has been completed under the auspices of Bug 871. From the viewpoint of simulation scripts not part of the ns-3 distribution, the key change is that WifiMode names of the form `wifi_x_n_mbs` are now invalid. Names now take the form `_Cccc_Rate_n_Mbps[BW_b_MHz]`, where `_n_` is the root bitrate in megabits-per-second as before (with only significant figures included, and an underscore replacing any decimal point), and `_Cccc_` is a representation of the Modulation Class as defined in Table 9-2 of IEEE Std. 802.11-2007. Currently-supported options for `_Cccc_` are `_Ofdm_and_Dsss_`. For modulation classes where optional reduced-bandwidth transmission is possible, this is captured in the final part of the form above, with `_b_` specifying the nominal signal bandwidth in megahertz.
* **Consolidation of classes support Wi-Fi Information Elements:** When the `mesh` module was introduced it added a class hierarchy for modelling of the various Information Elements that were required. In this release, this class hierarchy has extended by moving the base classes (WifiInformationElement and WifiInformationElementVector) into the `wifi` module. This change is intended to ease the addition of support for modelling of further Wi-Fi functionality.
* **Changed for `{Ipv4,Ipv6}PacketInfoTag` delivery:** In order to deliver ancillary information to the socket interface (fixed in bug 671), `Ipv4PacketInfoTag` and `Ipv6PacketInfoTag` are implemented. For the delivery of this information, the following changes are made into existing class. In `Ipv4EndPoint` class,

  ```cpp
  - void SetRxCallback (Callback<void, Ptr<Packet>, Ipv4Address, Ipv4Address, uint16_t> callback);
  + void SetRxCallback (Callback<void, Ptr<Packet>, Ipv4Header, uint16_t, Ptr<Ipv4Interface>> callback);

  - void ForwardUp (Ptr<Packet> p, Ipv4Address saddr, Ipv4Address daddr, uint16_t sport);
  + void ForwardUp (Ptr<Packet> p, const Ipv4Header& header, uint16_t sport, Ptr<Ipv4Interface> incomingInterface);
  ```

  In `Ipv4L4Protocol` class,

  ```cpp
  virtual enum RxStatus Receive (Ptr<Packet> p,
  -                              Ipv4Address const &source,
  -                              Ipv4Address const &destination,
  +                              Ipv4Header const &header,
                                 Ptr<Ipv4Interface> incomingInterface) = 0;

  - Ipv4RawSocketImpl::ForwardUp (Ptr<const Packet> p, Ipv4Header ipHeader, Ptr<NetDevice> device);
  + Ipv4RawSocketImpl::ForwardUp (Ptr<const Packet> p, Ipv4Header ipHeader, Ptr<Ipv4Interface> incomingInterface);

  - NscTcpSocketImpl::ForwardUp (Ptr<Packet> packet, Ipv4Address saddr, Ipv4Address daddr, uint16_t port);
  + NscTcpSocketImpl::ForwardUp (Ptr<Packet> packet, Ipv4Header header, uint16_t port,
  +                              Ptr<Ipv4Interface> incomingInterface);

  - TcpSocketImpl::ForwardUp (Ptr<Packet> packet, Ipv4Address saddr, Ipv4Address daddr, uint16_t port);
  + TcpSocketImpl::ForwardUp (Ptr<Packet> packet, Ipv4Header header, uint16_t port,
  +                           Ptr<Ipv4Interface> incomingInterface);

  - UdpSocketImpl::ForwardUp (Ptr<Packet> packet, Ipv4Address saddr, Ipv4Address daddr, uint16_t port);
  + UdpSocketImpl::ForwardUp (Ptr<Packet> packet, Ipv4Header header, uint16_t port,
  +                           Ptr<Ipv4Interface> incomingInterface);
  ```

* The method `OutputStreamWrapper::SetStream (std::ostream *ostream)` was removed.

### Changed behavior

* **Queue trace behavior during Enqueue changed:** The behavior of the Enqueue trace source has been changed to be more intuitive and to agree with documentation. Enqueue and Drop events in src/node/queue.cc are now mutually exclusive. In the past, the meaning of an Enqueue event was that the Queue Enqueue operation was being attempted; and this could be followed by a Drop event if the Queue was full. The new behavior is such that a packet is either Enqueue'd successfully or Drop'ped.
* **Drop trace logged for Ipv4/6 forwarding failure:** Fixed bug 861; this will add ascii traces (drops) in Ipv4 and Ipv6 traces for forwarding failures
* **Changed default WiFi error rate model for OFDM modulation types:** Adopted more conservative ErrorRateModel for OFDM modulation types (a/g). This will require 4 to 5 more dB of received power to get similar results as before, so users may observe a reduced WiFi range when using the defaults. See tracker issue 944 for more details.

## Changes from ns-3.7 to ns-3.8

### Changes to build system

* None for this release.

### New API

* **Matrix propagation loss model:** This radio propagation model uses a two-dimensional matrix of path loss indexed by source and destination nodes.
* **WiMAX net device**: The developed WiMAX model attempts to provide an accurate MAC and PHY level implementation of the 802.16 specification with the Point-to-Multipoint (PMP) mode and the WirelessMAN-OFDM PHY layer. By adding WimaxNetDevice objects to ns-3 nodes, one can create models of 802.16-based networks. The source code for the WiMAX models lives in the directory src/devices/wimax. The model is mainly composed of three layers:
  * The convergence sublayer (CS)
  * The MAC Common Part Sublayer (MAC-CPS)
  * The Physical layerThe main way that users who write simulation scripts will typically interact with the Wimax models is through the helper API and through the publicly visible attributes of the model. The helper API is defined in src/helper/wimax-helper.{cc,h}. Three examples containing some code that shows how to setup a 802.16 network are located under examples/wimax/
* **MPI Interface for distributed simulation:** Enables access to necessary MPI information such as MPI rank and size.
* **Point-to-point remote channel:** Enables point-to-point connection between net-devices on different simulators, for use with distributed simulation.
* **GetSystemId in simulator:** For use with distributed simulation, GetSystemId returns zero by non-distributed simulators. For the distributed simulator, it returns the MPI rank.
* **Enhancements to src/core/random-variable.cc/h:** New Zeta random variable generator. The Zeta random distribution is tightly related to the Zipf distribution (already in ns-3.7). See the documentation, especially because sometimes the Zeta distribution is called Zipf and vice-versa. Here we conform to the Wikipedia naming convention, i.e., Zipf is bounded while Zeta isn't.
* **Two-ray ground propagation loss model:** Calculates the crossover distance under which Friis is used. The antenna height is set to the nodes z coordinate, but can be added to using the model parameter SetHeightAboveZ, which will affect ALL stations
* **Pareto random variable** has two new constructors to specify scale and shape:

  ```cpp
  ParetoVariable (std::pair params);
  ParetoVariable (std::pair params, double b);
  ```

### Changes to existing API

* **Tracing Helpers**: The organization of helpers for both pcap and ascii tracing, in devices and protocols, has been reworked. Instead of each device and protocol helper re-implementing trace enable methods, classes have been developed to implement user-level tracing in a consistent way; and device and protocol helpers use those classes to provide tracing functionality.

  In addition to consistent operation across all helpers, the object name service has been integrated into the trace file naming scheme. The internet stack helper has been extensively massaged to make it easier to manage traces originating from protocols. It used to be the case that there was essentially no opportunity to filter tracing on interfaces, and resulting trace file names collided with those created by devices. File names are now disambiguated and one can enable traces on a protocol/interface basis analogously to the node/device granularity of device-based helpers.

  The primary user-visible results of this change are that trace-related functions have been changed from static functions to method calls; and a new object has been developed to hold streams for ascii traces.

  New functionality is present for ascii traces. It is now possible to create multiple ascii trace files automatically just as was possible for pcap trace files.

  The implementation of the helper code has been designed also to provide functionality to make it easier for sophisticated users to hook traces of various kinds and write results to (file) streams.

  Before:

  ```cpp
  CsmaHelper::EnablePcapAll ();

  std::ofstream ascii;
  ascii.open ("csma-one-subnet.tr", std::ios_base::binary | std::ios_base::out);
  CsmaHelper::EnableAsciiAll (ascii);

  InternetStackHelper::EnableAsciiAll (ascii);
  ```

  After:

  ```cpp
  CsmaHelper csmaHelper;
  InternetStackHelper stack;
  csmaHelper.EnablePcapAll ();

  AsciiTraceHelper ascii;
  csma.EnableAsciiAll (ascii.CreateFileStream ("csma-one-subnet.tr"));

  stack.EnableAsciiIpv4All (stream);
  ```

* **Serialization and Deserialization** in buffer, nix-vector, packet-metadata, and packet has been modified to use raw character buffers, rather than the Buffer class

  ```cpp
  + uint32_t Buffer::GetSerializedSize (void) const;
  + uint32_t Buffer::Serialize (uint8_t* buffer, uint32_t maxSize) const;
  + uint32_t Buffer::Deserialize (uint8_t* buffer, uint32_t size);

  - void NixVector::Serialize (Buffer::Iterator i, uint32_t size) const;
  + uint32_t NixVector::Serialize (uint32_t* buffer, uint32_t maxSize) const;
  - uint32_t NixVector::Deserialize (Buffer::Iterator i);
  + uint32_t NixVector::Deserialize (uint32_t* buffer, uint32_t size);

  - void PacketMetadata::Serialize (Buffer::Iterator i, uint32_t size) const;
  + uint32_t PacketMetadata::Serialize (uint8_t* buffer, uint32_t maxSize) const;
  - uint32_t PacketMetadata::Deserialize (Buffer::Iterator i);
  + uint32_t PacketMetadata::Deserialize (uint8_t* buffer, uint32_t size);

  + uint32_t Packet::GetSerializedSize (void) const;
  - Buffer Packet::Serialize (void) const;
  + uint32_t Packet::Serialize (uint8_t* buffer, uint32_t maxSize) const;
  - void Packet::Deserialize (Buffer buffer);
  + Packet::Packet (uint8_t const* buffer, uint32_t size, bool magic);
  ```

* **PacketMetadata uid** has been changed to a 64-bit value. The lower 32 bits give the uid, while the upper 32-bits give the MPI rank for distributed simulations. For non-distributed simulations, the upper 32 bits are simply zero.

  ```cpp
  - inline PacketMetadata (uint32_t uid, uint32_t size);
  + inline PacketMetadata (uint64_t uid, uint32_t size);
  - uint32_t GetUid (void) const;
  + uint64_t GetUid (void) const;
  - PacketMetadata::PacketMetadata (uint32_t uid, uint32_t size);
  + PacketMetadata::PacketMetadata (uint64_t uid, uint32_t size);
  - uint32_t Packet::GetUid (void) const;
  + uint64_t Packet::GetUid (void) const;
  ```

* **Moved propagation models** from `src/devices/wifi` to `src/common`
* **Moved Mtu attribute from base class NetDevice** This attribute is now found in all NetDevice subclasses.

### Changed behavior

* None for this release.

## Changes from ns-3.6 to ns-3.7

### Changes to build system

* None for this release.

### New API

* **Equal-cost multipath for global routing:** Enables quagga's equal cost multipath for Ipv4GlobalRouting, and adds an attribute that can enable it with random packet distribution policy across equal cost routes.
* **Binding sockets to devices:** A method analogous to a `SO_BINDTODEVICE` socket option has been introduced to class Socket:

  ```cpp
  virtual void Socket::BindToNetDevice (Ptr<NetDevice> netdevice);
  ```

* **Simulator event contexts**: The Simulator API now keeps track of a per-event `context` (a 32bit integer which, by convention identifies a node by its id). `Simulator::GetContext` returns the context of the currently-executing event while `Simulator::ScheduleWithContext` creates an event with a context different from the execution context of the caller. This API is used by the ns-3 logging system to report the execution context of each log line.
* `Object::DoStart`: Users who need to complete their object setup at the start of a simulation can override this virtual method, perform their adhoc setup, and then, must chain up to their parent.
* **Ad hoc On-Demand Distance Vector (AODV)** routing model, [RFC 3561](http://www.ietf.org/rfc/rfc3561.txt)
* `Ipv4::IsDestinationAddress (Ipv4Address address, uint32_t iif)` Method added to support checks of whether a destination address should be accepted as one of the host's own addresses. RFC 1122 Strong/Weak end system behavior can be changed with a new attribute (WeakEsModel) in class Ipv4.
* **Net-anim interface**: Provides an interface to net-anim, a network animator for point-to-point links in ns-3. The interface generates a custom trace file for use with the NetAnim program.
* **Topology Helpers**: New topology helpers have been introduced including PointToPointStarHelper, PointToPointDumbbellHelper, PointToPointGridHelper, and CsmaStarHelper.
* **IPv6 extensions support**: Provides API to add IPv6 extensions and options. Two examples (fragmentation and loose routing) are available.

### Changes to existing API

* `Ipv4RoutingProtocol::RouteOutput` no longer takes an outgoing interface index but instead takes an outgoing device pointer; this affects all subclasses of Ipv4RoutingProtocol.

  ```cpp
  - virtual Ptr<Ipv4Route> RouteOutput (Ptr<Packet> p, const Ipv4Header &header, uint32_t oif, Socket::SocketErrno &sockerr) = 0;
  + virtual Ptr<Ipv4Route> RouteOutput (Ptr<Packet> p, const Ipv4Header &header, Ptr<NetDevice> oif, Socket::SocketErrno &sockerr) = 0;
  ```

* `Ipv6RoutingProtocol::RouteOutput` no longer takes an outgoing interface index but instead takes an outgoing device pointer; this affects all subclasses of Ipv6RoutingProtocol.

  ```cpp
  - virtual Ptr<Ipv6Route> RouteOutput (Ptr<Packet> p, const Ipv6Header &header, uint32_t oif, Socket::SocketErrno &sockerr) = 0;
  + virtual Ptr<Ipv6Route> RouteOutput (Ptr<Packet> p, const Ipv6Header &header, Ptr<NetDevice> oif, Socket::SocketErrno &sockerr) = 0;
  ```

* `Application::Start` and `Application::Stop` have been renamed to `Application::SetStartTime` and `Application::SetStopTime`.
* `Channel::Send`: this method does not really exist but each subclass of the Channel base class must implement a similar method which sends a packet from a node to another node. Users must now use Simulator::ScheduleWithContext instead of Simulator::Schedule to schedule the reception event on a remote node.

  For example, before:

  ```cpp
    void
    SimpleChannel::Send (Ptr<Packet> p, uint16_t protocol,
                         Mac48Address to, Mac48Address from,
                         Ptr<SimpleNetDevice> sender)
    {
      for (std::vector<Ptr<SimpleNetDevice>>::const_iterator i = m_devices.begin (); i != m_devices.end (); ++i)
        {
          Ptr<SimpleNetDevice> tmp = *i;
          if (tmp == sender)
            {
              continue;
            }
          Simulator::ScheduleNow (&SimpleNetDevice::Receive, tmp, p->Copy (), protocol, to, from);
        }
    }
    ```

    After:

    ```cpp
    void
    SimpleChannel::Send (Ptr<Packet> p, uint16_t protocol,
                         Mac48Address to, Mac48Address from,
                         Ptr<SimpleNetDevice> sender)
    {
      for (std::vector<Ptr<SimpleNetDevice>>::const_iterator i = m_devices.begin (); i != m_devices.end (); ++i)
        {
          Ptr<SimpleNetDevice> tmp = *i;
          if (tmp == sender)
            {
              continue;
            }
          Simulator::ScheduleWithContext (tmp->GetNode ()->GetId (), Seconds (0),
                                          &SimpleNetDevice::Receive, tmp, p->Copy (), protocol, to, from);
        }
    }
    ```

* `Simulator::SetScheduler`: this method now takes an ObjectFactory instead of an object pointer directly. Existing callers can trivially be updated to use this new method.

  Before:

  ```cpp
  Ptr<Scheduler> sched = CreateObject<ListScheduler> ();
  Simulator::SetScheduler (sched);
  ```

  After:

  ```cpp
  ObjectFactory sched;
  sched.SetTypeId ("ns3::ListScheduler");
  Simulator::SetScheduler (sched);
  ```

* Extensions to IPv4 `Ping` application: verbose output and the ability to configure different ping sizes and time intervals (via new attributes)
* **Topology Helpers**: Previously, topology helpers such as a point-to-point star existed in the PointToPointHelper class in the form of a method (ex: PointToPointHelper::InstallStar). These topology helpers have been pulled out of the specific helper classes and created as separate classes. Several different topology helper classes now exist including PointToPointStarHelper, PointToPointGridHelper, PointToPointDumbbellHelper, and CsmaStarHelper. For example, a user wishes to create a point-to-point star network:

  Before:

  ```cpp
  NodeContainer hubNode;
  NodeContainer spokeNodes;
  hubNode.Create (1);
  Ptr<Node> hub = hubNode.Get (0);
  spokeNodes.Create (nNodes - 1);

  PointToPointHelper pointToPoint;
  pointToPoint.SetDeviceAttribute ("DataRate", StringValue ("5Mbps"));
  pointToPoint.SetChannelAttribute ("Delay", StringValue ("2ms"));
  NetDeviceContainer hubDevices, spokeDevices;
  pointToPoint.InstallStar (hubNode.Get (0), spokeNodes, hubDevices, spokeDevices);
  ```

  After:

  ```cpp
  PointToPointHelper pointToPoint;
  pointToPoint.SetDeviceAttribute ("DataRate", StringValue ("5Mbps"));
  pointToPoint.SetChannelAttribute ("Delay", StringValue ("2ms"));
  PointToPointStarHelper star (nSpokes, pointToPoint);
  ```

### Changed behavior

* Changed default value of YansWifiPhy::EnergyDetectionThreshold from -140.0 dBm to -96.0 dBm. Changed default value of YansWifiPhy::CcaModelThreshold from -140.0 dBm to -99.0 dBm. Rationale can be found [here](http://www.nsnam.org/bugzilla/show_bug.cgi?id=689).
* Default TTL of IPv4 broadcast datagrams changed from 1 to 64.
* Changed DcfManager::UpdateBackoff (): using flooring instead of rounding in calculation of remaining slots. [See bug 695.](http://www.nsnam.org/bugzilla/show_bug.cgi?id=695)

## Changes from ns-3.5 to ns-3.6

### Changes to build system

* **A new test framework is provided with ns-3.6 that primarilay runs outside waf**
  `./waf check` now runs the new unit tests of the core part of ns-3.6. In order to run the complete test package, use `./test.py` which is documented in a new manual -- find it in `./doc/testing`. `./waf check` no longer generates the introspected Doxygen. Now use `./waf doxygen` to do this and generate the Doxygen documentation in one step.

### New API

* **Longest prefix match, support for metrics, for `Ipv4StaticRouting` and `Ipv6StaticRouting`**
  * When performing route lookup, first match for longest prefix, and then based on metrics (default metric = 0). If metrics are equal, most recent addition is picked. Extends API for support of metrics but preserves backward compatibility. One small change is that the default route is no longer stored as index 0 route in the host route table so GetDefaultRoute () must be used.

* **Route injection for global routing**
  * Add ability to inject and withdraw routes to `Ipv4GlobalRouting`. This allows a user to insert a route and have it redistributed like an OSPF external LSA to the rest of the topology.

* **Athstats**
  * New classes `AthstatsWifiTraceSink` and `AthstatsHelper`.

* **WifiRemoteStationManager**
  * New trace sources exported by `WifiRemoteStationManager`: `MacTxRtsFailed`, `MacTxDataFailed`, `MacTxFinalRtsFailed` and `MacTxFinalDataFailed`.

* **IPv6 additions**

  Add an IPv6 protocol and ICMPv6 capability.

  * new classes `Ipv6`, `Ipv6Interface`, `Ipv6L3Protocol`, `Ipv6L4Protocol`
  * `Ipv6RawSocket` (no UDP or TCP capability yet)
  * a set of classes to implement Icmpv6, including neighbor discovery, router solicitation, DAD
  * new applications `Ping6` and `Radvd`
  * routing objects `Ipv6Route` and `Ipv6MulticastRoute`
  * routing protocols `Ipv6ListRouting` and `Ipv6StaticRouting`
  * examples: `icmpv6-redirect.cc`, `ping6.cc`, `radvd.cc`, `radvd-two-prefix.cc`, `simple-routing-ping6.cc`

* **Wireless Mesh Networking models**

  * General multi-interface mesh stack infrastructure (devices/mesh module).
  * IEEE 802.11s (Draft 3.0) model including Peering Management Protocol and HWMP.
  * Forwarding Layer for Meshing (FLAME) protocol.

* **802.11 enhancements**
  * 10MHz and 5MHz channel width supported by 802.11a model (Ramon Bauza and Kirill Andreev).
  * Channel switching support. `YansWifiPhy` can now switch among different channels (Ramon Bauza and Pavel Boyko).

* **Nix-vector Routing**
  * Add nix-vector routing protocol
  * new helper class `Ipv4NixVectorHelper`
  * examples: `nix-simple.cc`, `nms-p2p-nix.cc`

* **New Test Framework**
  * Add `TestCase`, `TestSuite` classes
  * examples: `src/core/names-test-suite.cc`, `src/core/random-number-test-suite.cc`, `src/test/ns3tcp/ns3tcp-cwnd-test-suite.cc`

### Changes to existing API

* **InterferenceHelper**
  * The method `InterferenceHelper::CalculateTxDuration (uint32_t size, WifiMode payloadMode, WifiPreamble preamble)` has been made static, so that the frame duration depends only on the characteristics of the frame (i.e., the function parameters) and not on the particular standard which is used by the receiving PHY. This makes it now possible to correctly calculate the duration of incoming frames in scenarios in which devices using different PHY configurations coexist in the same channel (e.g., a BSS using short preamble and another BSS using long preamble).

  * The following member methods have been added to `InterferenceHelper`:

  ```cpp
  static WifiMode GetPlcpHeaderMode (WifiMode, WifiPreamble);
  static uint32_t GetPlcpHeaderDurationMicroSeconds (WifiMode, WifiPreamble);
  static uint32_t GetPlcpPreambleDurationMicroSeconds (WifiMode, WifiPreamble);
  static uint32_t GetPayloadDurationMicroSeconds (size, WifiMode);
  ```

  The following member methods have been removed from `InterferenceHelper`:

  ```cpp
  void Configure80211aParameters (void);
  void Configure80211bParameters (void);
  void Configure80211_10MhzParameters (void);
  void Configure80211_5MhzParameters (void);
  ```

* **WifiMode**

  `WifiMode` now has a `WifiPhyStandard` attribute which identifies the standard the WifiMode belongs to. To properly set this attribute when creating a new WifiMode, it is now required to explicitly pass a WifiPhyStandard parameter to all `WifiModeFactory::CreateXXXX ()` methods. The `WifiPhyStandard` value of an existing `WifiMode` can be retrieved using the new method `WifiMode::GetStandard ()`.

* **NetDevice**

  In order to have multiple link change callback in NetDevice (i.e. to flush ARP and IPv6 neighbor discovery caches), the following member method has been renamed:

  ```cpp
  - virtual void SetLinkChangeCallback (Callback<void> callback);
  + virtual void AddLinkChangeCallback (Callback<void> callback);
  ```

  Now each NetDevice subclasses have a `TracedCallback<>` object (list of callbacks) instead of `Callback<void>` ones.

## Changes from ns-3.4 to ns-3.5

### Changes to build system

* None for this release.

### New API

* **YansWifiPhyHelper supporting radiotap and prism PCAP output**

  The newly supported pcap formats can be adopted by calling the following new method of `YansWifiPhyHelper`:

  ```cpp
  void SetPcapFormat (enum PcapFormat format);
  ```

  where format is one of `PCAP_FORMAT_80211_RADIOTAP`, `PCAP_FORMAT_80211_PRISM` or `PCAP_FORMAT_80211`. By default, `PCAP_FORMAT_80211` is used, so the default PCAP format is the same as before.

* **attributes for class Ipv4**

  class Ipv4 now contains attributes in `ipv4.cc`; the first one is called `IpForward` that will enable/disable Ipv4 forwarding.

* **packet tags**

  class `Packet` now contains `AddPacketTag`, `RemovePacketTag` and `PeekPacketTag` which can be used to attach a tag to a packet, as opposed to the old AddTag method which attached a tag to a set of bytes. The main semantic difference is in how these tags behave in the presence of fragmentation and reassembly.

### Changes to existing API

* **Ipv4Interface::GetMtu () deleted**

  The `Ipv4Interface` API is private to internet-stack module; this method was just a pass-through to `GetDevice ()->GetMtu ()`.

* **GlobalRouteManager::PopulateRoutingTables () and RecomputeRoutingTables () are deprecated**

  This API has been moved to the helper API and the above functions will be removed in ns-3.6. The new API is:

  ```cpp
  Ipv4GlobalRoutingHelper::PopulateRoutingTables ();
  Ipv4GlobalRoutingHelper::RecomputeRoutingTables ();
  ```

  Additionally, these low-level functions in `GlobalRouteManager` are now public, allowing more API flexibility at the low level ns-3 API:

  ```cpp
  GlobalRouteManager::DeleteGlobalRoutes ();
  GlobalRouteManager::BuildGlobalRoutingDatabase ();
  GlobalRouteManager::InitializeRoutes ();
  ```

* **CalcChecksum attribute changes**

  Four IPv4 `CalcChecksum` attributes (which enable the computation of checksums that are disabled by default) have been collapsed into one global value in class Node. These four calls:

  ```cpp
  Config::SetDefault ("ns3::Ipv4L3Protocol::CalcChecksum", BooleanValue (true));
  Config::SetDefault ("ns3::Icmpv4L4Protocol::CalcChecksum", BooleanValue (true));
  Config::SetDefault ("ns3::TcpL4Protocol::CalcChecksum", BooleanValue (true));
  Config::SetDefault ("ns3::UdpL4Protocol::CalcChecksum", BooleanValue (true));
  ```

  are replaced by one call to:

  ```cpp
  GlobalValue::Bind ("ChecksumEnabled", BooleanValue (true));
  ```

* **CreateObject changes**

  `CreateObject` is now able to construct objects with a non-default constructor. If you used to pass attribute lists to `CreateObject`, you must now use `CreateObjectWithAttributes`.

* **packet byte tags renaming**
  * `Packet::AddTag` to `Packet::AddByteTag`
  * `Packet::FindFirstMatchingTag` to `Packet::FindFirstMatchingByteTag`
  * `Packet::RemoveAllTags` to `Packet::RemoveAllByteTags`
  * `Packet::PrintTags` to `Packet::PrintByteTags`
  * `Packet::GetTagIterator` to `Packet::GetByteTagIterator`

* **`YansWifiPhyHelper::EnablePcap*` methods not static any more**

  To accommodate the possibility of configuring the PCAP format used for wifi promiscuous mode traces, several methods of `YansWifiPhyHelper` had to be made non-static:

  ```cpp
  -  static void EnablePcap (std::string filename, uint32_t nodeid, uint32_t deviceid);
  +         void EnablePcap (std::string filename, uint32_t nodeid, uint32_t deviceid);
  - static void EnablePcap (std::string filename, Ptr<NetDevice> nd);
  +        void EnablePcap (std::string filename, Ptr<NetDevice> nd);
  - static void EnablePcap (std::string filename, std::string ndName);
  +        void EnablePcap (std::string filename, std::string ndName);
  - static void EnablePcap (std::string filename, NetDeviceContainer d);
  +        void EnablePcap (std::string filename, NetDeviceContainer d);
  - static void EnablePcap (std::string filename, NodeContainer n);
  +        void EnablePcap (std::string filename, NodeContainer n);
  - static void EnablePcapAll (std::string filename);
  +        void EnablePcapAll (std::string filename);
  ```

* **Wifi Promisc Sniff interface modified**

  To accommodate support for the radiotap and prism headers in PCAP traces, the interface for promiscuos mode sniff in the wifi device was changed. The new implementation was heavily inspired by the way the madwifi driver handles monitor mode. A distinction between TX and RX events is introduced, to account for the fact that different information is to be put in the radiotap/prism header (e.g., RSSI and noise make sense only for RX packets). The following are the relevant modifications to the WifiPhy class:

  ```cpp
  - void NotifyPromiscSniff (Ptr<const Packet> packet);
  + void NotifyPromiscSniffRx (Ptr<const Packet> packet, uint16_t channelFreqMhz, uint32_t rate, bool isShortPreamble, double signalDbm, double noiseDbm);
  + void NotifyPromiscSniffTx (Ptr<const Packet> packet, uint16_t channelFreqMhz, uint32_t rate, bool isShortPreamble);
  - TracedCallback<Ptr<const Packet>> m_phyPromiscSnifferTrace;
  + TracedCallback<Ptr<const Packet>, uint16_t, uint32_t, bool, double, double> m_phyPromiscSniffRxTrace;
  + TracedCallback<Ptr<const Packet>, uint16_t, uint32_t, bool> m_phyPromiscSniffTxTrace;
  ```

  The above mentioned callbacks are expected to be used to call the following method to write Wifi PCAP traces in promiscuous mode:

  ```cpp
  + void WriteWifiMonitorPacket(Ptr<const Packet> packet, uint16_t channelFreqMhz, uint32_t rate, bool isShortPreamble, bool isTx, double signalDbm, double noiseDbm);
  ```

  In the above method, the isTx parameter is to be used to differentiate between TX and RX packets. For an example of how to implement these callbacks, see the implementation of PcapSniffTxEvent and PcapSniffRxEvent in `src/helper/yans-wifi-helper.cc`

* **Routing decoupled from class Ipv4**

  All calls of the form `Ipv4::AddHostRouteTo ()` etc. (i.e. to add static routes, both unicast and multicast) have been moved to a new class Ipv4StaticRouting. In addition, class Ipv4 now holds only one possible routing protocol; the previous way to add routing protocols (by ordered list of priority) has been moved to a new class Ipv4ListRouting. Class Ipv4 has a new minimal routing API (just to set and get the routing protocol):

  ```cpp
  - virtual void AddRoutingProtocol (Ptr<Ipv4RoutingProtocol> routingProtocol, int16_t priority) = 0;
  + virtual void SetRoutingProtocol (Ptr<Ipv4RoutingProtocol> routingProtocol) = 0;
  + virtual Ptr<Ipv4RoutingProtocol> GetRoutingProtocol (void) const = 0;
  ```

* **class Ipv4RoutingProtocol is refactored**

  The abstract base class `Ipv4RoutingProtocol` has been refactored to align with corresponding Linux Ipv4 routing architecture, and has been moved from `ipv4.h` to a new file `ipv4-routing-protocol.h`. The new methods (`RouteOutput ()` and `RouteInput ()`) are aligned with Linux `ip_route_output()` and `ip_route_input()`. However, the general nature of these calls (synchronous routing lookup for locally originated packets, and an asynchronous, callback-based lookup for forwarded packets) is still the same.

  ```cpp
  - typedef Callback<void, bool, const Ipv4Route&, Ptr<Packet>, const Ipv4Header&> RouteReplyCallback;
  + typedef Callback<void, Ptr<Ipv4Route>, Ptr<const Packet>, const Ipv4Header &> UnicastForwardCallback;
  + typedef Callback<void, Ptr<Ipv4MulticastRoute>, Ptr<const Packet>, const Ipv4Header &> MulticastForwardCallback;
  + typedef Callback<void, Ptr<const Packet>, const Ipv4Header &, uint32_t > LocalDeliverCallback;
  + typedef Callback<void, Ptr<const Packet>, const Ipv4Header &> ErrorCallback;
  - virtual bool RequestInterface (Ipv4Address destination, uint32_t& interface) = 0;
  + virtual Ptr<Ipv4Route> RouteOutput (Ptr<Packet> p, const Ipv4Header &header, uint32_t oif, Socket::SocketErrno &errno) = 0;
  - virtual bool RequestRoute (uint32_t interface,
  -                            const Ipv4Header &ipHeader,
  -                            Ptr<Packet> packet,
  -                            RouteReplyCallback routeReply) = 0;
  + virtual bool RouteInput (Ptr<const Packet> p, const Ipv4Header &header,
  +                          Ptr<const NetDevice> idev,
  +                          UnicastForwardCallback ucb, MulticastForwardCallback mcb,
  +                          LocalDeliverCallback lcb, ErrorCallback ecb) = 0;
  ```

* **previous class `Ipv4Route`, `Ipv4MulticastRoute` renamed; new classes with those same names added**

  The previous class `Ipv4Route` and `Ipv4MulticastRoute` are used by `Ipv4StaticRouting` and `Ipv4GlobalRouting` to record internal routing table entries, so they were renamed to class `Ipv4RoutingTableEntry` and `Ipv4MulticastRoutingTableEntry`, respectively. In their place, new class `Ipv4Route` and class `Ipv4MulticastRoute` have been added. These are reference-counted objects that are analogous to Linux struct rtable and struct `mfc_cache`, respectively, to achieve better compatibility with Linux routing architecture in the future.

* **class Ipv4 address-to-interface mapping functions changed**

  There was some general cleanup of functions that involve mappings from `Ipv4Address` to either `NetDevice` or Ipv4 interface index.

  ```cpp
  - virtual uint32_t FindInterfaceForAddr (Ipv4Address addr) const = 0;
  - virtual uint32_t FindInterfaceForAddr (Ipv4Address addr, Ipv4Mask mask) const = 0;
  + virtual int32_t GetInterfaceForAddress (Ipv4Address address) const = 0;
  + virtual int32_t GetInterfaceForPrefix (Ipv4Address address, Ipv4Mask mask) const = 0;
  - virtual int32_t FindInterfaceForDevice(Ptr<NetDevice> nd) const = 0;
  + virtual int32_t GetInterfaceForDevice (Ptr<const NetDevice> device) const = 0;
  - virtual Ipv4Address GetSourceAddress (Ipv4Address destination) const = 0;
  - virtual bool GetInterfaceForDestination (Ipv4Address dest,
  - virtual uint32_t GetInterfaceByAddress (Ipv4Address addr, Ipv4Mask mask = Ipv4Mask("255.255.255.255"));
  ```

* **class Ipv4 multicast join API deleted**

  The following methods are not really used in present form since IGMP is not being generated, so they have been removed (planned to be replaced by multicast socket-based calls in the future):

  ```cpp
  - virtual void JoinMulticastGroup (Ipv4Address origin, Ipv4Address group) = 0;
  - virtual void LeaveMulticastGroup (Ipv4Address origin, Ipv4Address group) = 0;
  ```

* **Deconflict `NetDevice::ifIndex` and `Ipv4::ifIndex` (bug 85).**

  All function parameters named `ifIndex` that refer to an Ipv4 interface are instead named `interface`.

  ```cpp
  - static const uint32_t Ipv4RoutingProtocol::IF_INDEX_ANY = 0xffffffff;
  + static const uint32_t Ipv4RoutingProtocol::INTERFACE_ANY = 0xffffffff;

  - bool Ipv4RoutingProtocol::RequestIfIndex (Ipv4Address destination, uint32_t& ifIndex);
  + bool Ipv4RoutingProtocol::RequestInterface (Ipv4Address destination, uint32_t& interface);

  // (N.B. this particular function is planned to be renamed to RouteOutput() in the
  // proposed IPv4 routing refactoring)

  - uint32_t Ipv4::GetIfIndexByAddress (Ipv4Address addr, Ipv4Mask mask);
  + int32_t Ipv4::GetInterfaceForAddress (Ipv4Address address, Ipv4Mask mask) const;

  - bool Ipv4::GetIfIndexForDestination (Ipv4Address dest, uint32_t &ifIndex) const;
  + bool Ipv4::GetInterfaceForDestination (Ipv4Address dest, uint32_t &interface) const;

  // (N.B. this function is not needed in the proposed Ipv4 routing refactoring)
  ```

* **Allow multiple IPv4 addresses to be assigned to an interface (bug 188)**
  * Add class Ipv4InterfaceAddress: This is a new class to resemble Linux's struct `in_ifaddr`. It holds IP addressing information, including mask, broadcast address, scope, whether primary or secondary, etc.

  ```cpp
  + virtual uint32_t AddAddress (uint32_t interface, Ipv4InterfaceAddress address) = 0;
  + virtual Ipv4InterfaceAddress GetAddress (uint32_t interface, uint32_t addressIndex) const = 0;
  + virtual uint32_t GetNAddresses (uint32_t interface) const = 0;
  ```

  * Regarding legacy API usage, typically where you once did the following, using the public Ipv4 class interface (e.g.):

  ```cpp
  ipv4A->SetAddress (ifIndexA, Ipv4Address ("172.16.1.1"));
  ipv4A->SetNetworkMask (ifIndexA, Ipv4Mask ("255.255.255.255"));
  ```

  you now do:

  ```cpp
  Ipv4InterfaceAddress ipv4IfAddrA = Ipv4InterfaceAddress (Ipv4Address ("172.16.1.1"), Ipv4Mask ("255.255.255.255"));
  ipv4A->AddAddress (ifIndexA, ipv4IfAddrA);
  ```

  * At the helper API level, one often gets an address from an interface container. We preserve the legacy `GetAddress (uint32_t i)` but it is documented that this will return only the first (address index 0) address on the interface, if there are multiple such addresses. We provide also an overloaded variant for the multi-address case:

  ```cpp
  Ipv4Address Ipv4InterfaceContainer::GetAddress (uint32_t i);
  + Ipv4Address Ipv4InterfaceContainer::GetAddress (uint32_t i, uint32_t j);
  ```

* **New WifiMacHelper objects**

  The type of wifi MAC is now set by two new specific helpers, `NqosWifiMacHelper` for non QoS MACs and `QosWifiMacHelper` for Qos MACs. They are passed as argument to `WifiHelper::Install` methods.

  ```cpp
  - void WifiHelper::SetMac (std::string type, std::string n0 = "", const AttributeValue &v0 = EmptyAttributeValue (), ...);

  - NetDeviceContainer WifiHelper::Install (const WifiPhyHelper &phyHelper, NodeContainer c) const;
  + NetDeviceContainer WifiHelper::Install (const WifiPhyHelper &phyHelper, const WifiMacHelper &macHelper, NodeContainer c) const;

  - NetDeviceContainer WifiHelper::Install (const WifiPhyHelper &phy, Ptr<Node> node) const;
  + NetDeviceContainer WifiHelper::Install (const WifiPhyHelper &phy, const WifiMacHelper &mac, Ptr<Node> node) const;

  - NetDeviceContainer WifiHelper::Install (const WifiPhyHelper &phy, std::string nodeName) const;
  + NetDeviceContainer WifiHelper::Install (const WifiPhyHelper &phy, const WifiMacHelper &mac, std::string nodeName) const;
  ```

  See `src/helper/nqos-wifi-mac-helper.h` and `src/helper/qos-wifi-mac-helper.h` for more details.

* **Remove `Mac48Address::IsMulticast`**

  This method was considered buggy and unsafe to call. Its replacement is `Mac48Address::IsGroup`.

### Changed behavior

* None for this release.

## Changes from ns-3.3 to ns-3.4

### Changes to build system

* A major option regarding the downloading and building of ns-3 has been added for ns-3.4 -- the ns-3-allinone feature. This allows a user to get the most common options for ns-3 downloaded and built with a minimum amount of trouble. See the ns-3 tutorial for a detailed explanation of how to use this new feature.
* The build system now runs build items in parallel by default. This includes the regression tests.

### New API

* XML support has been added to the ConfigStore in `src/contrib/config-store.cc`
* The ns-2 calendar queue scheduler option has been ported to `src/simulator`
* A `ThreeLogDistancePropagationLossModel` has been added to `src/devices/wifi`
* `ConstantAccelerationMobilityModel` in `src/mobility/constant-acceleration-mobility-model.h`
* A new emulation mode is supported with the `TapBridge` net device (see `src/devices/tap-bridge`)
* A new facility for naming ns-3 Objects is included (see `src/core/names.{cc,h}`)
* Wifi multicast support has been added in `src/devices/wifi`

### Changes to existing API

* Some fairly significant changes have been made to the API of the random variable code. Please see the ns-3 manual and `src/core/random-variable.cc` for details.
* The trace sources in the various NetDevice classes has been completely reworked to allow for a consistent set of trace sources across the devices. The names of the trace sources have been changed to provide some context with respect to the level at which the trace occurred. A new set of trace sources has been added which emulates the behavior of packet sniffers. These sources have been used to implement tcpdump- like functionality and are plumbed up into the helper classes. The user-visible changes are the trace source name changes and the ability to do promiscuous-mode pcap tracing via helpers. For further information regarding these changes, please see the ns-3 manual
* `StaticMobilityModel` has been renamed `ConstantPositionMobilityModel` `StaticSpeedMobilityModel` has been renamed `ConstantVelocityMobilityModel`
* The Callback templates have been extended to support more parameters. See `src/core/callback.h`
* Many helper API have been changed to allow passing Object-based parameters as string names to ease working with the object name service.
* The Config APIs now accept path segments that are names defined by the object name service.
* Minor changes were made to make the system build under the Intel C++ compiler.
* Trace hooks for association and deassociation to/from an access point were added to `src/devices/wifi/nqsta-wifi-mac.cc`

### Changed behavior

* The tracing system rework has introduced some significant changes in the behavior of some trace sources, specifically in the positioning of trace sources in the device code. For example, there were cases where the packet transmit trace source was hit before the packet was enqueued on the device transmit quueue. This now happens just before the packet is transmitted over the channel medium. The scope of the changes is too large to be included here. If you have concerns regarding trace semantics, please consult the net device documentation for details. As is usual, the ultimate source for documentation is the net device source code.

## Changes from ns-3.2 to ns-3.3

### New API

* ns-3 ABORT macros in `src/core/abort.h` `Config::MatchContainer` `ConstCast` and `DynamicCast` helper functions for `Ptr` casting `StarTopology` added to several topology helpers `NetDevice::IsBridge ()`
* 17-11-2008; changeset [4c1c3f6bcd03](http://code.nsnam.org/ns-3-dev/rev/4c1c3f6bcd03)
* The PppHeader previously defined in the point-to-point-net-device code has been made public.
* 17-11-2008; changeset [16c2970a0344](http://code.nsnam.org/ns-3-dev/rev/16c2970a0344)
* An emulated net device has been added as enabling technology for ns-3 emulation scenarios. See `src/devices/emu` and `examples/emu-udp-echo.cc` for details.
* 17-11-2008; changeset [4222173d1e6d](http://code.nsnam.org/ns-3-dev/rev/4222173d1e6d)
* Added method `InternetStackHelper::EnableAsciiChange` to allow allow a user to hook ascii trace to the drop trace events in `Ipv4L3Protocol` and `ArpL3Protocol`.

### Changes to existing API

* `NetDevice::MakeMulticastAddress()` was renamed to `NetDevice::GetMulticast()` and the original `GetMulticast()` removed
* Socket API changes:
  * return type of `SetDataSentCallback ()` changed from `bool` to `void`
  * `Socket::Listen()` no longer takes a queueLimit argument
* As part of the Wifi Phy rework, there have been several API changes at the low level and helper API level.
* At the helper API level, the `WifiHelper` was split to three classes: a `WifiHelper`, a `YansWifiChannel` helper, and a `YansWifiPhy` helper. Some functions like Ascii and Pcap tracing functions were moved from class `WifiHelper` to class `YansWifiPhyHelper`.
* At the low-level API, there have been a number of changes to make the Phy more modular:
* `composite-propagation-loss-model.h` is removed
* `DcfManager::NotifyCcaBusyStartNow()` has changed name
* fragmentation related functions (e.g. `DcaTxop::GetNFragments()`) have changed API to account for some implementation changes
* Interference helper and error rate model added
* `JakesPropagationLossModel::GetLoss()` moved to `PropagationLoss` class
* base class `WifiChannel` made abstract
* `WifiNetDevice::SetChannel()` removed
* a `WifiPhyState` helper class added
* addition of the `YansWifiChannel` and `YansWifiPhy` classes
* 17-11-2008; changeset [dacfd1f07538](http://code.nsnam.org/ns-3-dev/rev/dacfd1f07538)
* Change attribute `RxErrorModel` to `ReceiveErrorModel` in `CsmaNetDevice` for consistency between devices.

### Changed behavior

* 17-11-2008; changeset [ed0dfce40459](http://code.nsnam.org/ns-3-dev/rev/ed0dfce40459)
* Relax reasonableness testing in Ipv4AddressHelper::SetBase to allow the assignment of /32 addresses.
* 17-11-2008; changeset [756887a9bbea](http://code.nsnam.org/ns-3-dev/rev/756887a9bbea)
* Global routing supports bridge devices.

## Changes from ns-3.1 to ns-3.2

### New API

* 26-08-2008; changeset [5aa65b1ea001](http://code.nsnam.org/ns-3-dev/rev/5aa65b1ea001)
* Add multithreaded and real-time simulator implementation. Allows for emulated net devices running in threads other than the main simulation thread to schedule events. Allows for pacing the simulation clock at 1x real-time.
* 26-08-2008; changeset [c69779f5e51e](http://code.nsnam.org/ns-3-dev/rev/c69779f5e51e)
* Add threading and synchronization primitives. Enabling technology for multithreaded simulator implementation.

### New API in existing classes

* 01-08-2008; changeset [a18520551cdf](http://code.nsnam.org/ns-3-dev/rev/a18520551cdf)
* class `ArpCache` has two new attributes: `MaxRetries` and a `Drop` trace. It also has some new public methods but these are mostly for internal use.

### Changes to existing API

* 05-09-2008; changeset [aa1fb0f43571](http://code.nsnam.org/ns-3-dev/rev/aa1fb0f43571)
* Change naming of MTU and packet size attributes in CSMA and Point-to-Point devices
  After much discussion it was decided that the preferred way to think about the different senses of transmission units and encapsulations was to call the MAC MTU simply MTU and to use the overall packet size as the PHY-level attribute of interest. See the Doxygen of CsmaNetDevice::SetFrameSize and PointToPointNetDevice::SetFrameSize for a detailed description.
* 25-08-2008; changeset [e5ab96db540e](http://code.nsnam.org/ns-3-dev/rev/e5ab96db540e)
* bug 273: constify packet pointers.
  The normal and the promiscuous receive callbacks of the NetDevice API have been changed from:

  ```cpp
  Callback<bool,Ptr<NetDevice>,Ptr<Packet>,uint16_t,const Address &>
  Callback<bool,Ptr<NetDevice>, Ptr<Packet>, uint16_t,
           const Address &, const Address &, enum PacketType>
  ```

  to:

  ```cpp
  Callback<bool,Ptr<NetDevice>,Ptr<const Packet>,uint16_t,const Address &>
  Callback<bool,Ptr<NetDevice>, Ptr<const Packet>, uint16_t,
           const Address &, const Address &, enum PacketType>
  ```

  to avoid the kind of bugs reported in [bug 273](http://www.nsnam.org/bugzilla/show_bug.cgi?id=273). Users who implement a subclass of the `NetDevice` base class need to change the signature of their `SetReceiveCallback` and `SetPromiscReceiveCallback` methods.

* 04-08-2008; changeset [cba7b2b80fe8](http://code.nsnam.org/ns-3-dev/rev/cba7b2b80fe8)
* Cleanup of MTU confusion and initialization in `CsmaNetDevice`
  The MTU of the `CsmaNetDevice` defaulted to 65535. This did not correspond with the expected MTU found in Ethernet-like devices. Also there was not clear documentation regarding which MTU was being set. There are two MTU here, one at the MAC level and one at the PHY level. We split out the MTU setting to make this more clear and set the default PHY level MTU to 1500 to be more like Ethernet. The encapsulation mode defaults to LLC/SNAP which then puts the MAC level MTU at 1492 by default. We allow users to now set the encapsulation mode, MAC MTU and PHY MTU while keeping the three values consistent. See the Doxygen of `CsmaNetDevice::SetMaxPayloadLength` for a detailed description of the issues and solution.

* 21-07-2008; changeset [99698bc858e8](http://code.nsnam.org/ns-3-dev/rev/99698bc858e8)
* class NetDevice has added a pure virtual method that must be implemented by all subclasses:

  ```cpp
  virtual void SetPromiscReceiveCallback (PromiscReceiveCallback cb) = 0;
  ```

  All NetDevices must support this method, and must call this callback when processing packets in the receive direction (the appropriate place to call this is device-dependent). An approach to stub this out temporarily, if you do not care about immediately enabling this functionality, would be to add this to your device:

  ```cpp
  void
  ExampleNetDevice::SetPromiscReceiveCallback (NetDevice::PromiscReceiveCallback cb)
  {
    NS_ASSERT_MSG (false, "No implementation yet for SetPromiscReceiveCallback");
  }
  ```

  To implement this properly, consult the `CsmaNetDevice` for examples of when the `m_promiscRxCallback` is called.

* 03-07-2008; changeset [d5f8e5fae1c6](http://code.nsnam.org/ns-3-dev/rev/d5f8e5fae1c6)
* Miscellaneous cleanup of Udp Helper API, to fix bug 234

  ```cpp
  class UdpEchoServerHelper
  {
  public:
  - UdpEchoServerHelper ();
  - void SetPort (uint16_t port);
  + UdpEchoServerHelper (uint16_t port);
  +
  + void SetAttribute (std::string name, const AttributeValue &value);
  ApplicationContainer Install (NodeContainer c);
  ```

  ```cpp
  class UdpEchoClientHelper
  {
  public:
  - UdpEchoClientHelper ();
  + UdpEchoClientHelper (Ipv4Address ip, uint16_t port);
  - void SetRemote (Ipv4Address ip, uint16_t port);
  - void SetAppAttribute (std::string name, const AttributeValue &value);
  + void SetAttribute (std::string name, const AttributeValue &value);
  ApplicationContainer Install (NodeContainer c);
  ```

* 03-07-2008; changeset [3cdd9d60f7c7](http://code.nsnam.org/ns-3-dev/rev/3cdd9d60f7c7)
* Rename all instances method names using `Set..Parameter` to `Set..Attribute` (bug 232)
* How to fix your code: Any use of helper API that was using a method `Set...Parameter()` should be changed to read `Set...Attribute()`. e.g.,

  ```cpp
  - csma.SetChannelParameter ("DataRate", DataRateValue (5000000));
  - csma.SetChannelParameter ("Delay", TimeValue (MilliSeconds (2)));
  + csma.SetChannelAttribute ("DataRate", DataRateValue (5000000));
  + csma.SetChannelAttribute ("Delay", TimeValue (MilliSeconds (2)));
  ```

### Changed behavior

* 07-09-2008; changeset [5d836ab1523b](http://code.nsnam.org/ns-3-dev/rev/5d836ab1523b)
* Implement a finite receive buffer for TCP
  The native TCP model in TcpSocketImpl did not support a finite receive buffer. This changeset adds the following functionality in this regard:
* Being able to set the receiver buffer size through the attributes system.
* This receiver buffer size is now correctly exported in the TCP header as the advertised window. Prior to this changeset, the TCP header advertised window was set to the maximum size of 2^16 bytes. window
* The aforementioned window size is correctly used for flow control, i.e. the sending TCP will not send more data than available space in the receiver's buffer.
* In the case of a receiver window collapse, when a advertised zero-window packet is received, the sender enters the persist probing state in which it sends probe packets with one payload byte at exponentially backed-off intervals up to 60s. The receiver will continue to send advertised zero-window ACKs of the old data so long as the receiver buffer remains full. When the receiver window clears up due to an application read, the TCP will finally ACK the probe byte, and update its advertised window appropriately.
  See [bug 239](http://www.nsnam.org/bugzilla/show_bug.cgi?id=239) for more.
* 07-09-2008; changeset [7afa66c2b291](http://code.nsnam.org/ns-3-dev/rev/7afa66c2b291)
* Add correct FIN exchange behavior during TCP closedown
  The behavior of the native TcpSocketImpl TCP model was such that the final FIN exchange was not correct, i.e. calling Socket::Close didn't send a FIN packet, and even if it had, the ACK never came back, and even if it had, the ACK would have incorrect sequence number. All these various problems have been addressed by this changeset. See [bug 242](http://www.nsnam.org/bugzilla/show_bug.cgi?id=242) for more.
* 28-07-2008; changeset [6f68f1044df1](http://code.nsnam.org/ns-3-dev/rev/6f68f1044df1)
  * OLSR: HELLO messages hold time changed to `3*hello` interval from hello interval. This is an important bug fix as hold time == refresh time was never intentional, as it leads to instability in neighbor detection.
