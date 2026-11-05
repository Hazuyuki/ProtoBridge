# Config Guide — the `.cfg` / `.profile` primary input

ProtoBridge's primary input is a **protocol config** (`.cfg`) describing an
operation and its fabric. A reusable **protocol profile** (`.profile`) supplies
protocol, hardware, FEC, credit and error settings. Configurations can select a
collective algorithm or define a custom transfer stencil.

The shipped H200 ring configuration runs with:

```bash
./ns3 run "gpu-cluster-sim --protocolConfig=\
configs/protocol_configs/h200-ring-allreduce.cfg --numGpus=8 --dataSize=1048576"
# 8-GPU 1 MiB ring allreduce -> simTimeUs=38.3  (H200 measurement 37.24 us)
```

`numGpus` and `dataSize` stay on the CLI (they are the experiment knobs); the
`.cfg` carries the protocol + operation + hardware.
Calibration applies to the workload and conditions recorded in
[CALIBRATION.md](CALIBRATION.md); a configuration file alone does not establish
hardware accuracy.

For CPU/GPU RDMA Write, `[stack]` also selects submission and payload memory:

```ini
rdmaSubmission = detailed
rdmaInitiator = cpu
rdmaDataPath = gpudirect
rdmaCpuWqeBuildNs = 100
rdmaQueueDepth = 128
```

`rdmaSubmission=startup` retains the existing startup-delay behavior. Detailed
mode supports `cpu|gpu` initiation independently of `host|staged|gpudirect`
payload paths. See [RDMA.md](RDMA.md) for examples, parameter meanings and the
integration interface. The RDMA example profile is illustrative and uncalibrated.

## Configuration precedence

Load `.profile` values first, then override them with `[stack]` values. Resolved
profile/stack settings that map to CLI settings override the CLI value for
the same key. `[op] topology` also selects the fabric when supplied. Unspecified
CLI settings keep their values; `numGpus` and `dataSize` remain runtime inputs.

For example, change `[stack] rdmaSubmission` when switching an existing RDMA
config. A conflicting `--rdmaSubmission` flag does not override that stack key.

## 1. The two layers

A `.cfg` has two sections:

| Section | Layer | What it carries |
|---------|-------|-----------------|
| `[stack]` | PEX (packet execution) | a `profile = …` reference to a `.profile` (protocol model, FEC, credits, BER, LLR, **fabric hardware**) |
| `[op]` | OTP (operation → packet) | either `collective =` + `algorithm =` (delegate to a shared injector) **or** a transfer stencil |

```ini
[stack]
profile = configs/protocol_profiles/h200-ll128.profile

[op]
collective = allreduce   # delegate, OR leave empty and declare a stencil
algorithm  = ring
topology   = ring         # optional: overrides the CLI fabric
```

## 2. The two `[op]` forms

### 2a. Collective + algorithm delegation

Declaring `collective = <c>` and `algorithm = <a>` selects the same injector
as the inline CLI path. See [bit-identity checks](#4-the-bit-identity-guarantee)
for equivalent-input rules and regression commands.

| `collective` | `algorithm` | `topology` | shipped `.cfg` |
|--------------|------------|------------|-----------------|
| allreduce | ring | `ring` | `h200-ring-allreduce.cfg` |
| allreduce | tree | `switched` | `h200-tree-allreduce.cfg` |
| allreduce | sharp | `switched` | — |
| allreduce | hierarchical | `switched` | — |
| allgather | nvls | `switched` | `h200-nvls-allgather.cfg` |

Keep the listed topology when reproducing a reference configuration. Ring,
tree and NVLS use different fabric layouts; changing the topology changes the
experiment.

### 2b. Custom transfer stencil (define a new protocol)

Leave `collective`/`algorithm` unset and declare `param` / `replicate` /
`transfer.*` lines. A compiler (`ProtocolConfig::Compile`) expands the stencil
through the vendor `ProtocolModel` into a transaction graph run by the generic
runner. A minimal one-transfer stencil is:

```ini
[stack]
profile = configs/protocol_profiles/h200-ll128.profile

[op]
param.N = numGpus
replicate.gpu = 0 .. 0
startup = 0
per_step_delay = 0
transfer.0.kind = P2P
transfer.0.src = 0
transfer.0.dst = 1 mod N
transfer.0.bytes = dataSize
transfer.0.vc = 0
complete = all
```

This is the shipped `p2p-single.cfg`. Run it with at least two GPUs:

```bash
./ns3 run "gpu-cluster-sim --protocolConfig=configs/protocol_configs/p2p-single.cfg --numGpus=2 --dataSize=65536 --topology=switched" --no-build
```

| Key | Meaning |
|---|---|
| `param.<name>` | Integer expression using `numGpus`, `dataSize` and previously defined parameters. Expressions support `+`, `-`, `*`, `/`, `mod` and parentheses. |
| `replicate.<var>` | Inclusive integer domain `lo .. hi`. The first dimension identifies independent chains; instances within a chain and their numbered transfers execute in order. |
| `startup` | Per-chain startup in ns, or `auto` to use the protocol model's startup for the operation size and GPU count. |
| `per_step_delay` | Additional ns between transfer actions in a chain. |
| `transfer.<id>.*` | Numbered transfer fields: `kind` (`DATA` or `P2P`), `src`, `dst`, `bytes` and `vc`. Expressions may refer to replication variables. |
| `complete` | `all` waits for every terminal chain; `any` waits for one. |

`h200-request-response.cfg` is a two-leg example in which the response waits
for request delivery. Addressed RDMA operations use the
[operation API](RDMA.md#operations). The parser and compiler are defined in
[`protocol-config.h`](../src/gpu-cluster/model/protocol-config.h); the
[architecture guide](ARCHITECTURE.md#configuration-boundary) explains the
OTP/PEX boundary.

## 3. Protocol and network parameters

Profile hardware/protocol keys are sourced into the CLI variables used by
delegated collectives. PEX-bundle keys are applied through `Build()` to bundle
objects and endpoints. The latter include `vcCount`, `vcCredits`, `flowControl`
and the protocol attribute `PerGpuStartupDelayNs`; they are not all equivalent
to inline CLI overrides. The delegated path retains the inline fabric's VC
configuration. See the [precedence rules](#configuration-precedence).

For the H200 profile, the PEX credit settings are:

```ini
vcCount = 4
vcCredits = 64
flowControl = credit
```

Local credits apply to scale-up traffic. The [architecture guide](ARCHITECTURE.md#flow-control-and-congestion)
owns the ECN/CNP, pacing, PFC, retry and contention mechanisms. The following
tables list related native CLI controls; use `./ns3 run "gpu-cluster-sim --PrintHelp"`
for the complete CLI.

### Scale-out flow control

| Control | Meaning / default |
|---|---|
| `--dcqcn` | Enable ECN/CNP-based rate control; off by default. |
| `--ecnThreshold` | Queue threshold for RoCE ECN marking. |
| `--perQpCeilingGbps` | Per-flow rate ceiling in decimal Gb/s; 0 leaves it disabled. |
| `--switchPfcMode` | 0 off, 1 static, 2 dynamic, 3 timed pause; default 0. |
| `--pfcPauseThreshold`, `--pfcResumeOffset` | Pause threshold and resume hysteresis, defaults 64 and 8 chunks. |
| `--pfcPauseTime` | Timed-pause duration in microseconds; default 0. |
| `--pfcHeadroomChunks` | Reserved headroom for in-flight packets after pause; 0 selects legacy accounting. |
| `--voqDropThreshold` | Positive values opt RoCE into tail-drop and retry; the native plain-Ethernet default is 8. |
| `--llrMode`, `--llrRetryLimit` | `gobackn` or `sack`, and retry limit. |
| `--llrRetryTimeout`, `--llrTimeoutRetransmit` | Retry timeout as integer ns (`500000` for 500 us; 0 selects auto) and timeout-triggered retransmission (off by default). |
| `--soTxWindow` | Outstanding scale-out packet/chunk slots; separate from RDMA WR depth and PCIe read tags. |
| `--soGbnStrict` | In-order-only remote GBN receive semantics; off by default. |
| `--packetLossRate` | Whole-packet loss probability, independent of pre-FEC BER. |
| `--ber`, `--fecN`, `--fecK`, `--fecT` | Pre-FEC bit-error rate and the FEC code parameters. |
| `--rttcc` | Enable delay-based pacing for remote flows; off by default. |
| `--rttccProbeIntervalNs`, `--rttccTargetRttNs` | Probe interval and target RTT, defaults 5000 and 10000 ns. |

PFC needs a switched inter-node path (`host`, `tor`, `rail`, `leafspine`). Set
active thresholds in the order `ecnThreshold < pfcPauseThreshold < voqDropThreshold`.
When PFC and tail-drop are both enabled, the drop threshold must be above
the pause threshold.
Per-VC pause thresholds use `EthernetSwitch::SetPfcPauseThresholdForVc` in C++.
The default has no per-VC overrides. The [PFC comparison](CALIBRATION.md#pfc-and-scale-out-cross-checks)
records matched settings and headroom assumptions.

### Rate-machine parameters

| DCQCN CLI control | Default |
|---|---:|
| `--dcqcnTFastNs` | 50000 ns |
| `--dcqcnFastCycles` | 5 |
| `--dcqcnTAiNs` | 100000 ns |
| `--dcqcnRai` | 0.01 |
| `--dcqcnGInit`, `--dcqcnGDecay` | 0.5, 0.5 |
| `--dcqcnAlphaFloor` | 0.0 |

These CLI overrides are applied when supplied with an active value; otherwise
the TypeId default is kept. The DCQCN maximum-rate anchor follows the
inter-node link rate when enabled. RTTCC's TypeId exposes
`AdditiveIncreaseGbps` (10), `Beta` (0.8) and `MinRateGbps` in addition to the
probe/target controls above. Parameter sweeps are under
[`collective_topology_dcqcn/`](../examples/gpu-cluster/collective_topology_dcqcn/README.md).

### Workloads and contention

| CLI controls | Use |
|---|---|
| `--algorithm=ncclring`, `--ringChannels` | Allreduce on parallel global rings; 0 channels selects `gpusPerNode`. |
| `--interNodeTopology`, `--interNodeSpines`, `--interNodeTorusWidth`, `--interNodeDragonflyGroupSize` | Select the inter-node fabric and its shape. |
| `--linkDataRate` | Per-physical-link rate in Gb/s; 0 uses `bandwidth`. |
| `--perStepSwOverhead`, `--swOverheadPerByte`, `--computeBaseLL`, `--computeBaseLL128`, `--computeBaseSIMPLE`, `--llThreshold`, `--ll128Threshold`, `--startupDelay` | Collective protocol selection and per-step software/compute service controls. |
| `--contention`, `--contentionCollectiveWeight`, `--contentionMemoryWeight`, `--contentionP2pWeight` | Enable weighted contention and set class weights; defaults 0.7/0.2/0.1. |
| `--bgP2pEnabled`, `--bgP2pSrc`, `--bgP2pDst`, `--bgP2pSizeBytes`, `--bgP2pStartTimeNs`, `--bgP2pIntervalNs` | Inject a concurrent background flow. |
| `--randTraffic`, `--randMsgBytes`, `--randInjectionRate`, `--randWarmupNs`, `--randMeasureNs` | Fixed-duration random inter-node traffic; default message 64 KiB, warmup 50 us and measurement 200 us. |

`bgP2pIntervalNs=0` uses one packet-serialization time at the intra-node rate,
with a minimum of 1 ns. Set it explicitly to pace a remote background flow at
the inter-node rate. `bgP2pBytesSent`/`bgP2pPacketsSent` count injection calls.
Random traffic requires multiple nodes; its offered/delivered counters cover
only the measurement window. `RngRun` selects the random seed/initial phases.
These modes and congestion control run in packet simulation; the surrogate's
scope is described in [SURROGATE.md](SURROGATE.md#scope).

Example hardware, startup, FEC and BER keys from the H200 profile:

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
`linksPerGpu=18` fabric convention (all six re-measured at the enforced
bulk-chunk cap; the 38782/1496204/33634/1284496 ring values recorded in an
earlier revision of this table were mismeasured — current runs produce the
values below):

| op | size | GPUs | simTimeNs | measured |
|----|------|------|-----------|----------|
| ring | 1 MiB | 8 | 38308 | — |
| ring | 256 MiB | 8 | 1353580 | — |
| ring | 1 MiB | 4 | 33028 | — |
| ring | 256 MiB | 4 | 1163404 | — |
| tree | 1 MiB | 8 | 55148 | — |
| nvls | 1 MiB | 8 | 28034 | — |

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
applied — see [ARCHITECTURE.md](ARCHITECTURE.md#configuration-boundary)
and `h200-request-response.cfg`.

## 6. Time values and the simulation horizon

ns-3 stores time in signed 64-bit ticks. A tick is one nanosecond by default;
`Time::SetResolution()` can select Y, D, H, MIN, S, MS, US, NS, PS, or FS.
Changing resolution also changes the largest representable simulation duration.

ProtoBridge's bounded schedulers reserve `Time::Max()` as an unreachable marker.
They schedule only nonnegative delays whose deadlines are strictly before that
marker. Clamping a deadline to `Time::Max()` is insufficient: the raw ns-3
scheduler can execute an event there, and its next transmission can wrap time.

The shared `ns3/time-bounds.h` helpers apply these rules:

- Check integer nanoseconds before scaling. Unsigned inputs remain valid when
  they fit the selected native unit: `2^63` ns is `9223372036854775` US ticks.
  At PS or FS resolution, `2^61` ns saturates instead of wrapping to zero.
- Preserve ordinary ns-3 fixed-point rounding. Large finite seconds use native
  ticks when needed: `2^64` seconds is `307445734561825860` MIN ticks.
- Check quotients before dividing and rate products before multiplying. Pacers
  keep the existing minimum one-nanosecond slot, using a wide nanosecond ledger
  so sub-tick slots can accumulate at coarse resolutions.
- Keep FEC wire counts wide through serialization. Encoding `2^30` payload
  bytes with `n=2147483647, k=1` gives `2305843010287435775` encoded bytes;
  converting this count to bits must retain values above UINT64_MAX.
- Check actual point-to-point egress as well as deferred callbacks. The wire
  frame includes its PPP header. Serialization plus propagation, and
  serialization plus the interframe gap, must both fit. A queued frame is
  checked again when its transmission would start.
- Reject negative timing attributes and direct latency settings at the input
  boundary. No negative delay is passed to the raw scheduler.

An unreachable receive post reports `RdmaReceiveCompletion.success=false`
immediately. It consumes no receive-queue slot and leaves the post-availability
ledger unchanged. `postedReceives` counts validated post attempts;
`receiveCompletions` includes these failed completions. Other work whose next
step cannot occur within the horizon stays unfinished; it does not gain a
successful completion at the maximum timestamp.

Run the focused checks after building tests:

```bash
./ns3 build gpu-cluster-sim test-runner
./ns3 run "test-runner --test-name=gpu-cluster-time-safety --verbose"
python -m pytest test/parity/test_time_safety.py -q
```

The Python test starts a separate process for each resolution. On glibc it
enables traps for floating-point overflow, invalid operations, and division by
zero. The ordinary collective, RDMA, PCIe, and WebUI regression suites cover
the usual parameter ranges.
