# ProtoBridge Plan (local checkpoint)

ProtoBridge-local roadmap + milestones + risks. The **authoritative** full
history is the parent `doc/PLAN.md`+`doc/TASKS.md` checkpoint bridge, which is
**read-only** to this repo's commits (commits `c55ea98`…). This file is the
protobridge-side resume source; refresh the parent bridge when parent write
access is available. Risk detail lives in `doc/RISKS.md`.

## Where we are (HEAD after WU-SoH5 + RoCE P2P perftest calibration + fullmesh surrogate)

Three-layer OTP/PEX/surrogate architecture. Scale-up (Su) NVLink/NVSwitch
fabric is calibrated (H200 ring/tree/NVLS). Scale-out (So) RoCEv2 Ethernet
fabric + hierarchical Su→So collectives were built in WU1-WU7 (commit
`cc50698`) and reviewed `ship-with-fixes`. The SoHeader/ECN effort then landed
in two phases:

- **Phase A (WU-SoH1..4, commits `f6adb5d`→`4ed7fc2`) — DONE, dormant by design.**
  Polymorphic `SoHeader` (10 B, ECN carrier) + `EthernetSwitch` ECN-mark
  egress hook (`OnPacketEnqueue` virtual, no-op on NvSwitch) + CNP control
  packet + ECN-echo trigger + `--ecnThreshold` CLI. Shipped parity-safe (off =
  bit-identical) because the live So send path emitted `protocol=0` packets
  with no SoHeader, so the guards never fired.

- **Phase B (WU-SoH5, this branch `feature/protocol-transaction-model`) — DONE
  for DCQCN; PFC deferred.** Activated the live loop (Seam B: `SendBulkWire
  TransferSize` stamps a SoHeader on RoCE chunks) + added the DCQCN rate machine
  (`dcqcn-rate-machine.{h,cc}`) + folded its shaping delay into the sole
  egress chokepoint `SendPacketOnDevice` + `ProcessCnpPacket` + `--dcqcn` CLI
  (default off) + 6 C++ tests green. Resolves R-SoH-1. End-to-end the loop is
  dormant under light load (faithful RoCE: no congestion → no DCQCN action) —
  see R-SoH-3/R-SoH-4 in `doc/RISKS.md`.

- **RoCE v2 P2P perftest calibration (surrogate) — DONE.** A third-party
  perftest blog (ConnectX-7 400G, `ib_write_bw`/`ib_write_lat`) was used as a
  *soft, externally-anchored* sanity calibration of the **raw-RDMA scalar
  constants** (per-QP ceiling 49 Gbps, wire efficiency 0.988, per-message
  doorbell 1.45 µs, single-write serialization 192 Gbps, msg-issue 8.16 Mmsg/s,
  4 KB saturation knee). The calibrated analytical model
  `make_connectx7_p2p_perftest_theory()` (surrogate, `collective="sendrecv"`)
  reproduces `ib_write_lat` at MAPE 2.6 %. ns-3 **does** support P2P
  (`P2pTraffic` + `FabricEndpoint::SendP2p` raw path + `.cfg` `kind=P2P`
  stencil, demo `configs/protocol_configs/p2p-single.cfg`). The perftest
  **validates** the ns-3 RoCE wire constants (wire_eff 0.95 slightly conservative
  vs 0.988; line rate 400 Gbps match) — **no ns-3 model change warranted**.
  **ns-3 `ib_write_lat` direct comparison — DONE:** the inter-node `RoceProtocolModel`
  was already wired out-of-band, so a `h200-roce.profile` was not needed;
  `examples/gpu-cluster/roce_p2p_ns3_sweep.py` + `--interNodeTopology=tor`
  (single shared leaf, 1 hop) reproduce `ib_write_lat` at **MAPE 5.26 %** (best fit
  `startup=950/interNodeLatency=100/switchCutThroughDelay=200/bw=400`; the 1–4 KB
  transition undershoots — ns-3 linear vs `ib_write_lat` sublinear S^0.669,
  structural). Completion is delivery-gated (the prior "dispatch time" claim is
  retracted). **ns-3 `ib_write_bw` per-QP ceiling — DONE:** opt-in
  `--perQpCeilingGbps=49` mounts `DcqcnRateMachine`'s true per-flow pacer
  (per-flow `nextSendNs`, spaces consecutive chunks to the ceiling);
  `examples/gpu-cluster/roce_p2p_bw_sweep.py` → converged **MAPE 3.20 %** for
  sizes ≥64 MB (256 MB 47.60 G ≈ 49 G per-QP), correcting the 7.1× no-ceiling
  overestimate (256 MB 348.3 G → 47.6 G). Transition zone <64 MB over-estimates
  (1–7 8 MB-coalesced chunks un-paced; the blog's sub-ceiling small-size regime
  is msg-issue-limited, not modeled — same structural gap as `ib_write_lat`).
  Layer caveat: perftest is raw RDMA, *below* the NCCL collective layer; it does
  not exercise the 40 µs NCCL startup. Detail in `doc/CALIBRATION.md` §"RoCE v2
  P2P (scale-out)".

- **SRNIC Fig. 11 measured loss-recovery comparison — DONE (task #33; re-run
  after bug fixes, task #34; strict-GBN receive semantics, task #35).**
  Second external measured anchor for the So
  fabric: SRNIC (NSDI'23) Figure 11 (CX-6 go-back-N vs SRNIC selective
  repeat, 100G, perftest 4 KB, CC disabled, FPGA-injected random loss)
  mapped onto the fabric's own `llrMode=gobackn` vs `llrMode=sack` axis —
  only that axis changes between arms. First round exposed + fixed a
  production gap (no-op LLR retry timeout → `--llrTimeoutRetransmit`
  backstop, default 0, parity-pinned). Alignment analysis then exposed
  three more model bugs, all fixed (task #34, inert at defaults): no
  TX-depth/BDP window (new `--soTxWindow`, ACK-clocked over per-packet LLR
  ACKs), retry timers armed at synchronous t≈0 dispatch (window makes
  RTT-scale 500 µs timeouts sound), and NACK-of-NACK for plr-dropped
  control packets (now dropped silently, matching the switch path).
  The remaining GBN-arm optimism (#34: 0.820/0.467 @1%, arms inverting at
  2%) traced to the receive path: bypass-mode dedup kept out-of-order
  originals, so blanket retransmits arrived as cheap duplicates. Task #35
  adds `--soGbnStrict` (default off, So-only — the gate requires a remote
  source rank, so the sprayed Su NVLink fabric is byte-identical): true IB
  responder semantics (in-order-only acceptance, OOO discard with no ACK,
  one NAK per gap, oracle drop-NACK suppressed → the window×plr cascade is
  gone). GBN arm re-calibrated at w=64 (rewind span; back-fit within
  [BDP=16, tx_depth=128], CX-6 reverse-solves to 70–130 pkts/drop). Final:
  strict GBN within ~3 pts of CX-6 up to 0.5% loss (0.919/0.933 @0.1%,
  0.725/0.744 @0.5%), ~+10 pts optimistic at 1–2% (no RTO-scale stalls;
  0.580/0.467, 0.383/0.278), mode ordering (GBN < SACK) restored at every
  rate; SACK unchanged within ~5 pts everywhere. Artifacts:
  `examples/gpu-cluster/srnic_fig11_cx6_reference.csv` + `eth_gbn_loss_sweep.py`
  + `p2p-roce-lossy{,-sack}.cfg` + `test/parity/test_so_gbn_strict.py`;
  write-up in `doc/CALIBRATION.md` §"Loss
  recovery under injected packet loss".

- **Demystifying-NCCL Fig. 6 Su protocol anchor — DONE (task #36).** Third
  external measured anchor, first for the *Su* (NVLink) protocol axis:
  arXiv:2507.04786 Fig. 6 intra-node panels (LL/LL128/Simple × Ring/Tree
  AllReduce, 4× GH200 quad on CSCS Alps, NCCL 2.19.1, 2^14–2^32 B) digitized
  by pixel extraction (gridline-calibrated ±2%, zoom insets for ≤2^22) into
  `examples/gpu-cluster/demystify_nccl_fig6_reference.csv`, and compared
  against the stock `NcclProtocolModel` with only the physical spec changed:
  ring = `--topology=ring --numLanes=18 --bandwidth=200` (450 GB/s per-GPU
  multi-ring aggregate — the measured busBW of 335 GB/s confirms the
  aggregation), tree = `--topology=fullmesh --numLanes=6` (one 150 GB/s pair
  per edge — measured 27.5 ms at 2^32 ≈ D/150). Exactly 6 startup intercepts
  fitted at 2^14; the rest is a zero-tune transfer test: per-curve MAPE
  15–26% (one-sided optimistic; worst at the 2^22–2^26 transition), and the
  bandwidth-independent 2^32 protocol ratios transfer within 3% on the tree
  path (LL/Simple 1.95 vs 2.01; LL128/Simple 1.04 vs 1.07) — independent
  confirmation of LL=0.5/LL128=0.9375. Known gaps documented, not tuned:
  ring LL −21% (our 0.5 is NCCL's bound; paper says LL reaches 25–50% of
  peak), LL128-vs-Simple ordering at ≥2^28 on ring inverted by ~2.5%
  (chunk-granularity artifact). Inter-node panels not comparable (Slingshot
  Dragonfly + host-proxy ≠ our RoCE/ETH). Artifacts:
  `demystify_nccl_sweep.py` + `.json` + comparison figure;
  `doc/CALIBRATION.md` §"NCCL protocol shapes (scale-up)". Measurement-only
  round — no model-code changes, all parity pins untouched.

- **TreeAllReduce full-volume rewrite — DONE (task #37, deviation A of the
  #36 analysis).** The Demystify anchor proved the old tree model unphysical:
  it moved only D/N bytes per tree edge per direction (one `m_chunkSize`
  send per step per hop), giving an effective per-edge bandwidth 129% of the
  150 GB/s GH200 pair spec and a 2^32 runtime (22.2 ms) *below* the physical
  bound D/BW = 28.6 ms; at N=4 the old schedule also never sent GPU 3's
  reduce contribution. Rewritten as a chunk-pipelined binary tree (public
  API unchanged, heap parent=(g−1)/2 kept): full D up every edge in reduce,
  full D down in broadcast, interior GPUs forward
  `[sent, min-over-children-received)` quantized to
  `clamp(D/(32·depth), 64 KiB, 8 MiB)` uniform chunks; progress is
  byte-counted per (flow, edge) — spray reorder / GBN duplicates safe; sw
  overhead knobs pace per hop (defaults 0 = zero extra events). Gotcha
  fixed on the way: `FabricEndpoint`'s receive callback passes the
  *receiver's* rank as its first argument, so the sender must be read from
  `header.GetSourceRank()`. Post-fix Demystify tree arm: MAPE LL 5.8% /
  LL128 8.1% / Simple 16.3% (was 16.9/15.2/26.4), 2^32 Simple 28.84 ms =
  1.007× physical bound, 2^32 protocol ratios within 1%; tree/ring cross
  ratio now +24.5% (tree honest, ring plateau optimism — deviation D —
  remains, pre-fix errors cancelled). Ring arm byte-identical; all gates
  green (C++ EXTENSIVE, surrogate 64, 5 parity suites incl. tree
  config-vs-inline bit-identical). `h200-tree-allreduce.cfg` linkDataRate
  80/100/50 flagged for recalibration. Deviations B–F recorded in memory
  (`demystify-deviation-backlog`), not approved for implementation.

- **Collective-injector correctness audit + 2D AlltoAll relay-key fix — DONE
  (task #38).** All 15 injectors swept for the two #37 bug patterns. Fixed:
  `AlltoAllInjector` 2D mode (UB-Mesh `--topology=2dfullmesh`) keyed the
  first-hop relay counter on the receive callback's first argument — the
  *receiver's* own rank — pooling every same-row sender's bytes toward the
  same final destination into one counter whose threshold `erase()` discarded
  overshoot; now keyed on `header.GetSourceRank()`. The bug is latent: bulk
  deliveries arrive atomically (one callback per completed transfer with
  effectiveSize = the full chunk), so pre/post timings are byte-identical on
  2×2/3×3/4×4 grids even with forced `--sprayChunkSize` fragmentation; any
  future fragmented delivery (loss paths) would have deadlocked the relay.
  Known open, recorded but not approved to fix: RingBroadcast delivers only
  D/N per GPU (time (N−1)/N·D/BW beats the physical bound); RingReduce moves
  (N−1)/N·D per link. Also flagged: surrogate `make_h200_tree_theory`
  calibrated against the pre-#37 tree. All gates green, parity pins
  unchanged.

- **RingBroadcast/RingReduce full-volume rewrite — DONE (task #39, closes
  the two known-open #38 findings).** Both rooted ring collectives beat the
  physical D/BW bound (broadcast/reduce busbw factor is 1: every ring edge
  carries full D). Broadcast additionally had wrong semantics — each
  non-root received only D/N. Rewritten chunk-pipelined in the #37 style:
  broadcast streams D from the root around the ring; reduce streams D from
  the chain head (root's ring predecessor) toward the root with interior
  forwarding gated on received bytes. Post-fix 64 MB times ≈ old × N/(N−1)
  and nearly N-invariant, just above D/BW; small sizes slightly faster
  (the old sequential store-and-forward latency per hop partially cancelled
  the volume deficit). Neither collective is calibrated or parity-pinned;
  all pins unchanged, all gates green.

- **Full-mesh surrogate algorithm (ns-3-calibrated) — DONE (this branch).** The
  surrogate gained a 4th algorithm shape, `fullmesh`, calibrated directly to
  ns-3 as ground truth (not a wire-rate derivation like ring/tree/nvls).
  `make_h200_fullmesh_theory()` + a `fullmesh` branch at every algo-dispatch
  site (`_steps`/`_per_step`/`_step_time`/`_credit_pressure`/`predict`/
  `_shared_contention`/`make_surrogate`/`make_surrogate_from_wire`). Model:
  `T = startup + steps·(lat + (D/N)/fullmesh_bw)` (AR steps=2 barrier-gated,
  AG steps=1) — structurally NVLS **without** the N-fold serialization factor
  (every GPU-pair has a dedicated `--numLanes=18` bundle, concurrent not
  serialized). `configs/calibration/fullmesh_ns3_sweep.py` sweeps N∈{4,8,16} ×
  {AR,AG} × 1–256 MiB (54 pts) and back-fits `fullmesh_bw = Σ(V²)/Σ(R·V)` on
  the SIMPLE-regime points → `fullmesh_bw=449890 B/µs` (η=0.9998 vs the
  18×25000=450000 physics, MAPE 0.10%); 7 pytests + a parity test
  (`test/parity/test_surrogate_vs_ns3_fullmesh.py`, within 0.3% at N=8 AR+AG
  {16,64,256} MiB) green. Detail in `doc/CALIBRATION.md` §"Full-mesh".

## Milestones

- [x] Phase A: SoHeader carrier + ECN mark + CNP echo + `--ecnThreshold` (parity-safe, dormant).
- [x] Phase B: activate live SoHeader send path + DCQCN rate machine + egress chokepoint + `--dcqcn` + tests.
- [x] RoCE P2P perftest calibration: surrogate `make_connectx7_p2p_perftest_theory` (MAPE 2.6 % vs `ib_write_lat`) + `roce_p2p_calib.py`/`.json` + `p2p-single.cfg` demo + `doc/CALIBRATION.md` §RoCE P2P.
- [x] ns-3 RoCE P2P `ib_write_lat` direct comparison: DONE. The inter-node `RoceProtocolModel` was already wired out-of-band (no `h200-roce.profile` needed); `examples/gpu-cluster/roce_p2p_ns3_sweep.py` + new `--interNodeTopology=tor` (single shared leaf, 1 hop) reproduce `ib_write_lat` at **MAPE 5.26 %** (best fit `startup=950/interNodeLatency=100/switchCutThroughDelay=200/interNodeBandwidth=400`; 2-link 2×ser = single-write rate). The prior "`.cfg` SEND_P2P `latencyNs` is dispatch time" claim is **retracted** — completion is delivery-gated (`ProtocolConfigRunner` `m_startNs` → `PACKET_DELIVERED` → `durationNs`).
- [x] ns-3 RoCE P2P `ib_write_bw` per-QP ceiling: DONE. Opt-in `PerQpCeilingGbps` (~49 G) on `DcqcnRateMachine::ShapingDelay` as a true per-flow pacer (per-flow `nextSendNs`, not a per-chunk offset); `roce_p2p_bw_sweep.py` → converged MAPE 3.20 % (≥64 MB); corrects the 7.1× no-ceiling overestimate. Resolves R-SoH-4 (per-flow pacer mechanism now exists; DCQCN congestion still per-chunk, acceptable).
- [ ] PFC: per-VC priority pause via `FlowControlGate` (F9 seam) — R-SoH-2 (deferred, invasive).
- [ ] DCQCN e2e contention proof: multi-flow incast scenario saturating one switch output port — R-SoH-3 (deferred; unit tests cover every link).
- [x] True per-flow rate pacer — R-SoH-4 RESOLVED for the per-QP ceiling (`PerQpCeilingGbps` uses per-flow `nextSendNs`); the DCQCN congestion path still uses per-chunk extra-serialization shaping (acceptable for the lower-bound-band surrogate anchor).
- [x] So RoCEv2 fabric simplification pass: independent multi-agent code review (27 agents, adversarially verified) found the fabric already lean — removed ~49 LOC of zero-caller dead code + provably-dead guards + stale comments (6 dead topo accessors, 3 unused getters, `PerGpuStartupDelayNs` knob, 2 dcqcn `if` wrappers, `m_interNodeSwitch*`→locals, 5 comment fixes). Behavior-preserving; parity re-verified byte-identical (su 4769616, 2-node 15307388, 4-node 13421212 ns).
- [x] Hierarchical (Su→So) surrogate documented: `surrogate/theory/USER_GUIDE.md` §6 + recipes J/K/L cover the model-agnostic `HierarchicalSurrogate` entry point, the 3 build levels (full-custom `TheoryDerivedSurrogate` / wire-derived `make_surrogate_from_wire` / preset), So topology families (`so_fullmesh`/`so_spineleaf`/`so_fattree`), and the collective phase decomposition — non-H200 examples verified byte-exact (809.92 / 450.39 / 540.76 / 701.85 µs); stale test-count 37→54 fixed in §9.
- [x] Full-mesh surrogate algorithm (ns-3-calibrated): `make_h200_fullmesh_theory` + `fullmesh` branches at 11 dispatch sites; `fullmesh_bw=449890 B/µs` back-fit to the ns-3 `FullMeshAllReduce`/`FullMeshAllGather` sweep (η=0.9998, MAPE 0.10% on the SIMPLE regime); 7 pytests + parity test (within 0.3% at N=8 AR+AG {16,64,256} MiB). The 4th algorithm shape — ring/tree/nvls → ring/tree/nvls/fullmesh. Detail in `doc/CALIBRATION.md` §"Full-mesh".
- [x] Full-mesh surrogate reviewed (independent 6-dimension multi-agent code review, 3-skeptic adversarial verify per finding): verdict SHIP-WITH-FIXES, **no blockers**. 3 refinements applied — "lower bound by construction"→"calibrated back-fit, not a bound" (the symmetric parity band + 8/30 above-ns-3 fit points contradict a one-sided lower bound); parity band [0.90,1.10]→[0.95,1.05] + ns-3-GT unit-test `rel 5e-2`→`2e-2` (the prior band was ~100× the 0.10% fit error); `WHITEBOX_SURROGATE.md` algo enumeration + fullmesh. 61 pytests + parity still green.
- [x] Plain (non-RoCE) lossy Ethernet scale-out fabric (ns-3 + surrogate, branch `feature/plain-ethernet-fabric`): the lossy counterpart to RoCEv2. **ns-3:** `EthernetProtocolModel` (eff=1.0 locked, `GetProtocolId=0`, store-and-forward 1500/800 ns, 9 KiB chunk) + `EthernetSwitch` lossy VOQ tail-drop → `NvSwitch::EmitNackBack` NACK → sender LLR/SACK retransmit (`ProcessNackPacket`→`HandleRetryRequest`/`HandleSackRequest`, `OnRetryTimeout` backstop, `--llrRetryLimit` permanent-loss fallback). `OnPacketEnqueue` `void→bool`(+`inPort`), base `true` (RoCE byte-identical); `MultiNodeTopologyHelper::SetInterNodeSwitchLlrEnabled` (default `false`=RoCE parity) arms the So switch's NACK origin; scratch lights the dead `ETHERNET` enum + forces LLR on endpoint+switch; `p2p-eth.cfg` + `p2p-eth-incast.cfg`. 2 C++ tests + incast smoke (87 drops→285 retx→0 permLoss, 68.7 µs); `--ber=0` isolates the lossy path. RoCE parity byte-identical (su 4769616 / 2n 15307388 / 4n 13421212 ns). **Surrogate:** `make_h200_ethernet_theory` (same 400GbE wire, store-and-forward lat 3.0/3.0, eff 1.0) — spec-derived lower bound anchored to ns-3 (lossy behavior ns-3-only, same split as RoCE/DCQCN); `make_h200_hierarchical_theory` `so_factory` seam (default RoCE); 3 pytests (61→64) + parity test (lower-bound anchor, ratio ~0.2 = RoCE hierarchical anchor). `eta_ethernet` skipped (latency not bandwidth difference). Doc: `CALIBRATION.md` §Plain Ethernet + `USER_GUIDE` recipe N + `WHITEBOX` So-profile mention.
- [x] Plain-Ethernet fabric independently reviewed (3 read-only adversarial subagents): SHIP-WITH-FIXES, no blockers; RoCE parity re-confirmed. 5 fixes — `voqDropThreshold=0` genuinely lossless (was a stall foot-gun), `voqDropThreshold<switchVoqDepth` invariant enforced (`NS_ABORT_MSG_IF`) + dead-`Config::SetDefault(1024)` comment corrected, `ETH`+`fullmesh`/`ring` inert-path warning, RoCE parity numbers pinned by `test_so_hierarchical_regression.py` (was doc-only), new `test_ethernet_lossy_recovery.py` automates the incast NACK→retransmit loop + permanent-loss fallback (was manual smoke).
- [x] Config-primary So fabric selection: the profile→CLI sourcing block now sources the 4 inter-node axes (`interNodeFabricType`/`Topology`/`BandwidthGbps`/`LatencyNs`) from `.cfg [stack]`/`.profile`, so one config adapts across protocols (ETH vs RoCE) with no `--interNode*` CLI. Backward-compatible (keys absent → CLI default); `numNodes`/`gpusPerNode`/`dataSize` stay CLI (scale axes). Demo `eth-hierarchical.cfg` + `test_config_vs_inline_eth.py` (config-driven ETH == inline CLI, bit-identical). Closes the config-primary So gap.
- [x] So traffic bypasses credit flow control: `SendBulkWireTransferSize` + `OnPacketDelivered` gate credit on `IsLocalRank(destRank)` — So packets neither consume nor return credits (the credit shell at the default `--vcCredits` depth ≫ bulk chunk never fired anyway). So flow control is honestly the lossless/lossy mechanism: ECN/DCQCN/per-QP ceiling (RoCE) + VOQ tail-drop/LLR retransmit (Ethernet). Su keeps full credit gating — unchanged. Su baseline 4769616 + So RoCE hierarchical 15307388/13421212 re-verified byte-identical (behavior-equivalent at the old depth). `--vcCredits` documented Su-only.
- [x] Multi-operation temporal contention modeling: closed the two gaps that left "can a collective and a time-offset P2P share the fabric and contend?" only partially supported. (1) **Multi-node contention wiring** — the multi-node endpoint loop in `scratch/gpu-cluster-sim.cc` now creates an **independent** `ContentionModel` per endpoint (backlog local to that endpoint's injection side; WFQ bandwidth = intra-node rate) — this DIFFERS from (not corrects) the single-node `GpuClusterTopologyHelper`, which shares ONE clone across all endpoints so backlog aggregates node-wide (two layers, not a correctness difference; the helper header comment said "cloned for each endpoint" and now states the sharing). WFQ penalty fires only at `numActive>1`, so pure-collective So traffic stays single-class and byte-identical (su 4769616 / 2n 15307388 / 4n 13421212 re-verified). (2) **Background P2P injection** — new `--bgP2p*` CLI injects a self-driven `P2pTraffic` flow at `--bgP2pStartTimeNs` that shares the fabric with the primary collective until it completes; `--bgP2pIntervalNs=0` means one packet serialization time (a literal 0 would loop at a fixed sim-time and stall). Default off (parity-identical). Smoke: 2-node RoCE 16 MiB hierarchical 1248928→1353663 ns (+8.4%, 129 × 1 MiB injected). **Independent review (2 read-only agents) found that +8.4% is entirely link sharing** — with all weights=1.0 (penalty exactly 0, model still mounted) the cross-node run is byte-identical at 1353663; the WFQ term fires but sits behind the over-driven 400G NIC queue. An intra-node bg (0→1) on the same run discriminates WFQ: 1311031 vs 1295971. Single-node Su 319658→450982 ns (+41% link sharing; WFQ −3.7%, 468378 with weights=1.0). Also fixed from the review: `bgP2pIntervalNs=0` integer division floored <100 B to 0 ns and stalled the sim (now clamped ≥1 ns); scratch comment + helper header still carried the "correct model" / "cloned per endpoint" framing. Build clean; C++ 65 PASS/0 FAIL; surrogate 64; RoCE + ETH + lossy-recovery parity green. Gated by `test/parity/test_contention_interference.py` (4 cases: interference > baseline + complete + injected bytes for both regimes, WFQ discriminator default≠weights=1.0 for both regimes, all 7 `simTimeNs` pinned).
- [x] RTTCC delay-based congestion control for the plain-Ethernet So fabric (`--rttcc`, default off): the second CC option beside DCQCN, for the fabric DCQCN cannot cover (ETH has no ECN/CNP loop; `EthernetProtocolModel` locks `WireEfficiency=1.0`, which the DCQCN So gate keys on — RTTCC gates on `!IsLocalRank(destRank)` instead). New `RttccRateMachine` (per-flow Swift-style rate law: over-target `rate ×= 1−Beta·(rtt−target)/rtt` floored at `MinRateGbps`, at/below-target `rate += AdditiveIncreaseGbps` capped at line rate; enforced as a TRUE per-flow pacer at the `SendPacketOnDevice` chokepoint, composing with DCQCN via `max(dcqcnDelay, rttccDelay)`), new `RTT_PROBE`/`RTT_RESPONSE` `FabricPacketType`s added to `IsControlPacket()` (BER-bypassed, never tail-dropped, LLR-skipped — but still VOQ-enqueued, so the sampled RTT sees real congestion queueing; timestamps stay sender-local in a seq→sentNs map, never on the wire), receiver reflects probes unconditionally. Mounted **per endpoint** (unlike DCQCN's shared machine): the flow key `MakeSeqKey(destRank, vc, flowId)` carries no source rank, so a shared machine would merge the pacer/probe state of two incast senders. CLI `--rttcc`/`--rttccTargetRttNs` (10000)/`--rttccProbeIntervalNs` (5000) + RESULT stats (`rttccProbesSent/RttSamples/MinRttNs/MaxRttNs/MinRateGbps`) + single-node no-op warning. Incast (`p2p-eth-incast.cfg`) smoke: 256KB — 7 probes/4 samples, rate 400→144.7 (too short to cut drops: 87→93); 4MB — 52 probes/samples, rate at the 1G floor under 4.6ms peak RTT, **drops 16388→16317, retx 25151→24911, permLoss 280→272**. Default off byte-identical (incast 68664/87/285/0; RoCE triple su 4769616/2n 15307388/4n 13421212). Gates: build clean (`WARNINGS_AS_ERRORS=ON`); C++ 66 PASS/0 FAIL at EXTENSIVE (new `RttccRateMachineTest`: parity-zero, probe cadence, MD/AI math, 300ns-slot pacer, stale-echo/unknown-flow ignores, stats); pytest 78 green (new `test/parity/test_rttcc_ethernet.py`: default-off byte-identity pin + loop-alive pin 73700/93/279). Idealizations in `doc/RISKS.md` (R-RTTCC-1).
- [x] Scale-out topology expansion (rail/leafspine/torus2d/dragonfly → 8 `--interNodeTopology` values) + gateway-forwarding flood fix: three wiring shapes — one-NIC-per-GPU switched (host/tor/**leafspine** with `--interNodeSpines` per-MAC deterministic ECMP `destLocalIdx % S`), rdmaRouteMap + PXN proxy (fullmesh/**rail** = per-local-rank shared rail switch), GPU0-gateway multi-hop (ring/**torus2d** dimension-order wrap routing `--interNodeTorusWidth`/**dragonfly** palmtree minimal l-g-l `--interNodeDragonflyGroupSize`; all shape knobs 0=auto). `CreateInterNodeSwitch` unifies rail/leafspine switch config incl. LLR arming → lossy-ETH NACK path verified (rail 35 retx / leafspine 210 retx, 0 permLoss). Root-caused a LATENT pre-existing gateway flood storm (`ring` included): forwarded packets kept the first hop's header DestMac, unknown at the destination NVSwitch → flood → non-target GPUs re-forward → exponential at gpusPerNode≥8 (4n×8g never terminated; 4n×4g had 477× rx amplification). Fix = rewrite header DestMac per hop at both forwarding sites (fabric-endpoint.cc). New gate `test/parity/test_so_topologies.py` (8 topologies, completion + losslessness + deterministic pins in `so_topologies_pins.json`; rail==tor 297430 ns consistency signal). All existing parity pins unchanged (So 4769616/15307388/13421212, config-vs-inline ETH bit-identical, lossy recovery, C++ EXTENSIVE, surrogate 67).
- [x] Uniform-random traffic mode (`--randTraffic`) + 8-topology throughput-vs-injection-rate experiment under DCQCN: new fixed-duration synthetic-load mode in scratch (default off = parity-safe) — every rank self-schedules fixed-interval `--randMsgBytes` messages to uniform-random ranks on OTHER nodes at `--randInjectionRate`×line rate; offered/delivered counted only inside the `--randWarmupNs`+`--randMeasureNs` window, RngRun-reproducible. **DCQCN-chain fix**: inline runs mount the NCCL payload builder and sent `NcclProtocol::SIMPLE`(3), so `stampSoHeader` never fired and the `EthernetSwitch` ECN guard (`GetProtocol()==RoceProtocol::SIMPLE`(1)) never matched — zero ECN under a genuine 4:1 congestion; fix = on the RoCE fabric mount `RocePayloadBuilder` + send `RoceProtocol::SIMPLE` (exactly the config-mode RoCE mounting). New observability: `EthernetSwitch::GetEcnMarkedPackets` + endpoint CNP tx/rx counters in every RESULT block. Verified on 4:1 oversubscribed leafspine (1 µs links, 1 ms window): ecn=4845/cnpTx=1195/cnpRx=278; DCQCN on/off delivered identical — in a lossless fabric the saturated bottleneck port stays busy either way, DCQCN relocates queueing to senders (activity shown via ECN counts). Experiment `examples/gpu-cluster/random_traffic_dcqcn/` (88 runs = 8 topologies × 11 rates, 8n×4g RoCE 400G `--dcqcn`; results.csv + 2-panel figure): fullmesh linear to the 12.8 Tbps line rate; switched shapes (host/tor/rail/leafspine) bend at ~0.7 to the ~75% uniform-random destination-collision limit with ECN/CNP rising sharply past 0.6; gateway shapes saturate at the GPU0 funnel (ring ~2.2 T / dragonfly ~3.8 T / torus2d ~4.5 T; no EthernetSwitch in the So path → ECN structurally 0). New gate `test/parity/test_random_traffic.py` (non-blocking tor delivers ≥0.9×offered; 4:1 leafspine congests <0.5×offered AND lights ecn/cnpTx/cnpRx). Substantially addresses deferred D2 (DCQCN e2e contention proof). All gates green: 7 parity suites (pins unchanged), C++ EXTENSIVE, surrogate 67.
- [x] So payload-builder seam + deterministic-traffic topology comparison (16 nodes × 8 GPUs collectives under DCQCN): `FabricEndpoint::SetSoPayloadBuilder`/`GetPayloadBuilderForRank` mirror the `SetInterNodeProtocolModel` Su/So split — a mounted So builder frames remote-rank bulk sends (wire protocolId overridden to the So protocol model's own id, RoCE → `RoceProtocol::SIMPLE`) while Su traffic keeps the NCCL builder untouched. Fixes DCQCN dormancy on inline collectives (hierarchical injectors pass NCCL ids → SoHeader stamp + switch/receiver RoCE guards never fired); `--dcqcn=1` on RoCE mounts a shared So `RocePayloadBuilder` in scratch; `--dcqcn=0` byte-identical (SoHeader within wireSize budget, all pins intact). Experiment `examples/gpu-cluster/collective_topology_dcqcn/`: 80 runs (allreduce+alltoall × 8 topologies × 1–256 MiB, 128 ranks, RoCE 400G, DCQCN on) — fullmesh dominates (AR ~463 / A2A ~694 Gbps busbw), switched shapes cluster at the single-NIC bound with live ECN/CNP, ring worst (A2A ~21 Gbps GPU0 funnel), gateway shapes ECN structurally 0; dcqcn on/off completion bit-identical on oversubscribed leafspine (lossless-fabric finding extends to finite collectives). New gate `test/parity/test_collective_dcqcn.py`. All gates green.

## Build / test gate (DoD)

- Build: `./ns3 configure -d debug --enable-tests --enable-examples && ./ns3 build` (debug = `WARNINGS_AS_ERRORS=ON`; new `.cc` must be in `CMakeLists.txt` SOURCE_FILES + re-run configure).
- C++ tests: `./build/utils/ns3.47-test-runner-debug --test-name=gpu-cluster --fullness=EXTENSIVE --verbose` (QUICK silently skips).
- Surrogate: `python3 -m pytest surrogate/test/ -q`.
- Parity (SoH): `--dcqcn` off == pre-Phase-B bit-identical (su 4769616, 2-node 15307388, 4-node 13421212 ns).

## Constraints (binding)

- ONLY `protobridge/` is modifiable; parent `/share_data/zhuyu/ns-3.47` is READ-ONLY (incl. parent `doc/`).
- No `InternetStackHelper` on Su nodes; So RoCEv2 builds on the PEX substrate (FabricHeader + FabricEndpoint + EthernetSwitch), not a real L3 stack.
- Stock ns-3 `src/` modules are not edited. Split files (README/CONTRIBUTING/AUTHORS/CHANGES/RELEASE_NOTES) are edited only ABOVE the `---` separator.
- Metax-scrub red-line: no 沐曦 numbers by design.
