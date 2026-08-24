# ProtoBridge Risk Register

ProtoBridge-local risk ledger (触发条件 / 预案 / 回滚点). The cross-repo
checkpoint bridge lives in the parent `doc/PLAN.md`+`doc/TASKS.md`, which is
**read-only** to this repo's commits; this file is the protobridge-side mirror
so resume/rollback decisions survive without parent write access.

| ID | Title | Status | Trigger | Impact | Contingency | Rollback |
|----|-------|--------|---------|--------|-------------|----------|
| R-SoH-1 | SoHeader/ECN loop dormant in sim | **RESOLVED** | Phase A shipped the ECN carrier+mark+CNP seam but no live So send path emitted `protocol=RoCE::SIMPLE` packets carrying a SoHeader, so the mark/CNP guards never fired on production traffic. | ECN/DCQCN correct but unobservable e2e; parity held only because the mechanism did not trigger. | Seam B (WU-SoH5): `SendBulkWireTransferSize` now stamps a SoHeader on every RoCE chunk when `efficiency<1.0 && m_payloadBuilder==RocePayloadBuilder` (So endpoints only). | `git revert` the Seam B hunk in `fabric-endpoint.cc` (`SendBulkWireTransferSize` SoHeader stamp) — restores the Phase-A dormant state, bit-identical simTime. |
| R-SoH-2 | PFC (priority flow control) deferred | **OPEN (deferred)** | PFC is the lossless backpressure complement to DCQCN (per-VC pause on VOQ overflow). Implementing it requires threading `FlowControlGate` (presently called only from `SendMemoryRead`; the ~7 bulk-send paths bypass it) through the parity-sensitive send paths. | No L2 backpressure under oversubscription; the So fabric is lossless only via cut-through + drop-tail VOQ, not pause. DCQCN alone governs egress rate. | Ship DCQCN (WU-SoH5) without PFC; document the bound. PFC lands via the `FlowControlGate` virtual hook (F9 seam) once the bulk-send paths are threaded. | The F9 `FlowControlGate` hook is additive (virtual override, default admit) — PFC rollback = leave the hook dormant, as today. No parity impact. **Unqueue condition for PFC:** a paused VC resumes when the downstream VOQ drains below the pause threshold (hysteresis, not edge-triggered) — this must be modeled at the `FlowControlGate` seam, not at the endpoint, to keep credit/PFC layers orthogonal. |
| R-SoH-3 | DCQCN e2e dormant under light load + synchronous-bulk-send fidelity bound | **OPEN (accepted bound)** | With `--dcqcn` ON, the single-flow hierarchical allreduce smoke is bit-identical to OFF (15307388 ns). Root cause (verified): `NvSwitch::EnqueueToVoq` calls `OnPacketEnqueue` with the *arrival* VOQ depth *before* enqueue, and the empty-VOQ fast-path forwards at depth 0 → `MarkEcnIfCongested` sees `voqDepth=0 < threshold` → no mark. The non-blocking cut-through fabric builds no backlog under one flow, so no CNP, so Alpha stays 0. | DCQCN rate-backoff is not exercised e2e by the default collective smoke tests; it *is* exercised at every link by the 6 unit tests (switch mark, CNP emit, CNP dispatch, rate math, So-only shaping, dormancy). | (a) Document as a faithful RoCE bound (DCQCN engages only under real congestion); (b) to exercise e2e, run a multi-flow incast/oversubscription scenario that saturates a switch output port (deferred — not in the default collective path). | None — the bound is structural (non-blocking fabric + single flow), not a wiring bug. Reverting the rate machine restores the Phase-A state but does *not* change the smoke numbers, proving the bound is real. |
| R-SoH-4 | Synchronous bulk-send loop = shaping, not true rate pacing | **OPEN (accepted bound)** | `SendBulkWireTransferSize` issues all chunks of a transfer synchronously in one event; the DCQCN shaping delay is a per-chunk *deferred send* (schedule the next `dev->Send` at Now+ShapingDelay), not an inter-departure pacer that serializes across chunks. | Under sustained congestion the model approximates rate-backoff as an added latency, not a true bandwidth cap; over-/under-shoot is possible relative to a packet-level DCQCN implementation. | Document; acceptable for a latency-first surrogate anchor (the ns-3 So term is a lower-bound band, like NVLS). A true pacer needs a per-flow rate-limiter event loop — deferred. | Leave `--dcqcn` default-off; the bound is invisible at zero congestion (parity-safe). |

## Notes

- **Read-only parent bridge.** The parent `doc/PLAN.md`+`doc/TASKS.md` checkpoint
  (commits `c55ea98`…) mirrors protobridge commits and is read-only to this
  repo's working sessions. When parent write access is available, refresh the
  parent bridge to the latest protobridge HEAD; until then, this file + the
  protobridge-local `doc/PLAN.md`/`doc/TASKS.md` are the resume source.
- **Parity invariant.** Every SoH change ships default-off / null-path so that
  `--dcqcn` off and `--ecnThreshold` default are bit-identical to the
  pre-SoH baseline (su 4769616, 2-node 15307388, 4-node 13421212 ns). Any change
  that perturbs these numbers without an explicit congestion scenario is a bug.
