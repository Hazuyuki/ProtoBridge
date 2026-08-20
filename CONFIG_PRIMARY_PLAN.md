# Plan: Make the OTP/PEX `.cfg` the PRIMARY input (bit-identical to inline)

**Target repo:** `protobridge/` (OSS release; parent untouched). **Branch:** `master`.
**Locked params:** (a) protobridge  (b) protocol+op+hardware in `.cfg`/`.profile`, numGpus/dataSize on CLI  (c) MUST be bit-identical to the calibrated inline path + parity test.
**Approach chosen:** "Delegate calibrated ops" — ship H200 ring/tree/nvls `.cfg`s
whose `[op]` declares `collective =` + `algorithm =` (+ `topology =`). The sim
sources the `.profile` values into the SAME local CLI variables the inline path
uses, then runs the inline injector VERBATIM. Because every profile key maps 1:1
to an inline CLI flag, this is bit-identical by construction (same code, same
values). The generic transfer-stencil path stays for custom operations
(ApplyBundle + ProtocolConfigRunner, unchanged).

> **Refactor note (2026-08-19).** The original draft used a `[op] builtin =`
> keyword that mapped internally to a `(collective, algorithm)` pair. That
> intermediate layer was removed: a `.cfg` now declares the three real CLI axes
> directly — `collective =` + `algorithm =` (+ `topology =`) — exactly what the
> inline path consumes. No `builtin` keyword, map, or wording remains in code,
> configs, tests, or docs. See the parity test
> `test/parity/test_config_vs_inline_parity.py`.

## Root cause of the prior ~18x gap (measured, now bypassed)

With matched H200 hardware (400/400/18) + startup (15000) + protocol (auto), inline =
107.4µs but the STENCIL config path = 24.9µs at 1MB; the gap WIDENS with size (inline
4727µs vs config 161µs at 64MB). The stencil config path rate-limits on AGGREGATE
multi-lane bandwidth (~18x = numLanes), not per-link — a physical-model divergence in
`SendBulkWireTransferSize`/`ApplyBundle`'s spray/rate-limiting when driven by the
generic runner. Fixing the stencil wire-model is high-risk; DELEGATION bypasses it:
the collective/algorithm path uses the inline injector's (correct, per-link) wire
model directly.

## Files modified (all under `protobridge/`)

1. **`src/gpu-cluster/model/protocol-config.{h,cc}`** — `[op]` parses
   `collective =` + `algorithm =` + `topology =`. New `m_collective`,
   `m_algorithm`, `m_topology`; getters `GetCollective()`, `GetAlgorithm()`,
   `GetTopology()`. When `collective`/`algorithm` are set, the transfer
   stencil is optional (no error on missing `transfer.*`); the two axes must
   be declared together. Existing stencil parsing untouched.

2. **`src/gpu-cluster/model/protocol-profile.{h,cc}`** — 7 hardware keys added to
   `IsReserved()` (so `Build()` does not forward them as protocol attributes):
   `bandwidthGbps, latencyNs, numLanes, linksPerGpu, sprayChunkSize, switchVoqDepth,
   switchArbIntervalNs`. No new getters (the sim reads them via existing `Get()`).

3. **`scratch/gpu-cluster-sim.cc`** — the wiring:
   - In the `configMode` setup block (after `pbundle = prof.Build()`, ~line 473;
     BEFORE the `Config::SetDefault` and topology build): a `configMode` block
     that reads the profile's PEX+hardware keys via `prof.Get(key, default)`
     into the local CLI variables (bandwidthGbps, latencyNs, numLanes,
     linksPerGpu, sprayChunkSize, switchVoqDepth, switchArbIntervalNs,
     startupLL/LL128/SIMPLE/NVLS, llThreshold, ll128Threshold,
     perGpuStartupDelayNs, forceProtocol, protocolModelType,
     protocolWireEfficiency, fabricTypeStr, fecN/K/T, fecEncode/DecodeLatency,
     fecScope, berIntraNodeElectrical/RackElectrical/InterRackOptical,
     llrEnabled, llrModeStr, llrBufferSize, vcCreditsBytes, vcBufferSizeBytes,
     perStepSwOverheadNs, swOverheadPerByteNs) AND reads `.cfg`
     `collective`+`algorithm`+`topology` into the local `collective`/
     `algorithm`/`topology`. Config values win over CLI defaults (config is
     primary); numGpus/dataSize stay CLI.
     > **Sourcing scope (verified against `scratch/gpu-cluster-sim.cc`).** The
     > configMode block sources 25 profile keys into the inline CLI vars (7
     > fabric-hardware + 8 protocol/startup/threshold + 5 FEC + 3 BER + 2 LLR).
     > The PEX-bundle keys `vcCredits`/`vcCount`/`flowControl` are NOT promoted
     > to CLI vars on the collective/algorithm path (they are applied to the
     > bundle object by `ProtocolProfile::Build()` via `SetAttributeFailSafe`,
     > keeping the inline 1-VC fabric = the spec credits); `llrBufferSize`
     > stays CLI-only (`--llrBufferSize`), and `perGpuStartupDelayNs` is
     > likewise applied to the bundle's protocol model via `SetAttributeFailSafe`
     > (not via this block nor `Config::SetDefault`; the `Config::SetDefault`
     > path is driven by the separate `--startupPerGpuNs` CLI flag). The
     > authoritative 1:1 map is `PROFILE_TO_FLAG` in
     > `test/parity/test_config_vs_inline_parity.py`.
   - In the injector-dispatch block: the stencil gate is
     `if (configMode && pconfig.GetCollective().empty() && pconfig.GetAlgorithm().empty())`
     so a `collective`+`algorithm` config SKIPS the stencil branch
     (ApplyBundle + configRunner) and falls through to the inline
     `else if (collective == "allreduce"/"allgather"/...)` branches — which run
     UNTOUCHED. The inline injector therefore runs with profile-sourced vars.

4. **`configs/protocol_profiles/h200-ll128.profile`** — hardware keys:
   `bandwidthGbps=170`, `latencyNs=400`, `numLanes=18`, `linksPerGpu=1`,
   `sprayChunkSize=131072`, `switchVoqDepth=10000`, `switchArbIntervalNs=100`.
   (Startup 15000/25000/46000 + PEX values stay — they are the H200 config.)

5. **`configs/protocol_configs/`** — `h200-ring-allreduce.cfg`
   (`collective=allreduce`/`algorithm=ring`/`topology=ring`),
   `h200-tree-allreduce.cfg` (`collective=allreduce`/`algorithm=tree`/
   `topology=switched`), `h200-nvls-allgather.cfg` (`collective=allgather`/
   `algorithm=nvls`/`topology=switched`). `h200-request-response.cfg` stays as
   the stencil example.

6. **`test/parity/test_config_vs_inline_parity.py`** — parses
   `h200-ll128.profile`, emits the equivalent inline CLI flags, runs the binary
   TWICE (config vs inline-flags) for {1MiB, 256MiB} × {4,8} GPU ring + tree/
   nvls smoke, asserts `simTimeNs` EQUAL (exact).

7. **Docs** — `README.md` Quickstart leads with `--protocolConfig`;
   `doc/CONFIG_GUIDE.md` (the two-file `.cfg`+`.profile` form,
   collective/algorithm vs stencil); `doc/ARCHITECTURE.md` (the delegation
   section); `CONTRIBUTING.md` (the bit-identical reality + how to run the
   parity test).

## DoD (validation, in order)

1. `cmake --build cmake-cache -j 127` (protobridge) → green.
2. `./ns3 run "test-runner --suite=gpu-cluster"` + `--suite=gpu-cluster-integration` → green.
3. `python3 test/parity/test_config_vs_inline_parity.py` → all configs
   bit-identical (config simTimeNs == inline simTimeNs). **This is the
   bit-identical gate.**
4. Manual smoke: `./ns3 run "gpu-cluster-sim --protocolConfig=\
   configs/protocol_configs/h200-ring-allreduce.cfg --numGpus=8 --dataSize=1048576"`
   reproduces the inline H200 value (no hardware/protocol flags needed).
5. Commit in `protobridge` (explicit file paths; never `git add -A` in parent).
6. Update parent `doc/TASKS.md` + `PLAN.md` checkpoint. No push without authorization.

## Risks

- **Mapping mismatch:** if a profile key is mis-mapped to the wrong local var, the
  parity test catches it (config != inline). Fix (≤2 self-repairs).
- **float formatting:** the parity test parses `simTimeNs=` as an int compare.
- **topology in .cfg vs CLI:** config wins; if a user passes `--topology` it is
  overridden — acceptable (config is primary for fabric).
