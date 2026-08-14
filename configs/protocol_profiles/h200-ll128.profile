# ProtoBridge protocol profile -- H200 NVLink4, NCCL LL128, RS(544,514,15) FEC.
# This mirrors the validated COMMON_FLAGS used by the 128-GPU sweeps.
# Usage:  ./ns3 run "protocol-profile-demo --profile=configs/protocol_profiles/h200-ll128.profile"

# --- protocol choice ---
protocolModel = ns3::NcclProtocolModel
forceProtocol = 0                       # 0 = auto-select (LL/LL128/SIMPLE/NVLS)

# --- NCCL model attributes (per-TypeId; applied generically via String) ---
StartupDelayLL = 15000
StartupDelayLL128 = 15000
StartupDelaySIMPLE = 15000
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
