# Calibration

All calibration is against **NVIDIA H200 NVLink4** hardware. There is no A800
calibration in this release.

## Hardware reference

H200 NVLink4 is the calibration reference; the fabric-hardware + protocol
values live in
[`configs/protocol_profiles/h200-ll128.profile`](../configs/protocol_profiles/h200-ll128.profile)
(annotated in [`doc/CONFIG_GUIDE.md`](CONFIG_GUIDE.md) §3). The 4-GPU baseline
recipe below carries the exact runtime values it uses.

## Calibration data (`surrogate/calibration/`)

| File | Contents |
|------|----------|
| `h200_ring4_ar_baseline.json` | 27 raw ns-3 ring-allreduce points, 4 GPU, sizes 64 KiB – 1 GiB, 3 seeds each (`dataSize`, `numGpus`, `algorithm`, `simTimeUs`, …) |

## Surrogate accuracy

| Surrogate | Calibration set | Metric | Value |
|-----------|-----------------|--------|-------|
| Theory-derived (`surrogate/theory/`) | calibrated — ring_bw=177000 B/µs back-fit to the H200 HW point (first-principles wire derivation = 180000; 177000 sits 1.7% below), not a pure physical derivation | vs ns-3 | lower bound vs the 4-GPU ring baseline (≈0.20–0.57× at vcCredits=64; the baseline JSON records no credit value); ring (8-GPU/256MB) credit-sensitive — cr=128 reproduces HW 1347.65 µs (−0.003%), cr=64 → +42.3%; nvls credit-insensitive (≈0.566× vs 8-GPU ns-3) |
| Theory-derived full-mesh (`surrogate/theory/`) | ns-3-calibrated — `fullmesh_bw=449890 B/µs` back-fit to the ns-3 `FullMeshAllReduce`/`FullMeshAllGather` sweep (η=0.9998 vs the 18×25000 dedicated-bundle physics) | vs ns-3 | MAPE 0.10% on the SIMPLE-regime sweep points (N∈{4,8,16}, 1–256 MiB); parity within 0.3% at N=8 AR+AG {16,64,256} MiB (see the Full-mesh section below) |

## Generate fresh ns-3 baselines

To regenerate `h200_ring4_ar_baseline.json` (or any baseline) from the
simulator:

```bash
# 4-GPU ring allreduce, one size at a time. The sim is deterministic, so one
# run per size reproduces the baseline byte-for-byte (seeds 1/2/3 are
# identical). ber=0 => FEC/retry inactive, so no --fecN/K/T or --llr* needed.
for sz in 65536 262144 1048576 4194304 16777216 67108864 \
          134217728 268435456 1073741824; do
  ./ns3 run "gpu-cluster-sim --numGpus=4 --topology=ring --algorithm=ring \
    --dataSize=$sz --bandwidth=400 --latency=400 \
    --startupLL=15000 --startupLL128=25000 --startupSIMPLE=46000"
done
```

Each run emits `RESULT_START … simTimeUs=… RESULT_END`; collect the
`simTimeUs` values into the baseline JSON. The baseline uses `--bandwidth=400`
(the 8-GPU calibrated configs use the profile's `bandwidthGbps=170`; the 4-GPU
ring baseline uses the `400` override) and `ber=0`, so FEC/retry are inactive.
Reference points: 1 MiB → 61.0 µs, 4 MiB → 182.6 µs, 256 MiB → 8101.5 µs.

## RoCE v2 P2P (scale-out) — perftest-anchored, soft external calibration

The scale-out (So) RoCEv2 fabric has **no real-HW anchor** in this release (the
A800 calibration target is single-node NVLink; it cannot produce inter-node
RoCE measurements). A third-party perftest blog
(`/share_data/zhuyu/RoCEv2_benchmark_data.md`, ConnectX-7 400G, RoCEv2 GID idx
3, MTU 4096, RC, TX depth 128) is used as a **soft, externally-anchored sanity
calibration** of the *raw-RDMA scalar constants* — it is a third-party blog,
not peer-reviewed, not the project's A800 hardware.

### Layer distinction (the key caveat)

The perftest measures **raw RDMA** (one-sided `ib_write_bw` / `ib_write_lat`)
— the layer *below* NCCL. There is no collective algorithm, no GPU kernel
launch, no NCCL persistent-kernel wakeup. It therefore calibrates the raw
wire + NIC constants, **not** the NCCL-collective timing. The ns-3
`RoceProtocolModel`'s 40 µs startup is the NCCL-launch layer (validated
separately for the Su fabric); the perftest does **not** exercise it.

### Calibration script + extracted constants

[`configs/calibration/roce_p2p_calib.py`](../configs/calibration/roce_p2p_calib.py)
encodes the blog data and fits two models; the extracted constants land in
[`configs/calibration/roce_p2p_calib.json`](../configs/calibration/roce_p2p_calib.json):

| Constant | Value | How extracted | Fit quality |
|----------|-------|----------------|-------------|
| per-QP single-flow ceiling | 49.0 Gbps | single-QP `ib_write_bw` saturation | direct |
| wire efficiency | 0.988 | 8-QP saturation 395/400 = 0.9875 | direct |
| per-message fixed latency | 1.45 µs | 2-byte `ib_write_lat` (doorbell + prop + completion) | — |
| single-write serialization rate | ~199 Gbps | 2B→64 KB `ib_write_lat` slope (JSON stores 199.26; surrogate uses the round 192 Gbps = 24 k B/µs, which fits the linear endpoints better) | — |
| NIC message-issue rate | ~8.2 Mmsg/s | 2B/4B/64B `ib_write_bw` (∝ S; mean 8.24, 64 B point 8.16) | MAPE 1.6 % |
| `ib_write_lat` curve L(S)=a+b·S^c | a=1.45, b=1.65e-3, c=0.669 | 4-point `curve_fit` | MAPE 0.11 % |
| saturation knee | 4 KB | 90 % of ceiling | — |
| line rate | 400 Gbps | ConnectX-7 spec | match |

The bandwidth ramp (MAPE 19.8 % on a smooth Hill form) is perftest-TX-depth-128
pipeline dynamics — **not** a calibration target; only the ceiling (49 Gbps) and
the message-issue rate are calibration-relevant.

### Calibrated analytical model (surrogate)

`make_connectx7_p2p_perftest_theory()` (in `surrogate/theory/whitebox_surrogate_v2.py`)
returns a `TheoryDerivedSurrogate` configured so `predict(D, 2, "ring",
credits=128, collective="sendrecv")` reproduces `ib_write_lat`: per-message
fixed latency 1.45 µs (doorbell+completion 1.05 µs + same-switch propagation
0.4 µs), single-write effective rate 192 Gbps (24 000 B/µs — the round value
nearest the ~199 Gbps measured 2B→64 KB slope; NIC-processing-limited, below
the 400 G line rate; already effective so `wire_efficiency=1.0`),
single QP (`num_lanes=1`). Fit: 2 B / 64 B / 64 KB within 2 %, the 1 KB
transition within 8 % (the single-write curve is sublinear in NIC processing;
the linear surrogate fits the endpoints), overall **MAPE 2.6 %**.

The per-QP throughput ceiling (~49 Gbps) and the 0.988 wire efficiency are
**throughput / aggregate** concepts — they are **not** expressed by this
single-transfer latency model (the surrogate's `predict()` is a latency, not a
sustained-throughput model). The 49 Gbps ceiling would be applied in ns-3 via
an opt-in per-flow rate cap (follow-up, see below).

### What this validates in the ns-3 RoCE model

- `wire_efficiency` 0.95 → **validated as slightly conservative** (8-QP
  best-case saturation 0.9875; 0.95 is a reasonable operating value). No change.
- line rate 400 Gbps → **match**. No change.
- per-QP ceiling 49 Gbps + doorbell 1.45 µs → raw-RDMA constants; the ns-3
  NCCL-collective layer uses multiple QPs (aggregate → ~400 G) and is
  NCCL-launch-dominated, so these do **not** change collective timing. They
  are documented constants for the raw-RDMA layer.

### ns-3 P2P support (status)

ns-3 **does** support P2P: `P2pTraffic` + `FabricEndpoint::SendP2p` (the raw
single-packet send, no NCCL startup) + the `.cfg` `[op]` stencil `kind = P2P`
(`ProtocolTransactionKind::P2P_TRANSFER`). A runnable example is
[`configs/protocol_configs/p2p-single.cfg`](../configs/protocol_configs/p2p-single.cfg)
(`startup = 0` → raw transfer; verified: packets sent/received, `simTimeUs`
scales with size — 2 B → 1.2 µs, 8 MiB → 836.8 µs).

**Gaps (deferred) — updated after the ns-3 sweep:**
- ~~The `.cfg` SEND_P2P completion measures dispatch time (`latencyNs` is fixed
  at 400 ns regardless of size), not delivery latency.~~ **Retracted:** completion
  is **delivery-gated**. `ProtocolConfigRunner::Start()` stamps `m_startNs`
  (protocol-config-runner.cc:109); `OnAction` routes `SEND_P2P` through
  `SendBulkWireTransferSize` (runner.cc:136); `OnPacketReceived` builds a
  `PACKET_DELIVERED` event (runner.cc:165) that satisfies the transaction-graph
  WAIT node; `OnComplete` computes `durationNs = Now - m_startNs` (runner.cc:185).
  The `latencyNs = 400` is only a config echo, not the measured duration. Proven
  empirically by the ib_write_lat sweep
  ([`configs/calibration/roce_p2p_ns3_sweep.py`](../configs/calibration/roce_p2p_ns3_sweep.py)):
  `simTimeUs` scales with size (2 B → 1.50 µs, 64 KB → 4.40 µs), impossible for
  fixed-dispatch. So `simTimeUs` **is** the single-transfer delivery latency,
  directly comparable to `ib_write_lat`.
- The inter-node `RoceProtocolModel` **is** wired (scratch/gpu-cluster-sim.cc
  attaches it on every endpoint when `--interNodeFabricType=RoCE`;
  `FabricEndpoint::GetProtocolModelForDest`, fabric-endpoint.cc:716-726, selects
  it for non-local dests under the `m_localRankCount > 1` guard). A RoCE P2P run
  works **today** via `p2p-roce.cfg` + `--interNodeTopology=tor`
  + `--interNodeBandwidth=400 --interNodeLatency=100`, reproducing `ib_write_lat`
  at **MAPE 5.26 %** (sweep best fit; see the sweep JSON). A dedicated
  `h200-roce.profile` PEX bundle is **not** required — `[stack] = h200-ll128`
  governs only the Su model; the RoCE model is wired out-of-band.
- `--collective=p2p` inline CLI remains a no-op (P2P is reachable via the `.cfg`
  stencil path). The opt-in per-QP ceiling (`PerQpCeilingGbps` ≈ 49 G) for
  **ib_write_bw** sustained-throughput parity is **DONE**:
  `--perQpCeilingGbps=49` mounts `DcqcnRateMachine`'s true per-flow pacer;
  [`configs/calibration/roce_p2p_bw_sweep.py`](../configs/calibration/roce_p2p_bw_sweep.py)
  → converged **MAPE 3.20 %** for sizes ≥64 MB (64 MB 50.74 G +3.5 %, 256 MB
  47.60 G −2.9 % ≈ 49 G per-QP), correcting the 7.1× no-ceiling overestimate
  (256 MB 348.3 G → 47.6 G). Transition zone <64 MB over-estimates (1–7
  8 MB-coalesced chunks un-paced; the blog's sub-ceiling small-size regime is
  msg-issue-limited, not modeled — the same structural gap as `ib_write_lat`).

### Applicability / limits of this data

- **Usable for:** raw-RDMA scalar constants (per-QP ceiling, wire efficiency,
  per-message doorbell latency, saturation knee).
- **NOT usable for:** collective simTime fidelity (raw P2P ≠ AR/AG/A2A);
  multi-node/multi-hop scaling (2-node same-switch only); DCQCN/PFC/ECN
  (no congestion data — the blog itself flags this, pointing to the
  SIGCOMM 2015 DCQCN paper).

## Plain Ethernet (scale-out) — lossy, ns-3-anchored lower bound

The plain (non-RoCE) Ethernet So fabric is the **lossy counterpart** to RoCEv2.
Both ride the same 400GbE L2 wire; the difference is the congestion contract:

- **RoCEv2** is *lossless*: ECN marks the SoHeader on VOQ pressure, DCQCN
  backs the sender off, PFC would pause — drops are *avoided*, never
  recovered by retransmission.
- **Plain Ethernet** is *lossy*: the `EthernetSwitch` **tail-drops** at
  `VoqDropThreshold` and **emits a NACK back to the source**
  (`NvSwitch::EmitNackBack`, mirroring the BER-NACK path); the sender
  retransmits via the LLR/SACK path (`FabricEndpoint::ProcessNackPacket` →
  `HandleRetryRequest`/`HandleSackRequest`), with `LlrManager::OnRetryTimeout`
  as the backstop if the NACK itself is lost. `--llrRetryLimit` bounds the
  retransmit loop; exhausting it fires the permanent-loss fallback (faithful
  severe-congestion lossy-Ethernet degradation — the thing RoCE's PFC exists
  to prevent).

### ns-3 model (`EthernetProtocolModel`)

- `WireEfficiency = 1.0` (locked): plain L2 data carries **no RoCE
  IP/UDP/IB headers**, and ns-3 does not model Ethernet preamble/IFG framing,
  so the wire rate is the data rate (vs RoCE's 0.95). This is why the
  surrogate uses `wire_efficiency=1.0`, not 0.95.
- `GetProtocolId() = 0` (NONE): the non-bulk `SendDataAutoProtocol` path
  stamps `SetProtocol(protocolId)` unconditionally, so a non-zero id would
  misroute Ethernet DATA into the NCCL-LL receiver.
- Store-and-forward NIC driver: `StartupDelayNs=1500`, `PerStepLatencyNs=800`,
  `ChunkSize=9KiB` (jumbo MTU, smaller than RoCE's bulk path).
- **`VoqDropThreshold < VoqDepth` invariant**: the lossy-NACK path must fire
  before the base `NvSwitch` hard-cap drop (else the loss is silent). The sim
  wires `VoqDepth=1024` with `VoqDropThreshold=8` (default) for the ETH path.
- **LLR is required** (a fabric precondition, like RoCE's ECN/PFC infra):
  `--interNodeFabricType=ETH` forces `llrEnabled=1` on **both** the endpoint
  sender (so `StorePacket` populates the retry buffer and `ProcessNackPacket`
  can retransmit) **and** the So switch (so `EmitNackBack` does not early-return
  on `!m_llrEnabled`). Without switch-side LLR the drop is silent and the
  transfer stalls to the 3600s `Simulator::Stop` ceiling.
- `--ber=0 --packetLossRate=0` on the ns-3 command line (see the run examples
  in the `.cfg` headers) so the **only** loss source is VOQ tail-drop
  (deterministic), isolating the lossy-retransmit path from the BER model.
- **Config-primary So fabric selection.** The inter-node fabric axes —
  `interNodeFabricType`, `interNodeTopology`, `interNodeBandwidthGbps`,
  `interNodeLatencyNs` — are sourced from the `.cfg`'s `[stack]` section (or the
  `.profile`) into the same CLI variables the inline path consumes, so ONE config
  adapts across protocols: `interNodeFabricType = ETH` selects the lossy Ethernet
  So fabric, `RoCE` the lossless variant, with no `--interNode*` CLI flag. Keys
  absent fall back to the CLI default (backward-compatible). `numNodes`/
  `gpusPerNode`/`dataSize` stay on the CLI (scale axes). Demo:
  `configs/protocol_configs/eth-hierarchical.cfg` (a hierarchical allreduce over
  ETH, fully config-driven); bit-identical to the inline CLI path — gated by
  `test/parity/test_config_vs_inline_eth.py`.

### Surrogate (`make_h200_ethernet_theory`)

The surrogate is the **uncongested mean** — the lossy drop/NACK/retransmit
behavior is **ns-3-only** (the same split as RoCE: the surrogate gives the
uncongested mean while DCQCN/PFC/per-QP-ceiling live in ns-3). The profile is
spec-derived (store-and-forward `link_latency_us=3.0` / `t_min_switch_us=3.0`
vs RoCE's cut-through 1.0/1.5; `ring_bw=160000` shared with RoCE; `wire_eff=1.0`).
It is a **lower bound anchored to ns-3**, not a tight back-fit (no perftest/blog
ground truth for plain Ethernet, unlike RoCE's `ib_write_lat`). Plugged as the
So profile of `make_h200_hierarchical_theory` via the `so_factory` seam, the
measured surrogate/ns-3 ratio is ~0.2 — **identical to the RoCE hierarchical
anchor's ~0.2**, because the So stage is a small fraction of the uncongested
hierarchical and the store-and-forward latency is the only So difference (small
at large sizes). The parity test `test/parity/test_surrogate_vs_ns3_ethernet.py`
asserts the lower bound (surrogate ≤ ns-3) with a sanity floor, mirroring the
RoCE hierarchical anchor.

### Config + run

See `configs/protocol_configs/p2p-eth.cfg` (single P2P) and
`p2p-eth-incast.cfg` (2-flow incast that exercises the full NACK→LLR-retransmit
→delivery loop; `--voqDropThreshold=2 --llrRetryLimit=64` gives 87 drops → 285
retransmits → 0 permanent losses, completing in 68.7 µs).

## Full-mesh AllReduce/AllGather — ns-3-calibrated

Unlike ring (back-fit to one H200 HW point) and NVLS (an uncalibrated lower
bound), the full-mesh algorithm is **calibrated directly to ns-3 as ground
truth**. ns-3's `FullMeshAllReduce` / `FullMeshAllGather` injectors run on
`--topology=fullmesh` — every GPU-pair gets a dedicated `--numLanes=18` NVLink
bundle, no switch — and the surrogate's single scalar `fullmesh_bw` is the
least-squares back-fit to that sweep.

### Model

Each phase ships the chunk `D/N` to all `N-1` peers **concurrently** on
dedicated per-pair bundles, so a phase's time is `(D/N)/bundle_bw` — **no
N-fold serialization** (the structural difference from NVLS, which funnels `N`
chunks through one shared switch port). AllReduce = 2 phases (reduce-scatter +
all-gather, barrier-gated); AllGather = 1 phase.

```
T_AR = startup + 2·(lat + (D/N)/fullmesh_bw)
T_AG = startup + 1·(lat + (D/N)/fullmesh_bw)
```

### Sweep + fit

[`configs/calibration/fullmesh_ns3_sweep.py`](../configs/calibration/fullmesh_ns3_sweep.py)
sweeps `N ∈ {4,8,16}` × `{allreduce, allgather}` × 1 MiB–256 MiB (54 points,
[`fullmesh_sweep.csv`](../configs/calibration/fullmesh_sweep.csv)), parses
`simTimeUs`, then back-fits the single scalar on the SIMPLE-regime points
(chunk `D/N ≥ 2 MiB` ⇒ wire-eff 1.0, `startupSIMPLE=15000` ns == surrogate
`startup_us=15.0`):

```
fullmesh_bw = Σ(Vᵢ²)/Σ(Rᵢ·Vᵢ),   V = steps·(D/N),  R = T_ns3 − S − steps·lat
```

| Constant | Value | Anchor |
|----------|-------|--------|
| `fullmesh_bw` | 449890 B/µs | ns-3 sweep least-squares fit |
| `eta_fullmesh` | 0.9998 | `fullmesh_bw / (18 × 25000)` = 99.98% of the dedicated-bundle physics |
| MAPE | 0.10% | 30 SIMPLE-regime fit points |

The fit lands within 0.02% of the physics bundle `18 × 25000 = 450000` B/µs,
confirming the dedicated-bundle model: every GPU-pair runs at the full NVLink
aggregate with no contention. Result in
[`fullmesh_calib.json`](../configs/calibration/fullmesh_calib.json).

### Scope + limits

- **SIMPLE regime only** (chunk `D/N ≥ 2 MiB`). Sub-SIMPLE sizes use the
  LL/LL128 startup tiers and 50/93.75% wire efficiencies the scalar fit does
  not capture (the same scope restriction as the ring preset); the parity test
  covers only SIMPLE-regime sizes.
- **Calibrated back-fit, not a bound**: `fullmesh_bw` is a least-squares fit, so
  the surrogate straddles ns-3 rather than bounding it from below (above at 8 of
  30 fit points, below at 22; worst deviation 0.31%) — hence the parity test's
  symmetric band, not a one-sided floor. The model omits per-packet credit
  flow, VOQ arbitration and the inter-phase barrier that ns-3 models; the
  residual is ~0.1% in either direction (e.g. 64 MiB AllGather: surrogate
  34.05 µs vs ns-3 34.00 µs — slightly above), not a systematic offset.
- **AllReduce vs AllGather**: `T_AR ≈ 2·T_AG − startup` (two serial phases).
  Full-mesh AllReduce is ~8× faster than ring at 256 MiB/8 (165 µs vs 1326 µs)
  because the `N-1` peer sends are concurrent, not `2(N-1)` sequential hops.

### Verification

- 7 unit tests (`surrogate/test/test_theory.py`): step count, per-step volume,
  credit pressure, ns-3 reproduction at 256 MiB, the `T_AR ≈ 2·T_AG` relation,
  faster-than-ring.
- Parity test
  ([`test/parity/test_surrogate_vs_ns3_fullmesh.py`](../test/parity/test_surrogate_vs_ns3_fullmesh.py)):
  surrogate within 0.3% of ns-3 across N=8 AR+AG at {16,64,256} MiB (the
  calibrated [0.95, 1.05] band).


## Loss recovery under injected packet loss — SRNIC Fig. 11 (measured) vs ns-3

External measured anchor for the **loss-recovery mode** of the RoCE
scale-out fabric: SRNIC, *"SRNIC: A Scalable Architecture for RDMA NICs"*,
NSDI 2023, Figure 11
(<https://www.cse.ust.hk/~kaichen/papers/srnic-nsdi23.pdf>). Testbed: two
hosts on 100G links, perftest with 4 KB messages, congestion control
**disabled**, an FPGA in the path injecting uniform random packet drops.
Two measured curves: ConnectX-6 (go-back-N recovery — "drops rapidly when
the packet loss rate exceeds 0.1%, and drops to 25 Gbps when the loss rate
exceeds 1%") and the SRNIC prototype (bitmap selective repeat — "75 Gbps
goodput under 1% packet loss rate"). Digitized points with provenance:
[`configs/calibration/srnic_fig11_cx6_reference.csv`](../configs/calibration/srnic_fig11_cx6_reference.csv).

This maps directly onto the fabric's two LLR modes — **the ns-3 sweep changes
only `llrMode`** between arms:

| ns-3 arm | cfg | measured analogue |
|---|---|---|
| `llrMode=gobackn` | `configs/protocol_configs/p2p-roce-lossy.cfg` | ConnectX-6 |
| `llrMode=sack` | `configs/protocol_configs/p2p-roce-lossy-sack.cfg` | SRNIC |

Sweep driver: [`configs/calibration/eth_gbn_loss_sweep.py`](../configs/calibration/eth_gbn_loss_sweep.py)
(RoCE tor 100G, `--gpusPerNode=2` — required, see the cfg header —
`--bulkChunkSize=4096` for perftest 4 KB granularity, 16 MiB transfer,
`--packetLossRate` swept, 3 seeds). Results in
`configs/calibration/eth_gbn_loss_sweep.json`. The comparison is
**normalized per curve** (`G(p)/G(0)`): the ns-3 absolute plateau
(53.2 Gbps: tor 2-link serialization + 4 KB chunk headers) intentionally
differs from the testbed's single-link 90–97 Gbps.

### Result (normalized goodput, 3-seed mean)

| loss rate | ns-3 GBN | CX-6 measured | ns-3 SACK | SRNIC measured |
|---|---|---|---|---|
| ≤1e-4 | 0.99–1.00 | 1.000 | 1.000 | 1.000 |
| 1e-3 | 0.629 | 0.933 | 0.939 | 0.990 |
| 2e-3 | 0.526 | 0.889 | 0.937 | 0.979 |
| 5e-3 | 0.515 | 0.744 | 0.934 | 0.907 |
| 1e-2 | **0.510** | **0.467** | 0.929 | 0.773 |
| 2e-2 | 0.501 | 0.278 | 0.826 | 0.619 |

What reproduces (the point of the experiment):

- **The recovery-mode spread**: selective repeat degrades gently while GBN
  collapses — the qualitative core of Fig. 11. At 1% loss the ns-3 GBN arm
  (0.510) sits within 5 points of the measured CX-6 collapse (0.467).
- **SACK retransmit economy**: ns-3 SACK retransmits ≈ the actual drop count
  (tens of packets), GBN retransmits 3–5 orders of magnitude more
  (~10⁵ at 1–2% loss) for the same delivered bytes.

What deviates, and why (structural, documented rather than tuned away):

- **GBN collapses earlier than CX-6** (0.63 vs 0.93 at 0.1% loss). The bulk
  sender assigns every chunk sequence number in one synchronous dispatch, so
  a NACK re-blankets from the lost seq to the end of the *message* — the GBN
  window is the whole remaining transfer, not the NIC's BDP/tx-depth window
  that bounds CX-6's per-drop waste to ~RTT×C.
- **GBN saturates at ~0.50 instead of following CX-6 down to 0.28**. For the
  same reason the waste is bounded by ~one extra message time (the pipe stays
  100% busy re-blanketing), while the real per-drop waste keeps growing with
  drop count — the two asymptotics cross near 1% loss.
- **SACK is more optimistic than SRNIC** (0.93 vs 0.77 at 1%): retransmitting
  only lost 4 KB chunks over an otherwise idle reverse path is cheaper than a
  real RNIC's bitmap/ACK machinery under load.
- GBN `permanentLosses>0` at ≥1% loss are retry-limit exhaustions of packets
  already delivered by an overlapping blanket (status stays `complete`).

### Model fix exposed by this experiment (`--llrTimeoutRetransmit`)

The SACK arm initially **deadlocked** under random loss: NACK emission at
receiver drop time was the *only* recovery trigger, and `--packetLossRate`
applies to control packets too (only BER/FEC has a control bypass) — a lost
NACK stalled the transfer forever. Root cause: `LlrManager::OnRetryTimeout`
only invoked a callback that *unit tests* registered; production had a no-op
retry timeout. New opt-in `--llrTimeoutRetransmit=1` (default 0 = legacy
no-op, parity-pinned) wires the endpoint to the timeout: on fire it
re-issues the retransmission via the mode's own path (SACK single-seq /
GBN blanket), re-arming timers and honoring `--llrRetryLimit`.

Timeout sizing caveat (same whole-message-window bound as above): timers arm
at StorePacket time and the dispatch is synchronous, so `--llrRetryTimeout`
must exceed the worst-case delivery+RETRY_ACK delay of the *whole transfer*
(~2.6 ms for 16 MiB here; the sweep uses 3 ms for SACK, and 20 ms on the GBN
arm purely as deadlock insurance that never fires). A lost-NACK event
therefore costs ~one transfer time to recover. Both lossy cfgs also raise
`vcCredits` to effectively-infinite: CREDIT control packets are not
LLR-protected, and the reference testbed has no link-level credit flow
control, so a randomly dropped CREDIT under the default 64-credit budget
would be an artifact stall.

Layer caveat (same as the perftest section above): ns-3 models a bare-metal
L2 fabric endpoint, not a verbs NIC — the comparison validates the
*recovery-mode degradation shape*, not absolute goodput.
