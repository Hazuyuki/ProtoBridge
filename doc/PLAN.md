# ProtoBridge Plan (local checkpoint)

ProtoBridge-local roadmap + milestones + risks. The **authoritative** full
history is the parent `doc/PLAN.md`+`doc/TASKS.md` checkpoint bridge, which is
**read-only** to this repo's commits (commits `c55ea98`…). This file is the
protobridge-side resume source; refresh the parent bridge when parent write
access is available. Risk detail lives in `doc/RISKS.md`.

## Where we are (HEAD after WU-SoH5)

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

## Milestones

- [x] Phase A: SoHeader carrier + ECN mark + CNP echo + `--ecnThreshold` (parity-safe, dormant).
- [x] Phase B: activate live SoHeader send path + DCQCN rate machine + egress chokepoint + `--dcqcn` + tests.
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
