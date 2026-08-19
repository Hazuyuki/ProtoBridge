# ProtoBridge protocol profile -- H200 NVLink4, NCCL LL128, RS(544,514,15) FEC.
# This mirrors the validated COMMON_FLAGS used by the 128-GPU sweeps.
# Usage:  ./ns3 run "protocol-profile-demo --profile=configs/protocol_profiles/h200-ll128.profile"

# --- protocol choice ---
protocolModel = ns3::NcclProtocolModel
forceProtocol = 0                       # 0 = auto-select (LL/LL128/SIMPLE/NVLS)

# --- NCCL model attributes (per-TypeId; applied generically via String) ---
# Protocol-specific startup (mirrors configs/hardware/h200.json startupDelay):
# auto-select (forceProtocol=0) picks LL<8KB, LL128 8KB..2MB, SIMPLE>2MB, so
# each size lands on its measured startup (matches the calibration sweep).
StartupDelayLL = 15000
StartupDelayLL128 = 25000
StartupDelaySIMPLE = 46000
StartupNVLS = 23000
LlThreshold = 8192
Ll128Threshold = 2097152
PerGpuStartupDelayNs = 0

# --- packet execution layer: FEC (reliability mechanism as parameters) ---
fecN = 544
fecK = 514
fecT = 15
fecEncodeLatencyNs = 50
fecDecodeLatencyNs = 80

# --- packet execution layer: link BER (injected knob) ---
berIntraNodeElectrical = 1e-15
berIntraRackElectrical = 1e-13
berInterRackOptical = 1e-9

# --- packet execution layer: flow control (reusable credit object) ---
vcCount = 4
vcCredits = 64

# --- send-gate flow-control policy (credit=per-VC implemented; window/rate seam-only) ---
flowControl = credit

# --- link-level retry (ARQ) ---
llrEnabled = 0                           # NVLink relies on FEC, not ARQ; UB sets this = 1
llrMode = gobackn                        # gobackn (cumulative) | sack (selective)

# --- fabric hardware (consumed by the simulator's topology builder, not the
# PEX bundle; reserved so Build() does not forward them as protocol attrs).
# These let a `collective =` / `algorithm =` op reproduce a calibrated fabric
# without per-run CLI flags. Values mirror configs/hardware/h200.json.
# bandwidthGbps is the 8-GPU effective per-link rate (375 GB/s aggregate /
# 18 lanes ~= 166 Gbps; rounded to the empirical sweep best, reproduces ring
# allreduce to ~4-5%).
bandwidthGbps = 170
latencyNs = 400
numLanes = 18
linksPerGpu = 1
sprayChunkSize = 131072
switchVoqDepth = 10000
switchArbIntervalNs = 100
