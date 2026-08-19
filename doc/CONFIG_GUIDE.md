# Config Guide — the `.cfg` / `.profile` primary input

ProtoBridge's primary user input is a **protocol config** (`.cfg`) that names a
calibrated collective and the fabric it runs on, plus a **protocol profile**
(`.profile`) that bundles the protocol / FEC / credit / BER / hardware values.
A one-line run reproduces a measured H200 latency with no per-run CLI flags:

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
| `[op]` | OTP (operation → packet) | either `builtin = <name>` (delegate to a calibrated injector) **or** a transfer stencil |

```ini
[stack]
profile = configs/protocol_profiles/h200-ll128.profile

[op]
builtin  = ring        # delegate, OR leave empty and declare a stencil
topology = ring        # optional: overrides the CLI fabric
```

## 2. The two `[op]` forms

### 2a. Builtin delegation (primary, calibrated, bit-identical)

`builtin = <ring|tree|sharp|hierarchical|nvls>` delegates to the calibrated
inline injector. The simulator sources the profile's PEX + fabric-hardware
keys into the same local CLI variables the inline path consumes, then runs the
injector **verbatim**. Because every sourced key maps 1:1 to an inline CLI
flag, the `.cfg` run is **bit-identical** to the inline path with the
profile's values as flags (same code, same values, same `RngRun`).

| `builtin` | collective | algorithm | `topology` | shipped `.cfg` |
|-----------|-----------|-----------|------------|-----------------|
| `ring` | allreduce | ring | `ring` | `h200-ring-allreduce.cfg` |
| `tree` | allreduce | tree | `switched` | `h200-tree-allreduce.cfg` |
| `sharp` | allreduce | sharp | `switched` | — |
| `hierarchical` | allreduce | hierarchical | `switched` | — |
| `nvls` | allgather | nvls | `switched` | `h200-nvls-allgather.cfg` |

> `topology` is part of the calibration: ring allreduce on `switched` does
> **not** reproduce the measurement (it runs ~6× slower). Tree and NVLS use
> the NVSwitch (`switched`) fabric.

### 2b. Custom transfer stencil (define a new protocol)

Leave `builtin` empty and declare `param` / `replicate` / `transfer.*` lines.
A compiler (`ProtocolConfig::Compile`) expands the stencil through the vendor
`ProtocolModel` into a transaction graph run by the generic runner. See
[ARCHITECTURE.md](ARCHITECTURE.md#config-driven-protocol-stack-cfg) for the
full stencil schema; `h200-request-response.cfg` is a worked two-leg
ping-pong example.

## 3. The `.profile` keys

`h200-ll128.profile` mirrors `configs/hardware/h200.json`. Keys fall into two
groups; both are **sourced** into the inline path when a `[op]` uses
`builtin=`:

**PEX bundle** (drive the protocol stack — applied to the bundle / endpoint,
not sourced into CLI vars for the builtin path, since the builtin path keeps
the inline 1-VC fabric model):

```
protocolModel      = ns3::NcclProtocolModel
forceProtocol      = 0          # 0=auto (LL<8KB, LL128 8KB..2MB, SIMPLE>2MB)
vcCount            = 4
vcCredits          = 64
flowControl        = credit
llrEnabled         = 0
llrMode            = gobackn
```

**Fabric hardware + protocol startup + FEC + BER** (sourced into the local CLI
variables — these are what make a `.cfg` reproduce a calibrated run without
per-run flags):

```
# fabric hardware
bandwidthGbps           = 170     # 8-GPU effective per-link (375 GB/s / 18 lanes)
latencyNs               = 400
numLanes                = 18
linksPerGpu             = 1
sprayChunkSize          = 131072
switchVoqDepth          = 10000
switchArbIntervalNs     = 100
# protocol startup (per-TypeId; mirrors h200.json startupDelay)
StartupDelayLL          = 15000
StartupDelayLL128       = 25000
StartupDelaySIMPLE      = 46000
StartupNVLS             = 23000
LlThreshold             = 8192
Ll128Threshold          = 2097152
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

A `builtin=` `.cfg` sources the profile's values into the local variables the
inline path consumes, then runs the same injector. The two paths must produce
identical `simTimeNs`. `test/parity/test_config_builtin_parity.py` is the
gate — it parses the profile, emits the equivalent inline flags, runs both
paths for `{ring, tree, nvls}` × sizes × GPU counts, and asserts exact
`simTimeNs` equality:

```bash
python3 -m pytest test/parity/test_config_builtin_parity.py -q
# or: python3 test/parity/test_config_builtin_parity.py
```

Verified cases (config `==` inline, `simTimeNs`):

| builtin | size | GPUs | simTimeNs | measured |
|---------|------|------|-----------|----------|
| ring | 1 MiB | 8 | 38308 | 37.24 µs |
| ring | 256 MiB | 8 | 1353580 | 1347.65 µs |
| ring | 1 MiB | 4 | 33028 | — |
| ring | 256 MiB | 4 | 1163404 | — |
| tree | 1 MiB | 8 | 114794 | — |
| nvls | 1 MiB | 8 | 80352 | — |

If you change a sourced profile key, the C++ sourcing block
(`scratch/gpu-cluster-sim.cc`, after `configMode = true;`), or an injector,
re-run the parity test — a divergence is a behavior change, not a refactor.

## 5. Authoring

### A new hardware profile

Copy `h200-ll128.profile`, change the fabric-hardware + startup keys (mirroring
a `configs/hardware/*.json` spec), and reference it from a `.cfg`
`[stack] profile =`. The `bandwidthGbps` value is the effective per-link rate
for the target GPU count (H200 8-GPU: 375 GB/s aggregate ÷ 18 lanes ≈ 166,
rounded to the empirical sweep best 170).

### A new builtin op

Add the name to the `builtin → (collective, algorithm)` map in
`scratch/gpu-cluster-sim.cc` (the `if (!builtin.empty())` block) and ship a
`.cfg` with `builtin = <name>` + the calibrated `topology`. The injector must
be the one the inline dispatch already runs for that `(collective, algorithm)`.

### A new custom protocol

Author a `.cfg` with an `[op]` transfer stencil (no `builtin=`). The stencil
runs through the generic runner with the profile's PEX bundle applied — see
[ARCHITECTURE.md](ARCHITECTURE.md#config-driven-protocol-stack-cfg) and
`h200-request-response.cfg`.
