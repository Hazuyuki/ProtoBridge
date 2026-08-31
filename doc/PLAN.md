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
  `configs/calibration/roce_p2p_ns3_sweep.py` + `--interNodeTopology=tor`
  (single shared leaf, 1 hop) reproduce `ib_write_lat` at **MAPE 5.26 %** (best fit
  `startup=950/interNodeLatency=100/switchCutThroughDelay=200/bw=400`; the 1–4 KB
  transition undershoots — ns-3 linear vs `ib_write_lat` sublinear S^0.669,
  structural). Completion is delivery-gated (the prior "dispatch time" claim is
  retracted). **ns-3 `ib_write_bw` per-QP ceiling — DONE:** opt-in
  `--perQpCeilingGbps=49` mounts `DcqcnRateMachine`'s true per-flow pacer
  (per-flow `nextSendNs`, spaces consecutive chunks to the ceiling);
  `configs/calibration/roce_p2p_bw_sweep.py` → converged **MAPE 3.20 %** for
  sizes ≥64 MB (256 MB 47.60 G ≈ 49 G per-QP), correcting the 7.1× no-ceiling
  overestimate (256 MB 348.3 G → 47.6 G). Transition zone <64 MB over-estimates
  (1–7 8 MB-coalesced chunks un-paced; the blog's sub-ceiling small-size regime
  is msg-issue-limited, not modeled — same structural gap as `ib_write_lat`).
  Layer caveat: perftest is raw RDMA, *below* the NCCL collective layer; it does
  not exercise the 40 µs NCCL startup. Detail in `doc/CALIBRATION.md` §"RoCE v2
  P2P (scale-out)".

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
- [x] ns-3 RoCE P2P `ib_write_lat` direct comparison: DONE. The inter-node `RoceProtocolModel` was already wired out-of-band (no `h200-roce.profile` needed); `configs/calibration/roce_p2p_ns3_sweep.py` + new `--interNodeTopology=tor` (single shared leaf, 1 hop) reproduce `ib_write_lat` at **MAPE 5.26 %** (best fit `startup=950/interNodeLatency=100/switchCutThroughDelay=200/interNodeBandwidth=400`; 2-link 2×ser = single-write rate). The prior "`.cfg` SEND_P2P `latencyNs` is dispatch time" claim is **retracted** — completion is delivery-gated (`ProtocolConfigRunner` `m_startNs` → `PACKET_DELIVERED` → `durationNs`).
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
