# Config Guide — the `.cfg` / `.profile` primary input

ProtoBridge's primary user input is a **protocol config** (`.cfg`) that names a
calibrated collective + algorithm and the fabric it runs on, plus a **protocol
profile** (`.profile`) that bundles the protocol / FEC / credit / BER / hardware
values. A one-line run reproduces a measured H200 latency with no per-run CLI
flags:

```bash
./ns3 run "gpu-cluster-sim --protocolConfig=\
configs/protocol_configs/h200-ring-allreduce.cfg --numGpus=8 --dataSize=1048576"
# 8-GPU 1 MiB ring allreduce -> simTimeUs=38.3  (H200 measurement 37.24 us)
```

`numGpus` and `dataSize` stay on the CLI (they are the experiment knobs); the
`.cfg` carries the protocol + operation + hardware.

## 1. The two layers

A `.cfg` has two sections:

| Section | Layer | What it carries |
|---------|-------|-----------------|
| `[stack]` | PEX (packet execution) | a `profile = …` reference to a `.profile` (protocol model, FEC, credits, BER, LLR, **fabric hardware**) |
| `[op]` | OTP (operation → packet) | either `collective =` + `algorithm =` (delegate to a calibrated injector) **or** a transfer stencil |

```ini
[stack]
profile = configs/protocol_profiles/h200-ll128.profile

[op]
collective = allreduce   # delegate, OR leave empty and declare a stencil
algorithm  = ring
topology   = ring         # optional: overrides the CLI fabric
```

## 2. The two `[op]` forms

### 2a. Collective + algorithm delegation (primary, calibrated, bit-identical)

Declaring `collective = <c>` + `algorithm = <a>` delegates to the calibrated
inline injector. The simulator sources the profile's fabric-hardware + protocol
keys into the same local CLI variables the inline path consumes (the PEX-bundle
keys go to the bundle object via `Build()`, not CLI vars), then runs the
injector **verbatim**. Because every sourced key maps 1:1 to an inline CLI
flag, the `.cfg` run is **bit-identical** to the inline path with the
profile's values as flags (same code, same values, same `RngRun`).

| `collective` | `algorithm` | `topology` | shipped `.cfg` |
|--------------|------------|------------|-----------------|
| allreduce | ring | `ring` | `h200-ring-allreduce.cfg` |
| allreduce | tree | `switched` | `h200-tree-allreduce.cfg` |
| allreduce | sharp | `switched` | — |
| allreduce | hierarchical | `switched` | — |
| allgather | nvls | `switched` | `h200-nvls-allgather.cfg` |

> `topology` is part of the calibration: ring allreduce on `switched` does
> **not** reproduce the measurement (it runs ~6× slower). Tree and NVLS use
> the NVSwitch (`switched`) fabric.

### 2b. Custom transfer stencil (define a new protocol)

Leave `collective`/`algorithm` unset and declare `param` / `replicate` /
`transfer.*` lines. A compiler (`ProtocolConfig::Compile`) expands the stencil
through the vendor `ProtocolModel` into a transaction graph run by the generic
runner. See [ARCHITECTURE.md](ARCHITECTURE.md#config-driven-protocol-stack-cfg)
for the full stencil schema; `h200-request-response.cfg` is a worked two-leg
ping-pong example.

## 3. The `.profile` keys

`h200-ll128.profile` carries the H200 PEX bundle + fabric-hardware keys,
falling into two groups. The fabric-hardware + protocol keys are **sourced**
into the inline path's local CLI vars when an `[op]` declares
`collective=`+`algorithm=`; the PEX-bundle keys below are **not** — they are
applied to the bundle/endpoint object by `Build()` (`SetAttributeFailSafe`),
since the collective/algorithm path keeps the inline 1-VC fabric model:

**PEX bundle** (drive the protocol stack — applied to the bundle / endpoint
object, not sourced into CLI vars for the collective/algorithm path; the
fabric-hardware + protocol groups are the sourced ones — see below):
`PerGpuStartupDelayNs` likewise is applied to the bundle's protocol model via
`SetAttributeFailSafe`, not as a CLI var:

```
vcCount            = 4
vcCredits          = 64
flowControl        = credit
```

**Credit is a scale-up (Su) mechanism only.** `vcCredits`/`flowControl` gate
the intra-node (NVLink) fabric; scale-out (So) traffic bypasses credit on both
send (`SendBulkWireTransferSize`'s `IsLocalRank(destRank)` guard) and the
credit-return path (`OnPacketDelivered`). So flow control is the lossless/lossy
fabric mechanism instead: ECN + DCQCN + per-QP ceiling
(`--ecnThreshold`/`--dcqcn`/`--perQpCeilingGbps`) for RoCE, VOQ tail-drop +
LLR retransmit (`--voqDropThreshold`/`--llrMode`/`--llrRetryLimit`) for plain
Ethernet — matching real hardware (RDMA uses ECN/PFC, not per-packet credits;
NVLink is credit-based). Tuning `vcCredits` therefore only affects Su latency.

**Lossy RoCE (no-PFC) is an explicit opt-in:** pass `--voqDropThreshold=N`
(N>0) on a RoCE run to arm VOQ tail-drop + NACK→LLR retransmit beside ECN
marking (set `--ecnThreshold < N` for mark-before-drop; LLR is
force-enabled with the opt-in). Without the flag the RoCE fabric is
lossless — the switch attribute default is 0 and the scratch CLI default 8
applies only to plain Ethernet. In that lossy regime the DCQCN alpha knobs
move completion time (GInit/GDecay up to ~8 % swing on a drop-heavy
allreduce); see `examples/gpu-cluster/collective_topology_dcqcn/README.md`.

**RTTCC delay-based congestion control (`--rttcc`).** DCQCN needs the RoCE
ECN/CNP loop, which the plain lossy Ethernet fabric does not have (and the
DCQCN So gate keys on `WireEfficiency<1.0`, which `EthernetProtocolModel`
locks at 1.0) — so ETH gets a delay-based option instead (NVIDIA RTT-CC /
Swift-style). With `--rttcc=1`, each So endpoint mounts an `RttccRateMachine`:
the sender emits at most one `RTT_PROBE` control packet per
`--rttccProbeIntervalNs` (default 5000) per flow alongside the data, the
receiver reflects it as an `RTT_RESPONSE`, and each echo becomes an RTT
sample: at/below `--rttccTargetRttNs` (default 10000) the flow rate ramps by
`AdditiveIncreaseGbps` (attribute, default 10) toward the line rate; above
target it decays by `1 − Beta·(rtt−target)/rtt` (Beta default 0.8, floor
`MinRateGbps`). The rate is enforced as a per-flow pacer at the same egress
chokepoint as DCQCN and composes with it (`max` of the two delays). Probes
are control packets: never tail-dropped, BER-bypassed, LLR-skipped — but
still queued through switch VOQs, so the sampled RTT sees real congestion.
Default off is byte-identical; RESULT reports `rttccProbesSent` /
`rttccRttSamples` / `rttccMinRttNs` / `rttccMaxRttNs` / `rttccMinRateGbps`.
Idealizations: a probe always returns a sample (control path is loss-free at
BER=0), and reflection is unconditional (no receiver-side machine). Works on
any So fabric type but only shapes inter-node flows (`numNodes>1`).

**Real-NCCL ring allreduce (`--algorithm=ncclring`, `--ringChannels`).**
Opt-in alternative to the default multi-node `hierarchical` allreduce
schedule, modeled on what NCCL's ring algorithm actually does: C parallel
channels (`--ringChannels`, `0 = auto` → gpusPerNode), each one global
pipelined ring over all N ranks, rail-rotated so channel c enters each node
at local GPU c and exits at local GPU (c+g−1)%g — one NIC in / one NIC out
per ring per node, all rails busy concurrently. Each channel moves S/C
bytes in 2(N−1) same-direction chunk steps (chunk = S/(C·N)), advanced
event-driven with no inter-phase barriers, so intra- and inter-node hops
overlap (max-of-bottleneck instead of the hierarchical sum-of-phases).
Wins ≥2× at large sizes on switched topologies (741 vs 362 Gbps busbw at
256 MiB, 16 n × 8 g RoCE 400G); loses below ~16 MiB where the 2(N−1)-step
latency dominates — the same regime where real NCCL switches to tree.
Honors the shared per-step knobs (`--perStepSwOverheadNs`,
`--swOverheadPerByteNs`, `--computeBase*Ns`, `--llThreshold` /
`--ll128Threshold`, `--startupDelayNs`). AllReduce only; gate
`test/parity/test_ncclring.py`.

**So topologies (`--interNodeTopology` + shape knobs).** Eight inter-node
topologies in three wiring shapes: one-NIC-per-GPU switched (`host`, `tor`,
`leafspine`), direct rdmaRouteMap + intra-node PXN proxy (`fullmesh`, `rail`),
and GPU-0 gateway multi-hop (`ring`, `torus2d`, `dragonfly` — these require
`intraNodeTopology=switched` and funnel all cross-node traffic through each
node's GPU 0). Shape knobs, all `0 = auto`: `--interNodeSpines` (leafspine
spine count; auto = gpusPerNode, non-blocking), `--interNodeTorusWidth`
(torus2d grid width W with height = numNodes/W; auto = largest divisor
≤ √numNodes), `--interNodeDragonflyGroupSize` (nodes per dragonfly group;
auto = divisor nearest √numNodes). The lossy-ETH VOQ tail-drop + NACK path
needs a switch in the inter-node path, i.e. `host`/`tor`/`rail`/`leafspine`;
the switchless/gateway topologies warn and run the fabric lossless.
`test/parity/test_so_topologies.py` pins a completion smoke for all eight.

**WFQ contention model (`--contention*Weight`, `--bgP2p*`).** Each endpoint
carries a `ContentionModel` that does weighted-fair-queuing over three traffic
classes — COLLECTIVE / MEMORY / P2P (`--contentionCollectiveWeight` /
`--contentionMemoryWeight` / `--contentionP2pWeight`, defaults 0.7/0.2/0.1).
The WFQ penalty fires **only when more than one class is concurrently active**
on the same endpoint's injection side; single-class traffic (a pure collective)
is byte-identical to the unmounted baseline. As of this release multi-node
(scale-out) endpoints are wired too — previously only single-node (Su) was.

**Modeling-layer difference.** The two regimes mount the model at *different*
layers, so their magnitudes are not expected to match: single-node (Su) shares
ONE `ContentionModel` clone across all the node's endpoints (the helper's
`AttachContentionModelToEndpoints` — backlog aggregates node-wide, a
shared-egress view); multi-node (So) mounts an **independent** per-endpoint
instance (backlog local to that endpoint's injection side). Both use the
intra-node NVLink rate as the WFQ bandwidth, also for So-bound packets;
cross-node port contention is additionally covered by the So switch VOQ
arbiter. Neither layer has a hardware calibration behind it.

**Two mechanisms slow a collective under a background flow**, and they are
not the same thing: (a) **link sharing** — the background packets FIFO-serialize
on the same NetDevice / So switch VOQ as the collective's; (b) the **WFQ
penalty** above, an extra send-side delay when more than one class is active.
Link sharing dominates every smoke below; the WFQ penalty can be entirely off
the critical path. To isolate it, set all three weights to 1.0: that makes the
penalty exactly 0 while leaving the model mounted, so any difference from the
default-weight run is the WFQ contribution.

To exercise contention, inject a time-offset background P2P flow with
`--bgP2pEnabled=1 --bgP2pSrc=R --bgP2pDst=R' --bgP2pSizeBytes=B
--bgP2pStartTimeNs=T`; it is an unlimited self-driven `P2pTraffic` that shares
the fabric with the primary collective until the collective completes (which
stops the sim). `--bgP2pIntervalNs=0` means back-to-back = one packet
serialization time at the **intra-node** bandwidth, clamped to ≥1 ns (a
literal 0 ns would loop at a fixed sim-time and stall the sim). For a
cross-node flow that over-drives the inter-node link by the intra/inter rate
ratio (2× at 800G/400G) and the excess queues at the NIC; pass
`--bgP2pIntervalNs` explicitly to pace at the inter-node rate. The result block
reports `bgP2pBytesSent`/`bgP2pPacketsSent` — these count *injection*
(`SendP2p` calls), not delivery.

Reference numbers (`--RngRun=1`, 16 MiB allreduce, 1 MiB bg packets from
t=5 µs; all pinned by `test/parity/test_contention_interference.py`):

| regime | bg flow | default weights | weights=1.0 | baseline |
|--------|---------|-----------------|-------------|----------|
| 2-node RoCE hierarchical | rank 0→4 (cross-node) | 2776201 | 2776201 | 1248928 |
| 2-node RoCE hierarchical | rank 0→1 (intra-node) | 1330831 | 1315771 | 1248928 |
| single-node Su ring | rank 0→2 | 450982 | 468378 | 319658 |

The cross-node +122% is all link sharing (WFQ byte-identical; the bg flow,
paced at the 800G intra-node rate onto a 400G link, is limited by NIC link
sharing); the intra-node bg is what discriminates WFQ on the multi-node path.
The multi-node bg numbers were re-anchored by the HierarchicalAllReduce
PERMANENT_LOSS/P2P-progress fix (only `FabricPacketType::DATA` advances
collective phases): the pre-fix values (1353663/1311031) were corrupted by
phantom progress — the bg flow's P2P packets incremented the same phase
counters the collective uses, so runs "completed" early on missing collective
bytes; the fixed runs wait for real data and show the true (much larger)
interference cost. The single-node +41% is
link sharing with a −3.7% WFQ correction on the P2P class — WFQ reshuffles
who waits rather than adding latency monotonically. The regimes differ in
baseline length and link type (credit-gated NVLink vs an over-driven 400G
NIC), which is what separates their magnitudes, not the WFQ term.

**Uniform-random traffic mode (`--randTraffic`).** Replaces the collective
injector with a fixed-duration synthetic load: every rank self-schedules
fixed-interval messages of `--randMsgBytes` (default 64 KiB) to
uniform-random ranks on OTHER nodes, at `--randInjectionRate` × the
inter-node line rate per source (interval = msgBytes×8 / (rate × interNodeBW),
clamped ≥1 ns). The run stops at `--randWarmupNs + --randMeasureNs`
(defaults 50 µs + 200 µs) and only bytes offered/delivered inside the
measurement window are counted; per-source random initial phases avoid
lockstep and everything is `--RngRun`-seeded. On the RoCE fabric the mode
mounts `RocePayloadBuilder` and sends with protocol `RoceProtocol::SIMPLE`,
so every data packet carries a SoHeader — the full ECN → CNP → DCQCN chain
is live wherever an `EthernetSwitch` sits in the So path (switched
topologies; the gateway shapes ring/torus2d/dragonfly and switchless
fullmesh never mark). The result block reports `randMsgsSent` /
`randOfferedBytes` / `randDeliveredBytes` / `randOfferedGbps` /
`randDeliveredGbps`, plus the always-present `ecnMarkedPackets` /
`cnpPacketsSent` / `cnpPacketsReceived` counters. Requires `--numNodes>1`;
default off is parity-safe. Gate: `test/parity/test_random_traffic.py`;
sweep + figure: `examples/gpu-cluster/random_traffic_dcqcn/`.

**So payload-builder seam (DCQCN on inline collectives).** Inline collective
runs mount the NCCL payload builder, which never stamps a SoHeader — so the
switch ECN-mark and receiver CNP-echo guards (both key on
`RoceProtocol::SIMPLE` + SoHeader) would leave the whole ECN → CNP → DCQCN
chain dormant. `--dcqcn=1` on a RoCE fabric therefore mounts a dedicated So
`RocePayloadBuilder` via `FabricEndpoint::SetSoPayloadBuilder`: remote-rank
bulk sends are framed by it, with the wire protocol id overridden to the So
protocol model's own id, while local-rank (Su) traffic keeps the NCCL
builder's framing/timing untouched. The 10-byte SoHeader sits within the
wire-size budget, so chunk count and egress serialization are unchanged —
`--dcqcn=0` (default, seam unmounted) is byte-identical to before. Gate:
`test/parity/test_collective_dcqcn.py`; experiment (2 collectives × 8
topologies × 5 sizes at 16 n × 8 g): `examples/gpu-cluster/collective_topology_dcqcn/`.

**DCQCN rate-machine knobs.** The `DcqcnRateMachine` TypeId attributes are
CLI-exposed for parameter sweeps — `--dcqcnTFastNs` (fast-recovery timer,
default 50000), `--dcqcnFastCycles` (default 5), `--dcqcnTAiNs`
(additive-increase timer, default 100000), `--dcqcnRai` (default 0.01),
`--dcqcnGInit` (initial multiplicative backoff g, default 0.5),
`--dcqcnGDecay` (default 0.5), `--dcqcnAlphaFloor` (default 0.0). Each is
only `SetAttribute`-ed when given (0 = keep the default), so the default
path stays byte-identical. When `--dcqcn=1` the machine's `MaxRateBps`
(the alpha shaping-delay math's rate anchor) is back-filled from
`--interNodeBandwidth` — the TypeId default 50e9 B/s is only correct for
400G links. Parameter-sensitivity sweep (host ncclring, the one scenario
where the chain fires): `examples/gpu-cluster/collective_topology_dcqcn/`
(`run_dcqcn_param_sweep.py`, `plot_dcqcn_params.py`).

**Fabric hardware + protocol startup + FEC + BER** (sourced into the local CLI
variables — these are what make a `.cfg` reproduce a calibrated run without
per-run flags):

```
# fabric hardware
bandwidthGbps           = 170     # 8-GPU effective per-link (375 GB/s / 18 lanes)
latencyNs               = 400
numLanes                = 18
linksPerGpu             = 18      # TRUE H200 fabric: separate p2p GPU<->NVSwitch
                                 # NVLink4 lanes @ 170 Gbps, spray-routed (the
                                 # retired linksPerGpu=1 collapsed them into one pipe)
sprayChunkSize          = 131072
switchVoqDepth          = 10000
switchArbIntervalNs     = 100
# protocol startup (per-TypeId)
StartupDelayLL          = 15000
StartupDelayLL128       = 25000
StartupDelaySIMPLE      = 46000
StartupNVLS             = 23000
LlThreshold             = 8192
Ll128Threshold          = 2097152
# protocol model + link-level retry (sourced into CLI vars)
protocolModel            = ns3::NcclProtocolModel
forceProtocol            = 0          # 0=auto (LL<8KB, LL128 8KB..2MB, SIMPLE>2MB)
llrEnabled               = 0
llrMode                  = gobackn
# FEC (RS 544/514/15)
fecN                    = 544
fecK                    = 514
fecT                    = 15
fecEncodeLatencyNs      = 50
fecDecodeLatencyNs      = 80
# link BER
berIntraNodeElectrical  = 1e-15
berIntraRackElectrical  = 1e-13
berInterRackOptical     = 1e-9
```

## 4. The bit-identity guarantee

A `collective=`+`algorithm=` `.cfg` sources the profile's values into the local
variables the inline path consumes, then runs the same injector. The two paths
must produce identical `simTimeNs`. `test/parity/test_config_vs_inline_parity.py`
is the gate — it parses the profile, emits the equivalent inline flags, runs
both paths for `{ring, tree, nvls}` × sizes × GPU counts, and asserts exact
`simTimeNs` equality:

```bash
python3 -m pytest test/parity/test_config_vs_inline_parity.py -q
# or: python3 test/parity/test_config_vs_inline_parity.py
```

Verified cases (config `==` inline, `simTimeNs`) on the current
`linksPerGpu=18` fabric convention (all six re-measured; the pre-1fb92ba
`linksPerGpu=1` values 38308/1353580/33028/1163404/114794/80352 are
obsolete):

| op | size | GPUs | simTimeNs | measured |
|----|------|------|-----------|----------|
| ring | 1 MiB | 8 | 38782 | — |
| ring | 256 MiB | 8 | 1496204 | — |
| ring | 1 MiB | 4 | 33634 | — |
| ring | 256 MiB | 4 | 1284496 | — |
| tree | 1 MiB | 8 | 55148 | — |
| nvls | 1 MiB | 8 | 27576 | — |

If you change a sourced profile key, the C++ sourcing block
(`scratch/gpu-cluster-sim.cc`, after `configMode = true;`), or an injector,
re-run the parity test — a divergence is a behavior change, not a refactor.

## 5. Authoring

### A new hardware profile

Copy `h200-ll128.profile`, change the fabric-hardware + startup keys (using
the GPU data-sheet values: per-link NVLink rate, lane count, link latency,
per-TypeId startup delays), and reference it from a `.cfg`
`[stack] profile =`. The `bandwidthGbps` value is the effective per-link rate
for the target GPU count (H200 8-GPU: 375 GB/s aggregate ÷ 18 lanes ≈ 166,
rounded to the empirical sweep best 170).

### A new calibrated op

Ship a `.cfg` with `collective = <c>` + `algorithm = <a>` + the calibrated
`topology`. The `(collective, algorithm)` pair must be one the inline dispatch
already handles (so the same injector runs, now with profile-sourced vars); see
the `else if (collective == …)` branches in `scratch/gpu-cluster-sim.cc`. Add a
new `(collective, algorithm)` pair only by extending that inline dispatch.

### A new custom protocol

Author a `.cfg` with an `[op]` transfer stencil (no `collective=`/`algorithm=`).
The stencil runs through the generic runner with the profile's PEX bundle
applied — see [ARCHITECTURE.md](ARCHITECTURE.md#config-driven-protocol-stack-cfg)
and `h200-request-response.cfg`.
