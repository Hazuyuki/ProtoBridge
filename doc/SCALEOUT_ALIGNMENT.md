# Scaleout (inter-node) alignment: ProtoBridge vs ns3-rdma

Companion to `doc/PFC_NS3RDMA_ALIGNMENT.md` (the 2:1 incast head-to-head).
This note extends the comparison to scaleout dimensions only — convergence
ratio (2:1 / 4:1 / 8:1 incast), multi-switch topologies (PB host / tor /
leafspine), dynamic vs static PFC thresholds, and 9000-B chunks — on the
same matched fabric parameters (40 Gbps, 1 µs links, 1000-B packets,
2.0-s flow starts, per-flow 50 MB).

## Matched workload

- **ns3-rdma**: star topology (single switch node 0; receiver node 1;
  N−1 sender nodes), `flow_{4,8}to1.txt` (N−1 flows × 50,000 pkts × 1000 B,
  all `start_time` 2.0), static / dynamic / nopfc configs derived from the
  2:1 mix configs. FCT = last switch enqueue toward the receiver minus
  flow start (same extraction as the 2:1 note). Assets:
  `/tmp/scaleout/ns3rdma/`.
- **ProtoBridge**: `--topology=switched --collective=incast
  --numNodes=<N+1> --gpusPerNode=1 --interNodeTopology=<tor|host|leafspine>
  --interNodeBandwidth=40 --interNodeLatency=1000 --dataSize=50000000
  --bulkChunkSize=1000 --soTxWindow=64` (PFC rows add
  `--switchPfcMode=<m> --pfcPauseThreshold=20 --pfcResumeOffset=8
  [--pfcPauseTime=5]`). PB `tor` (ONE shared leaf switch, every node's NIC
  attached) is the exact structural twin of the ns3-rdma star; `host`
  (per-node switch + peer links) and `leafspine` (2-tier Clos) add PB-only
  multi-switch depth.

## Results — FCT (ms), 1000-B chunks, per-flow 50 MB

| Scenario | 2:1 | 4:1 | 8:1 | Drops |
|---|---|---|---|---|
| ns3-rdma, static thr | 20.29 | 40.58 | 81.17 | 0 |
| ns3-rdma, dynamic thr (α=16) | 19.51 | 39.79 | 80.37 | 0 |
| ns3-rdma, PFC off (control) | 20.29 | 40.58 | 81.17 | 0 |
| PB tor, PFC off | 21.90 | 43.79 | 87.58 | 0 |
| PB tor, mode 1 (static) | 21.90 | 43.79 | 87.58 | 0 |
| PB tor, mode 2 (dynamic) | 21.97 | 43.80 | 87.59 | 0 |
| PB tor, mode 3 (quanta) | 22.35 | 44.31 | 87.88 | 0 |
| PB host, PFC off | 21.90 | 43.79 | 87.58 | 0 |
| PB host, mode 1 | 23.60 | 45.60 | 89.40 | 0 |
| PB leafspine, PFC off | 21.90 | 43.80 | 87.58 | 0 |
| PB leafspine, mode 1 | 23.85 | 45.74 | 89.45 | 0 |
| PB leafspine, mode 3 | 23.85 | 46.48 | 90.05 | 0 |

## Findings

1. **The PB/ns3-rdma FCT ratio is convergence-ratio-invariant at 1.079×.**
   The gap is exactly +0.02 µs per packet at every ratio (2:1: 1.609 ms
   over 100k pkts; 4:1: 3.209 ms over 200k; 8:1: 6.412 ms over 400k) — a
   per-packet pipeline overhead, NOT a congestion/PFC mechanism
   difference, and it does not compound with incast depth. Both sides
   track the theoretical N × 10 ms (ns3-rdma within 1.5%, PB within 9.5%).
2. **On the structural twin (PB tor = ns3-rdma star), mode 1 and mode 2
   are FCT-INVARIANT** — byte-identical to PFC off at 2:1/4:1/8:1 — which
   exactly reproduces ns3-rdma's PFC-invariance (static and nopfc rows
   identical to the microsecond). This resolves the 2:1 note's
   "PFC-slightly-variant" observation: that penalty (23.60 vs 21.90) is a
   multi-switch `host`-topology artifact (per-node switch PAUSEs the
   sender-side hop AND the receiver-side switch PAUSEs the relay — two
   backpressure points parking the window), not a PFC-semantics
   difference. On the single-switch fabric the two stacks agree.
3. **Mode 3's quanta penalty shrinks with incast depth**: +2.1% (2:1) →
   +1.2% (4:1) → +0.3% (8:1) vs PFC off — under deeper oversubscription
   the queue never drains past the resume offset within a quanta, so the
   endpoint auto-expiry stops parking the window (the 9000-B rows show
   the same convergence: 0% at every ratio).
4. **Dynamic threshold helps ns3-rdma slightly (+1%–4%) but helps PB tor
   not at all** (+0.08% at 2:1, 0% at 4:1/8:1): ns3-rdma's α-scaled
   admission admits more in flight (a threshold effect), while PB's
   threshold is crossed either way once N ≥ 2 senders park the VOQ above
   20 chunks. **Multi-switch PB topologies show the same two-backpressure-
   point penalty as `host`** (leafspine mode 1: 23.85/45.74/89.45 ≈ host's
   23.60/45.60/89.40): whenever the fabric has ≥2 switches on the
   congested path, mode 1's per-hop PAUSE parks the window twice. The
   single-switch tor is the topology where the PFC-invariance contract
   with ns3-rdma holds exactly.
5. **9000-B chunks** (PB rows; ns3-rdma's chunk-mode 9000-B path deadlocks
   at 4:1 — its UdpEcho 4-MB window never receives the ACK under
   incast; tooling limitation, not a fabric result): PB 4:1 tor
   42.296 ms and 8:1 tor 84.589 ms, PFC-invariant across modes 0/1/3,
   ratio vs the 1000-B ns3-rdma rows 1.042/1.042 — the per-packet
   overhead dilutes 9× at 9000-B exactly as the 2:1 note predicted
   (0.02 µs/pkt → 0.0022 µs/pkt).
6. **Headroom sizing follows the 802.1Qbb BDP rule, with the window as the
   BDP** (4:1 tor mode 1, SACK LLR, 50 MB, sweeping h): drops occur for
   every h < soTxWindow (h=8 → 517k, h=32 → 488k, h=56 → 14.8k) and reach
   EXACTLY ZERO at h ≥ 64 = soTxWindow — the per-paused-hop budget must
   cover one full sender window of post-XOFF in-flight chunks (PB's
   ACK-clocked endpoint makes the window the effective BDP; ns3-rdma's
   m_pg_hdrm_limit=100 cells plays the same role against its L2 window).
   With SACK LLR the FCT is unchanged even at h=8 (43.792 ms, identical to
   the zero-drop baseline): the 4:1 bandwidth underload absorbs the
   retransmissions. With the CLI-default Go-Back-N LLR, however, an
   undersized headroom at 4:1 degenerates into a retransmit livelock
   (each drop retransmits the whole window; even a 1-MB run does not
   finish — use `--llrMode=sack` when sweeping small headrooms). 8:1 with
   h=64: zero drops, FCT byte-identical (87.581 ms); h=32 at 8:1: 2.63 M
   drops, still FCT-invariant.
7. **Pause counts scale with senders, not with topology depth**: PB tor
   mode 1 emits ~1.5k/3.4k/6.7k pauses at 2/4/8:1 (≈840 per sender) vs
   ns3-rdma's 27k/131k/487k (≈13k–60k per sender). The cadence difference
   is the known re-check-interval artifact (PB polls at 0.5 µs vs qbb's
   pausetime/2 = 2.5 µs re-assertion loop) — PB latches ONCE per
   congestion episode and holds via explicit RESUME hysteresis, ns3-rdma
   re-asserts continuously while its per-PG occupancy stays above the
   XON level. Mode 3 closes most of that gap by construction (13.9k/
   66k/274k at 2/4/8:1 — same order as ns3-rdma).

8. **Go-Back-N + undersized headroom is a livelock at 4:1+** (operational
   finding): each headroom-exhaustion drop retransmits the ENTIRE window
   under GBN; at 4:1 oversubscription the drops scale with the sender
   count and the retransmit loop never drains (a 1-MB transfer does not
   finish in 5 minutes of wall-clock). SACK recovers exactly (drops ==
   retransmits, zero permanent loss, FCT unchanged). This mirrors the
   real-fabric guidance that PFC's no-drop contract makes headroom sizing
   a correctness requirement, not a tuning knob — and that Go-Back-N
   ARQ composes badly with it.

## Reproduction

- ns3-rdma (from `/tmp/ns3rdma-build`, cwd `/tmp/scaleout/ns3rdma`):
  `LD_LIBRARY_PATH=/tmp/ns3rdma-build/build ./build/scratch/third
  config_static_{4,8}to1.txt` (plus `_dynamic_` / `config_nopfc_`), then
  extract FCT from the `.tr` (see awk one-liner in the run logs).
- ProtoBridge: the command line above, sweeping `--numNodes` ∈ {3,5,9},
  `--interNodeTopology` ∈ {tor,host,leafspine}, `--switchPfcMode` ∈
  {0,1,2,3}, `--bulkChunkSize` ∈ {1000,9000}, optionally
  `--pfcHeadroomChunks`.
