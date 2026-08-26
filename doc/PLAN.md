# ProtoBridge Plan (local checkpoint)

ProtoBridge-local roadmap + milestones + risks. The **authoritative** full
history is the parent `doc/PLAN.md`+`doc/TASKS.md` checkpoint bridge, which is
**read-only** to this repo's commits (commits `c55ea98`…). This file is the
protobridge-side resume source; refresh the parent bridge when parent write
access is available. Risk detail lives in `doc/RISKS.md`.

## Where we are (HEAD after WU-SoH5 + RoCE P2P perftest calibration)

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
  retracted). The per-QP ceiling for `ib_write_bw` sustained throughput remains
  open.
  Layer caveat: perftest is raw RDMA, *below* the NCCL collective layer; it does
  not exercise the 40 µs NCCL startup. Detail in `doc/CALIBRATION.md` §"RoCE v2
  P2P (scale-out)".

## Milestones

- [x] Phase A: SoHeader carrier + ECN mark + CNP echo + `--ecnThreshold` (parity-safe, dormant).
- [x] Phase B: activate live SoHeader send path + DCQCN rate machine + egress chokepoint + `--dcqcn` + tests.
- [x] RoCE P2P perftest calibration: surrogate `make_connectx7_p2p_perftest_theory` (MAPE 2.6 % vs `ib_write_lat`) + `roce_p2p_calib.py`/`.json` + `p2p-single.cfg` demo + `doc/CALIBRATION.md` §RoCE P2P.
- [x] ns-3 RoCE P2P `ib_write_lat` direct comparison: DONE. The inter-node `RoceProtocolModel` was already wired out-of-band (no `h200-roce.profile` needed); `configs/calibration/roce_p2p_ns3_sweep.py` + new `--interNodeTopology=tor` (single shared leaf, 1 hop) reproduce `ib_write_lat` at **MAPE 5.26 %** (best fit `startup=950/interNodeLatency=100/switchCutThroughDelay=200/interNodeBandwidth=400`; 2-link 2×ser = single-write rate). The prior "`.cfg` SEND_P2P `latencyNs` is dispatch time" claim is **retracted** — completion is delivery-gated (`ProtocolConfigRunner` `m_startNs` → `PACKET_DELIVERED` → `durationNs`).
- [ ] ns-3 RoCE P2P `ib_write_bw` per-QP ceiling: opt-in `PerQpCeilingGbps` (~49 G) on `DcqcnRateMachine::ShapingDelay` for sustained-throughput parity (large-dataSize pipelined single-QP transfer); NOT needed for the `ib_write_lat` single-transfer sweep above.
- [ ] PFC: per-VC priority pause via `FlowControlGate` (F9 seam) — R-SoH-2 (deferred, invasive).
- [ ] DCQCN e2e contention proof: multi-flow incast scenario saturating one switch output port — R-SoH-3 (deferred; unit tests cover every link).
- [ ] True per-flow rate pacer (vs per-chunk shaping delay) — R-SoH-4 (deferred; acceptable for lower-bound-band surrogate anchor).

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
