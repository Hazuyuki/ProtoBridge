# ProtoBridge Task Checklist (local checkpoint)

Task ledger with status. Authoritative parent mirror is `doc/TASKS.md` in the
parent repo (read-only to this repo's commits). Risk detail in `doc/RISKS.md`.

## SoHeader / ECN effort (current branch `feature/protocol-transaction-model`)

| # | Task | Pri | Slot | Status |
|---|------|-----|------|--------|
| 8  | WU-SoH1: real polymorphic SoHeader + RocePayloadBuilder refactor | — | — | ✅ done |
| 9  | WU-SoH2: EthernetSwitch ECN-mark egress hook | — | — | ✅ done |
| 10 | WU-SoH3: CNP control packet + recv-side emit | — | — | ✅ done |
| 11 | WU-SoH4: `--ecnThreshold` CLI + parity + C++ tests (Phase A closeout) | — | — | ✅ done |
| 13 | Seam B activate: bulk send stamps within-budget SoHeader + `SetProtocol(SIMPLE)` | — | — | ✅ done |
| 14 | CNP-reaction e2e test: low EcnThreshold proves the loop lights | — | — | ✅ done |
| 15 | DCQCN rate machine + `ProcessCnpPacket` + `SendPacketOnDevice` chokepoint + `--dcqcn` CLI + CNP BER-bypass + 4 tests | — | — | ✅ done |
| 16 | PFC defer: CHANGES.md + new `doc/RISKS.md` | — | — | ✅ done |
| 17 | Closeout: `doc/PLAN.md`+`doc/TASKS.md` + build/tests green + commit | — | — | ✅ done |
| 12 | WU-SoH5 (Phase B umbrella): DCQCN rate machine + PFC pause | — | — | 🔄 DCQCN done; PFC deferred (R-SoH-2) |
| 18 | RoCE P2P perftest calibration: surrogate `make_connectx7_p2p_perftest_theory` + 2 pytests (MAPE 2.6 % vs `ib_write_lat`) | — | — | ✅ done |
| 19 | ns-3 `--collective=p2p` inline CLI + opt-in per-QP ceiling | RoCE PEX profile + inter-node model wiring | 🔁 rescoped → WU1–WU3 (deferred) |
| 20 | ns-3 RoCE P2P sweep harness vs perftest | RoCE profile + delivery-complete instrumentation | 🔁 rescoped → WU1–WU3 (deferred) |
| 21 | Record calibration provenance (`doc/CALIBRATION.md` §RoCE P2P) + `PLAN.md`/`TASKS.md`/`CHANGES.md` + commit | — | — | ✅ done |

## Deferred (open)

| # | Task | Blocker | Risk |
|---|------|---------|------|
| D1 | PFC per-VC priority pause | `FlowControlGate` threading through ~7 bulk-send paths | R-SoH-2 |
| D2 | DCQCN e2e contention proof (multi-flow incast) | needs a saturating-incast scenario not in default collective path | R-SoH-3 |
| D3 | True per-flow rate pacer (vs per-chunk shaping delay) | per-flow rate-limiter event loop | R-SoH-4 |

## Verification status (current HEAD)

- Build: clean (`./ns3 build` → ninja no work to do; configure re-runs pick up new `dcqcn-rate-machine.{h,cc}`).
- C++ tests: `gpu-cluster` suite PASS at QUICK (1.586 s); SoH tests all green — `EthernetSwitchEcnTest`, `CnpReactionTest`, `DcqcnRateMachineTest`, `CnpDispatchTest`, `SoOnlyPredicateTest`, `DormantTest`.
- Parity: `--dcqcn` off == pre-Phase-B bit-identical (su 4769616, 2-node 15307388, 4-node 13421212 ns).
- RoCE P2P calibration: surrogate `make_connectx7_p2p_perftest_theory` MAPE 2.60 % vs `ib_write_lat`; 54 surrogate pytests green; `configs/calibration/roce_p2p_calib.py` runs clean → `roce_p2p_calib.json`; `configs/protocol_configs/p2p-single.cfg` runs — NVLink(Su) P2P verified `simTimeUs` 2 B → 1.2 µs, 8 MiB → 836.8 µs (1 pkt tx/rx, 0 dropped). No C++ touched this turn.
