/*
 * gpu-cluster-sim.cc
 *
 * Generic GPU cluster simulation program.
 * Accepts all parameters via CommandLine for config-file driven interface.
 * Uses GpuClusterTopologyHelper for topology construction.
 *
 * Output format: machine-parseable RESULT_START/RESULT_END block
 */

#include "ns3/core-module.h"
#include "ns3/node-list.h"
#include "ns3/gpu-cluster-helper.h"
#include "ns3/fabric-endpoint.h"
#include "ns3/nvswitch.h"
#include "ns3/ethernet-switch.h"
#include "ns3/ring-allreduce.h"
#include "ns3/sharp-allreduce.h"
#include "ns3/nvls-allgather.h"
#include "ns3/tree-allreduce.h"
#include "ns3/fullmesh-allreduce.h"
#include "ns3/fullmesh-allgather.h"
#include "ns3/fullmesh-reducescatter.h"
#include "ns3/ring-allgather.h"
#include "ns3/ring-reducescatter.h"
#include "ns3/ring-broadcast.h"
#include "ns3/ring-reduce.h"
#include "ns3/alltoall-injector.h"
#include "ns3/hierarchical-allreduce.h"
#include "ns3/hierarchical-allgather.h"
#include "ns3/hierarchical-reducescatter.h"
#include "ns3/hierarchical-alltoall.h"
#include "ns3/nccl-ring-allreduce.h"
#include "ns3/multi-node-topology-helper.h"
#include "ns3/device-type.h"
#include "ns3/fabric-type.h"
#include "ns3/protocol-model.h"
#include "ns3/protocol-profile.h"
#include "ns3/protocol-config.h"
#include "ns3/protocol-config-runner.h"
#include "ns3/nccl-protocol-model.h"
#include "ns3/ub-protocol-model.h"
#include "ns3/roce-protocol-model.h"
#include "ns3/ethernet-protocol-model.h"
#include "ns3/dcqcn-rate-machine.h"
#include "ns3/rttcc-rate-machine.h"
#include "ns3/protocol-payload-builder.h"
#include "ns3/nccl-protocol-payload-builder.h"
#include "ns3/roce-payload-builder.h"
#include "ns3/ub-payload-builder.h"

#include "ns3/link-degradation.h"
#include "ns3/fec-model.h"
#include "ns3/llr-manager.h"
#include "ns3/contention-model.h"
#include "ns3/p2p-traffic.h"
#include "ns3/point-to-point-helper.h"
#include "ns3/point-to-point-net-device.h"
#include "ns3/point-to-point-channel.h"
#include "ns3/drop-tail-queue.h"

#include <algorithm>
#include <iostream>
#include <iomanip>
#include <limits>
#include <queue>
#include <unordered_map>
#include <vector>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("GpuClusterSim");

static uint64_t g_durationNs = 0;
static bool g_collectiveCompleted = false;

// ---------------------------------------------------------------------------
// Wire-transfer patterns (p2p / fanout / incast): bare point-to-point bulk
// transfers on the SAME credit-gated bulk path (SendBulkWireTransferSize)
// the collectives use — the credit-BDP experiment's non-collective arms.
// Unlike the collective injectors there is no NCCL startup (bare-transfer
// semantics, p2p-single.cfg precedent): completion = last chunk delivered.
// Patterns (root = --rootRank, default 0):
//   p2p     : one flow      root -> (root+1)%N
//   fanout  : N-1 flows     root -> every other rank       (1-to-many)
//   incast  : N-1 flows     every other rank -> root       (many-to-1)
// ---------------------------------------------------------------------------
static uint64_t g_patternExpectedBytes = 0;
static uint64_t g_patternReceivedBytes = 0;
static uint64_t g_patternStartNs = 0;

void OnCollectiveComplete(uint64_t durationNs);  // defined below

static void
PatternReceive(uint16_t srcRank, Ptr<Packet> packet, FabricHeader header)
{
    (void)srcRank;
    // Control-plane packets (CREDIT/ACK/NACK) do not advance a transfer, and
    // permanently lost data cannot complete a delivery (mirrors the
    // collective injectors; only reachable at ber>0 / lossy fabrics, which
    // these lossless experiments do not use).
    if (header.GetPacketType() != FabricPacketType::DATA)
    {
        return;
    }
    uint64_t bytes = header.GetEffectiveDataSize();
    if (bytes == 0)
    {
        bytes = packet->GetSize();
    }
    g_patternReceivedBytes += bytes;
    if (g_patternExpectedBytes > 0
        && g_patternReceivedBytes >= g_patternExpectedBytes)
    {
        OnCollectiveComplete(Simulator::Now().GetNanoSeconds() - g_patternStartNs);
    }
}

// Adaptive algorithm selection (NCCL-like): pick the collective algorithm
// from (topology, message size, N). Small/latency-bound messages favor tree
// (2*log2(N) steps); large/bandwidth-bound favor ring (2*(N-1) pipelined);
// NVSwitch fabrics can use NVLS/SHARP (switch-assisted, ~2 steps). Mirrored
// in Python by scripts/algo_selector.py.
static std::string
ResolveAutoAlgorithm(const std::string& coll, const std::string& topo,
                     uint64_t dataSize, uint32_t N, bool hasNvls)
{
    const uint64_t MEDIUM = 1ull << 26;   // 64 MiB
    const uint64_t NVLS_THRESH = 1ull << 23;  // 8 MiB

    if (coll == "allgather" || coll == "reducescatter")
    {
        if (hasNvls && dataSize >= NVLS_THRESH)
        {
            return "nvls";
        }
        return "ring";
    }
    if (coll == "allreduce")
    {
        if (hasNvls && dataSize >= NVLS_THRESH)
        {
            return "nvls";
        }
        // Tree wins for latency-bound small messages (far fewer steps).
        if (dataSize < MEDIUM)
        {
            return "tree";
        }
        return "ring";
    }
    return "ring";
}

void
OnCollectiveComplete(uint64_t durationNs)
{
    g_durationNs = durationNs;
    g_collectiveCompleted = true;
    Simulator::Stop();
}

// ---------------------------------------------------------------------------
// Random-traffic mode (--randTraffic): fixed-duration uniform-random So load.
// Every rank injects fixed-size messages at a fixed interval derived from
// --randInjectionRate (fraction of the inter-node line rate), each message to
// a uniformly random rank on ANOTHER node. Messages go through
// SendBulkWireTransferSize — the same SoHeader/ECN/DCQCN data path the
// collectives use (SendP2p carries no SoHeader, so DCQCN would be inert).
// Offered/delivered bytes are counted only inside the measurement window
// [warmup, warmup+measure); the sim stops at the window end.
// ---------------------------------------------------------------------------
static bool g_randTrafficRan = false;
static uint64_t g_randWindowStartNs = 0;
static uint64_t g_randWindowEndNs = 0;
static uint64_t g_randOfferedBytes = 0;
static uint64_t g_randDeliveredBytes = 0;
static uint64_t g_randMsgsSent = 0;

static void
RandTrafficReceive(uint16_t /*rank*/, Ptr<Packet> packet, FabricHeader header)
{
    uint64_t now = Simulator::Now().GetNanoSeconds();
    if (now < g_randWindowStartNs || now >= g_randWindowEndNs)
    {
        return;
    }
    // A PERMANENT_LOSS callback is a terminal-loss NOTICE (a synthetic empty
    // packet carrying the lost chunk's metadata), not a delivery: counting
    // its effectiveDataSize as delivered goodput reported 1363.149 Gbps on a
    // 2-node / 4-GPU / leafspine / drop-threshold-1 / retry-0 run whose true
    // payload goodput is 298.844 Gbps — 203 of 260 counted "messages" were
    // loss notices (codex round 24, finding 3).
    if (header.GetPacketType() == FabricPacketType::PERMANENT_LOSS)
    {
        return;
    }
    uint32_t bytes = header.GetEffectiveDataSize();
    if (bytes == 0 && packet)
    {
        bytes = packet->GetSize();
    }
    g_randDeliveredBytes += bytes;
}

static void
RandTrafficSend(Ptr<FabricEndpoint> ep,
                std::vector<uint16_t> remoteRanks,
                Ptr<UniformRandomVariable> destPick,
                uint32_t msgBytes, uint64_t intervalNs, uint8_t protocolId)
{
    uint64_t now = Simulator::Now().GetNanoSeconds();
    if (now >= g_randWindowEndNs)
    {
        return;
    }
    uint16_t dest = remoteRanks[destPick->GetInteger(0, remoteRanks.size() - 1)];
    if (now >= g_randWindowStartNs)
    {
        g_randOfferedBytes += msgBytes;
        g_randMsgsSent++;
    }
    ep->SendBulkWireTransferSize(dest, msgBytes, protocolId, ep->GetRank(), 0);
    Simulator::Schedule(NanoSeconds(intervalNs), &RandTrafficSend, ep,
                        remoteRanks, destPick, msgBytes, intervalNs, protocolId);
}

int
main(int argc, char* argv[])
{
    std::string topology = "ring";
    std::string collective = "allreduce";
    std::string algorithm = "ring";
    std::string protocolModelType = "ns3::NcclProtocolModel";
    std::string fabricTypeStr = "NVLink";
    std::string deviceTypeStr = "GPU";
    uint32_t numGpus = 4;
    uint64_t dataSize = 1048576;
    uint64_t bandwidthGbps = 800;
    uint64_t latencyNs = 500;
    uint64_t startupDelayNs = 0;
    uint64_t startupLLNs = 65000;
    uint64_t startupLL128Ns = 65000;
    uint64_t startupSIMPLENs = 65000;
    uint64_t startupNVLSNs = 23000;
    uint64_t startupPerGpuNs = 0;
    uint64_t startupAlltoAllNs = 0;
    uint64_t startupAllGatherNs = 0;
    uint64_t localComputeDelayNs = 0;
    uint64_t interNodeComputeDelayNs = 0;
    uint64_t localComputePerByteNs = 0;
    uint64_t interNodeComputePerByteNs = 0;
    uint64_t localComputeBaseLLNs = 0;
    uint64_t localComputeBaseLL128Ns = 0;
    uint64_t localComputeBaseSIMPLENs = 0;
    uint64_t interNodeComputeBaseLLNs = 0;
    uint64_t interNodeComputeBaseLL128Ns = 0;
    uint64_t interNodeComputeBaseSIMPLENs = 0;
    uint64_t swOverheadPerByteNs = 0;
    uint64_t computeBaseLLNs = 0;
    uint64_t computeBaseLL128Ns = 0;
    uint64_t computeBaseSIMPLENs = 0;
    uint64_t interNodeBwRampNs = 0;
    uint64_t interNodeBwRampThreshold = 0;
    uint64_t fullmeshPerStepLLNs = 0;
    uint64_t fullmeshPerStepLL128Ns = 0;
    uint64_t fullmeshPerStepSIMPLENs = 0;
    uint64_t interNodeStartupLLNs = 0;
    uint64_t interNodeStartupLL128Ns = 0;
    uint64_t interNodeStartupSIMPLENs = 0;
    uint64_t localStartupLLNs = 0;
    uint64_t localStartupLL128Ns = 0;
    uint64_t localStartupSIMPLENs = 0;
    uint64_t llThreshold = 8192;
    uint64_t ll128Threshold = 2 * 1024 * 1024;
    uint32_t linksPerGpu = 1;
    uint64_t linkDataRateGbps = 0;  // 0=use bandwidth (default); separate from endpoint effective BW for switched topology
    uint32_t fattreeRadix = 4;
    uint32_t railCount = 8;
    uint32_t railNodesPerLeaf = 32;
    uint32_t railSpineSwitches = 0;
    uint32_t railCoreSwitches = 0;
    uint32_t railLinksPerSpine = 0;
    uint32_t leafSwitches = 6;
    uint32_t spineSwitches = 3;
    uint32_t nvl72SwitchPlanes = 18;
    uint32_t nvl72GpusPerGroup = 0;
    uint32_t torusDimX = 4;
    uint32_t torusDimY = 4;
    uint32_t torusDimZ = 4;
    uint32_t meshRows = 4;
    uint32_t meshCols = 4;
    uint32_t hypercubeDims = 4;
    uint32_t rackRows = 8;
    uint32_t rackCols = 8;
    uint32_t numRacks = 1;
    uint32_t numSpineSwitchesUb = 0;
    // Phase A.4: new topology parameters
    uint32_t dflyGroups = 4;
    uint32_t dflyRoutersPerGroup = 2;
    uint32_t multiPlaneCount = 4;
    uint32_t hier3MidSwitches = 2;
    uint64_t syncMemLatencyNs = 0;
    uint64_t asyncMemLatencyNs = 0;
    bool llrEnabled = false;
    uint64_t vcCreditsBytes = 104857600;
    uint64_t vcBufferSizeBytes = 0;   // Phase A.2: credits = vcBufferSizeBytes / sprayChunkSize
    uint32_t sprayChunkSize = 131072;
    uint32_t bulkChunkSize = 8 * 1024 * 1024;
    uint32_t numLanes = 1;
    uint32_t switchVoqDepth = 10000;
    uint64_t switchArbIntervalNs = 100;
    uint64_t switchCutThroughDelayNs = 200;
    std::string arbiterType = "roundrobin"; // crossbar arbitration strategy
    uint64_t sharpAggregationDelayNs = 500;
    uint32_t nvlsNumPartitions = 8;
    uint64_t segmentSizeBytes = 524288;
    uint64_t perStepSwOverheadNs = 0;
    uint16_t rootRank = 0;
    uint32_t forceProtocol = 0;  // 0=auto, 1=LL, 2=LL128, 3=SIMPLE
    double protocolWireEfficiency = 1.0;
    bool useTransactionModel = true;
    bool verbose = false;

    // Optical reliability parameters
    double ber = 0.0;
    double berIntraNodeElectrical = 0.0;
    double berIntraRackElectrical = 0.0;
    double berInterRackElectrical = 0.0;
    double berInterRackOptical = 0.0;
    std::string interSwitchMedium = "electrical";
    bool failureAwareRouting = false;
    int32_t failedOpticalLink = -1;
    uint32_t opticalInterSwitchLinks = 0;
    uint32_t operationalOpticalInterSwitchLinks = 0;
    double operationalOpticalBerMin = 0.0;
    double operationalOpticalBerMax = 0.0;
    bool failureRoutesComplete = true;
    uint64_t failureUnreachableGpuPairs = 0;
    double failedOpticalLinkBer = 0.0;
    std::string failedOpticalLinkDescription;
    double packetLossRate = 0.0;
    std::string errorModeStr = "independent";  // independent/burst
    uint32_t codewordSize = 0;
    uint32_t burstLength = 1;
    double burstArrivalRate = 0.0;
    uint32_t fecN = 0;
    uint32_t fecK = 0;
    uint32_t fecT = 0;
    uint64_t fecEncodeLatencyNs = 50;
    uint64_t fecDecodeLatencyNs = 80;
    bool fecEnabled = true;
    std::string fecScope = "all";
    std::string llrModeStr = "gobackn";  // gobackn/sack
    uint32_t llrRetryLimit = UINT32_MAX;  // unlimited retries — real NVLink retries until success
    uint32_t llrBufferSize = 1000;
    uint64_t llrBufferSizeBytes = 0;  // Phase A.2: packets = llrBufferSizeBytes / sprayChunkSize
    std::string llrOverflowPolicyStr = "dropoldest";  // dropoldest/dropnewest
    uint64_t llrRetryTimeoutNs = 0;
    bool llrTimeoutRetransmit = false;  // opt-in sender-side timeout retransmit backstop
    uint32_t soTxWindow = 0;  // opt-in So bulk TX window in chunks (perftest tx_depth analog; 0=unbounded)
    bool soGbnStrict = false; // opt-in strict So go-back-N receive semantics (in-order-only + one NAK/gap)
    uint64_t llrReloadBandwidthGBps = 4800;
    uint64_t llrReloadLatencyNs = 300;




    // Contention model parameters (WFQ: collective vs memory vs P2P traffic)
    double contentionCollectiveWeight = 0.7;
    double contentionMemoryWeight = 0.2;
    double contentionP2pWeight = 0.1;

    // Multi-node parameters
    uint32_t numNodes = 1;
    uint32_t gpusPerNode = 8;
    std::string interNodeTopology = "fullmesh";
    uint32_t interNodeSpines = 0;
    uint32_t interNodeTorusWidth = 0;
    uint32_t interNodeDragonflyGroupSize = 0;
    uint64_t interNodeBandwidthGbps = 400;
    uint64_t interNodeLatencyNs = 10000;
    std::string interNodeFabricTypeStr = "RoCE";
    uint64_t interNodeStartupNs = 40000;
    // Ethernet (RoCEv2) switch ECN threshold: VOQ depth (packets) at which the
    // switch marks the inner SoHeader ECN bit on enqueue. 0 disables marking.
    // The ECN reaction (CNP + DCQCN rate-backoff) is gated by --dcqcn (default
    // off = parity-safe); ECN marking itself is live (Phase B): the switch
    // marks SoHeader ECN at this VOQ depth on enqueue.
    uint32_t ecnThreshold = 8;
    // Plain-Ethernet lossy VOQ tail-drop threshold (pkts); 0=off. Must be <
    // NvSwitch::VoqDepth (the ETHERNET branch raises that to 1024) so the
    // NACK path fires before the base hard-cap drop. Default 8 = the
    // EthernetSwitch Attribute default (parity-neutral).
    uint32_t voqDropThreshold = 8;
    // PFC (IEEE 802.1Qbb-style) priority flow control on the So switch
    // (opt-in, default off = byte-identical legacy). mode 1 = static pause
    // threshold, 2 = dynamic (HPCC SwitchMmu-style headroom-scaled). The
    // intended threshold ordering when all three arms are armed:
    // ecnThreshold < pfcPauseThreshold < voqDropThreshold (mark first, pause
    // as the no-drop backstop, drop never — with PFC holding the queue below
    // the drop threshold the lossy path should stay unfired).
    uint32_t switchPfcMode = 0;
    uint32_t pfcPauseThreshold = 64;
    uint32_t pfcResumeOffset = 8;
    uint32_t pfcPauseTimeUs = 0;
    // DCQCN rate machine (Phase B). Off by default: with no DcqcnRateMachine
    // mounted, CNPs are no-ops and SendPacketOnDevice adds zero shaping delay,
    // so all So runs stay bit-identical to the Phase-A baseline. On, a shared
    // DcqcnRateMachine is mounted on every So (inter-node) endpoint; the switch
    // ECN path stamps SoHeader ECN, receivers emit CNPs, and the sender's
    // ProcessCnpPacket drives the per-flow Alpha rate-backoff folded into the
    // single SendPacketOnDevice egress chokepoint.
    bool enableDcqcn = false;
    // Opt-in per-QP sustained-throughput ceiling (Gbps; 0 = off). Mounts the
    // DCQCN rate machine (its true per-flow pacer enforces the cap) even when
    // --dcqcn is off. Calibrated to the ~49 Gbps single-QP ib_write_bw ceiling.
    double perQpCeilingGbps = 0.0;
    // DCQCN rate-machine knobs (0/negative = keep the TypeId default; set
    // via SetAttribute only when the user overrides, so the default path is
    // byte-identical).
    uint64_t dcqcnTFastNs = 0;        // 0 = default 50000 ns
    uint32_t dcqcnFastCycles = 0;     // 0 = default 5
    uint64_t dcqcnTAiNs = 0;          // 0 = default 100000 ns
    double dcqcnRai = 0.0;            // 0 = default 0.01
    double dcqcnGInit = 0.0;          // 0 = default 0.5
    double dcqcnGDecay = 0.0;         // 0 = default 0.5
    double dcqcnAlphaFloor = 0.0;     // 0 = default 0.0 (negative = no-op)
    // RTTCC (RTT-probe-based congestion control, NVIDIA RTT-CC / Swift
    // style). Off by default: no RttccRateMachine mounted => no probes, no
    // pacing, byte-identical baseline. On, each So endpoint mounts its OWN
    // machine (the flow key carries no source rank, so a shared machine
    // would merge per-flow state across incast senders). Fabric-agnostic:
    // unlike DCQCN it needs no ECN/CNP loop, so it is the congestion-control
    // option for the plain lossy Ethernet fabric (and also runs on RoCE).
    bool enableRttcc = false;
    uint64_t rttccTargetRttNs = 0;      // 0 = machine default (10000 ns)
    uint64_t rttccProbeIntervalNs = 0;  // 0 = machine default (5000 ns)
    std::string intraNodeTopology = "switched";
    std::string intraNodeAlgorithm = "fullmesh";


    CommandLine cmd;
    cmd.AddValue("topology", "Topology: ring/fullmesh/switched/fattree/railfattree/leafspine/nvl72/torus/mesh/hypercube/2dfullmesh/ndfullmesh/2dfullmeshclos", topology);
    cmd.AddValue("collective", "Collective: allreduce/alltoall/allgather/reducescatter/broadcast/reduce/p2p/fanout/incast", collective);
    cmd.AddValue("algorithm", "Algorithm: ring/sharp/nvls/tree/fullmesh/collnetdirect/collnetchain/hierarchical/ncclring", algorithm);
    cmd.AddValue("protocolModel", "Protocol model type: ns3::NcclProtocolModel/ns3::UbProtocolModel/ns3::McclProtocolModel", protocolModelType);
    cmd.AddValue("protocolWireEfficiency", "Protocol payload bytes divided by transmitted bytes", protocolWireEfficiency);
    cmd.AddValue("fabricType", "Fabric type: NVLink/ETH/ICI/HCCS/xGMI/MetaXLink/RoCE/UB", fabricTypeStr);
    cmd.AddValue("deviceType", "Device type: GPU/CPU/TPU/NPU/Gaudi/UB_NPU", deviceTypeStr);
    cmd.AddValue("numGpus", "Number of GPUs", numGpus);
    cmd.AddValue("dataSize", "Data size in bytes", dataSize);
    cmd.AddValue("bandwidth", "Per-link bandwidth in Gbps (effective, not spec; see hardware JSON specBandwidthGBps vs perLinkEffectiveBandwidthGBps)", bandwidthGbps);
    cmd.AddValue("latency", "Per-link latency in nanoseconds", latencyNs);
    cmd.AddValue("startupDelay", "Startup delay override in nanoseconds (0=auto)", startupDelayNs);
    cmd.AddValue("startupLL", "LL protocol startup delay in nanoseconds", startupLLNs);
    cmd.AddValue("startupLL128", "LL128 protocol startup delay in nanoseconds", startupLL128Ns);
    cmd.AddValue("startupSIMPLE", "SIMPLE protocol startup delay in nanoseconds", startupSIMPLENs);
    cmd.AddValue("startupNVLS", "NVLS protocol startup delay in nanoseconds", startupNVLSNs);
    cmd.AddValue("startupAlltoAll", "AlltoAll-specific startup delay override in nanoseconds (0=use protocol startup)", startupAlltoAllNs);
    cmd.AddValue("startupAllGather", "AllGather-specific startup delay override in nanoseconds (0=use protocol startup)", startupAllGatherNs);
    cmd.AddValue("localComputeDelay", "Local reduce-scatter compute delay in nanoseconds", localComputeDelayNs);
    cmd.AddValue("interNodeComputeDelay", "Inter-node reduce-scatter compute delay in nanoseconds", interNodeComputeDelayNs);
    cmd.AddValue("localComputePerByte", "Per-byte compute rate for local reduce (ns/B, 0=flat delay)", localComputePerByteNs);
    cmd.AddValue("interNodeComputePerByte", "Per-byte compute rate for inter-node reduce (ns/B, 0=flat delay)", interNodeComputePerByteNs);
    cmd.AddValue("localComputeBaseLL", "Base compute delay for LL protocol in local reduce (ns)", localComputeBaseLLNs);
    cmd.AddValue("localComputeBaseLL128", "Base compute delay for LL128 protocol in local reduce (ns)", localComputeBaseLL128Ns);
    cmd.AddValue("localComputeBaseSIMPLE", "Base compute delay for SIMPLE protocol in local reduce (ns)", localComputeBaseSIMPLENs);
    cmd.AddValue("interNodeComputeBaseLL", "Base compute delay for LL protocol in inter-node reduce (ns)", interNodeComputeBaseLLNs);
    cmd.AddValue("interNodeComputeBaseLL128", "Base compute delay for LL128 protocol in inter-node reduce (ns)", interNodeComputeBaseLL128Ns);
    cmd.AddValue("interNodeComputeBaseSIMPLE", "Base compute delay for SIMPLE protocol in inter-node reduce (ns)", interNodeComputeBaseSIMPLENs);
    cmd.AddValue("interNodeBwRamp", "Inter-node BW ramp-up max delay in nanoseconds (decays with chunk size)", interNodeBwRampNs);
    cmd.AddValue("interNodeBwRampThreshold", "Inter-node BW ramp-up threshold in bytes", interNodeBwRampThreshold);
    cmd.AddValue("fullmeshPerStepLL", "Per-step delay for LL protocol in fullmesh local phases (ns)", fullmeshPerStepLLNs);
    cmd.AddValue("fullmeshPerStepLL128", "Per-step delay for LL128 protocol in fullmesh local phases (ns)", fullmeshPerStepLL128Ns);
    cmd.AddValue("fullmeshPerStepSIMPLE", "Per-step delay for SIMPLE protocol in fullmesh local phases (ns)", fullmeshPerStepSIMPLENs);
    cmd.AddValue("interNodeStartupLL", "Protocol-aware inter-node startup delay for LL (ns)", interNodeStartupLLNs);
    cmd.AddValue("interNodeStartupLL128", "Protocol-aware inter-node startup delay for LL128 (ns)", interNodeStartupLL128Ns);
    cmd.AddValue("interNodeStartupSIMPLE", "Protocol-aware inter-node startup delay for SIMPLE (ns)", interNodeStartupSIMPLENs);
    cmd.AddValue("localStartupLL", "Protocol-aware local phase startup delay for LL (ns)", localStartupLLNs);
    cmd.AddValue("localStartupLL128", "Protocol-aware local phase startup delay for LL128 (ns)", localStartupLL128Ns);
    cmd.AddValue("localStartupSIMPLE", "Protocol-aware local phase startup delay for SIMPLE (ns)", localStartupSIMPLENs);
    cmd.AddValue("startupPerGpuNs", "Per-GPU scaling factor for startup delay in nanoseconds", startupPerGpuNs);
    cmd.AddValue("llThreshold", "LL→LL128 threshold in bytes", llThreshold);
    cmd.AddValue("ll128Threshold", "LL128→SIMPLE threshold in bytes", ll128Threshold);
    cmd.AddValue("linksPerGpu", "Links per GPU to switch (switched/leafspine)", linksPerGpu);
    cmd.AddValue("linkDataRate", "Per-link PointToPoint data rate in Gbps (0=use bandwidth; set to hardware per-link BW for switched topology)", linkDataRateGbps);
    cmd.AddValue("fattreeRadix", "Fat-tree switch radix", fattreeRadix);
    cmd.AddValue("railCount", "GPU ranks and rails per enclosure", railCount);
    cmd.AddValue("railNodesPerLeaf", "Enclosures served by each rail leaf", railNodesPerLeaf);
    cmd.AddValue("railSpineSwitches", "Spine switches in the rail-optimized fat tree (0=derive)", railSpineSwitches);
    cmd.AddValue("railCoreSwitches", "Core switches in the rail-optimized fat tree (0=derive)", railCoreSwitches);
    cmd.AddValue("railLinksPerSpine", "Parallel links from each rail leaf to each spine (0=derive)", railLinksPerSpine);
    cmd.AddValue("leafSwitches", "Number of leaf switches (leafspine)", leafSwitches);
    cmd.AddValue("spineSwitches", "Number of spine switches (leafspine)", spineSwitches);
    cmd.AddValue("nvl72SwitchPlanes", "Number of NVSwitch ASIC planes (nvl72, default 18)", nvl72SwitchPlanes);
    cmd.AddValue("nvl72GpusPerGroup", "GPUs per hierarchical group (nvl72, 0=auto numGpus/6)", nvl72GpusPerGroup);
    cmd.AddValue("torusDimX", "Torus X dimension size", torusDimX);
    cmd.AddValue("torusDimY", "Torus Y dimension size", torusDimY);
    cmd.AddValue("torusDimZ", "Torus Z dimension size", torusDimZ);
    cmd.AddValue("meshRows", "Mesh rows", meshRows);
    cmd.AddValue("meshCols", "Mesh columns", meshCols);
    cmd.AddValue("hypercubeDims", "Hypercube dimensions (2^dims nodes)", hypercubeDims);
    cmd.AddValue("rackRows", "UB-Mesh rack rows (2dfullmesh)", rackRows);
    cmd.AddValue("rackCols", "UB-Mesh rack columns (2dfullmesh)", rackCols);
    cmd.AddValue("numRacks", "Number of racks (2dfullmeshclos)", numRacks);
    cmd.AddValue("numSpineSwitchesUb", "Number of spine switches for UB inter-rack", numSpineSwitchesUb);
    cmd.AddValue("dflyGroups", "Number of dragonfly+ groups (dragonfly+ topology)", dflyGroups);
    cmd.AddValue("dflyRoutersPerGroup", "Leaf routers per dragonfly+ group", dflyRoutersPerGroup);
    cmd.AddValue("multiPlaneCount", "Number of independent switch planes (multiplane topology)", multiPlaneCount);
    cmd.AddValue("hier3MidSwitches", "Mid-tier switches (3levelhierarchical topology)", hier3MidSwitches);
    cmd.AddValue("syncMemLatencyNs", "Sync load/store latency override (0=use protocol default)", syncMemLatencyNs);
    cmd.AddValue("asyncMemLatencyNs", "Async URMA latency override (0=use protocol default)", asyncMemLatencyNs);
    cmd.AddValue("llrEnabled", "Enable link-level retry (0=disabled, 1=enabled)", llrEnabled);

    // Optical reliability CLI flags
    cmd.AddValue("ber", "Bit error rate (0=no errors)", ber);
    cmd.AddValue("berIntraNodeElectrical",
                 "BER for intra-node electrical links (NVLink within a node). "
                 "0 = fall back to --ber.",
                 berIntraNodeElectrical);
    cmd.AddValue("berIntraRackElectrical",
                 "BER for intra-rack electrical links (GPU-to-switch within rack). "
                 "0 = fall back to --ber.",
                 berIntraRackElectrical);
    cmd.AddValue("berInterRackElectrical",
                 "BER for inter-rack electrical links (switch-to-switch). "
                 "0 = fall back to --ber.",
                 berInterRackElectrical);
    cmd.AddValue("berInterRackOptical",
                 "BER for inter-rack optical links (AOC/parallel fiber). "
                 "0 = fall back to --ber.",
                 berInterRackOptical);
    cmd.AddValue("interSwitchMedium",
                 "Medium for links between switch tiers: electrical/optical",
                 interSwitchMedium);
    cmd.AddValue("failedOpticalLink",
                 "Permanently remove this optical inter-switch link index (-1=none)",
                 failedOpticalLink);
    cmd.AddValue("failureAwareRouting",
                 "Recompute equal-cost switch routes over operational links",
                 failureAwareRouting);
    cmd.AddValue("packetLossRate", "Packet loss rate (0=no drops)", packetLossRate);
    cmd.AddValue("errorMode", "Error mode: independent/burst", errorModeStr);
    cmd.AddValue("codewordSize", "Codeword size in bytes for burst mode", codewordSize);
    cmd.AddValue("burstLength", "Burst length in codewords", burstLength);
    cmd.AddValue("burstArrivalRate", "Burst arrival rate per codeword", burstArrivalRate);
    cmd.AddValue("fecN", "FEC codeword length N (0=no FEC)", fecN);
    cmd.AddValue("fecK", "FEC data length K", fecK);
    cmd.AddValue("fecT", "FEC correction capability T", fecT);
    cmd.AddValue("fecEncodeLatency", "FEC encode latency in ns", fecEncodeLatencyNs);
    cmd.AddValue("fecDecodeLatency", "FEC decode latency in ns", fecDecodeLatencyNs);
    cmd.AddValue("fecEnabled", "FEC enabled (0=disabled even if N>0)", fecEnabled);
    cmd.AddValue("fecScope", "FEC scope: all/optical", fecScope);
    cmd.AddValue("llrMode", "LLR mode: gobackn/sack", llrModeStr);
    cmd.AddValue("llrRetryLimit", "LLR retry limit", llrRetryLimit);
    cmd.AddValue("llrBufferSize", "LLR buffer size in packets", llrBufferSize);
    cmd.AddValue("llrBufferSizeBytes",
                 "LLR buffer size in bytes. When >0, packets = llrBufferSizeBytes / sprayChunkSize "
                 "(overrides --llrBufferSize). 0 = use --llrBufferSize.",
                 llrBufferSizeBytes);
    cmd.AddValue("llrOverflowPolicy", "LLR overflow policy: dropoldest/dropnewest", llrOverflowPolicyStr);
    cmd.AddValue("llrRetryTimeout", "LLR retry timeout in ns (0=auto)", llrRetryTimeoutNs);
    cmd.AddValue("llrTimeoutRetransmit",
                 "Retransmit un-ACKed LLR entries when their retry timer fires (sender-side "
                 "backstop; required for llrMode=sack under random loss where a lost NACK "
                 "otherwise deadlocks the flow). Default 0 = timer fires but does nothing "
                 "(legacy behavior, parity-safe).",
                 llrTimeoutRetransmit);
    cmd.AddValue("soTxWindow",
                 "Bound the number of in-flight So bulk chunks per endpoint "
                 "(NIC send-queue depth, perftest tx_depth analog; ACK-clocked "
                 "via LLR per-packet ACKs, so it requires --llrEnabled). "
                 "Default 0 = legacy unbounded synchronous dispatch.",
                 soTxWindow);
    cmd.AddValue("soGbnStrict",
                 "Strict go-back-N receive semantics for So flows in "
                 "llrMode=gobackn: in-order-only acceptance, out-of-order "
                 "arrivals discarded (no ACK), one NAK per gap (IB responder "
                 "behavior). Su (NVLink) traffic is never affected. "
                 "Default false = legacy dedup that keeps OOO originals.",
                 soGbnStrict);
    cmd.AddValue("llrReloadBandwidth",
                 "Backing-source bandwidth in GB/s for retry-buffer misses",
                 llrReloadBandwidthGBps);
    cmd.AddValue("llrReloadLatency",
                 "Backing-source access latency in ns for retry-buffer misses",
                 llrReloadLatencyNs);




    cmd.AddValue("vcCredits", "VC credits in bytes (legacy: 1 credit = 1 byte)", vcCreditsBytes);
    cmd.AddValue("vcBufferSizeBytes",
                 "VC buffer size in bytes. When >0, credits = vcBufferSizeBytes / sprayChunkSize "
                 "(overrides --vcCredits). 0 = use --vcCredits.",
                 vcBufferSizeBytes);
    cmd.AddValue("sprayChunkSize", "Spray chunk size in bytes", sprayChunkSize);
    cmd.AddValue("bulkChunkSize", "Maximum packet size for bulk transfers", bulkChunkSize);
    cmd.AddValue("numLanes", "Number of physical lanes per logical link (1=no sub-lanes, 6=NVLink)", numLanes);
    cmd.AddValue("switchVoqDepth", "NVSwitch VOQ depth", switchVoqDepth);
    cmd.AddValue("switchArbInterval", "NVSwitch arbitration interval in ns", switchArbIntervalNs);
    cmd.AddValue("switchCutThroughDelay", "NVSwitch cut-through forwarding delay in ns (0=store-and-forward)", switchCutThroughDelayNs);
    cmd.AddValue("arbiter", "Crossbar arbitration strategy: roundrobin (default) or an ns3::Arbiter TypeId string", arbiterType);
    cmd.AddValue("sharpAggregationDelay", "SHARP in-switch reduction latency in ns", sharpAggregationDelayNs);
    cmd.AddValue("nvlsNumPartitions", "Number of partitions for pipelined NVLS AllReduce multicast (0=single, 8=pipelined)", nvlsNumPartitions);
    cmd.AddValue("segmentSize", "AlltoAll segment size in bytes", segmentSizeBytes);
    cmd.AddValue("perStepSwOverhead", "Software overhead per ring step in ns (ring collectives)", perStepSwOverheadNs);
    cmd.AddValue("swOverheadPerByte", "Per-byte software overhead for AllReduce per-step (ns/B, 0=flat delay)", swOverheadPerByteNs);
    cmd.AddValue("computeBaseLL", "Base compute delay for LL protocol in ring AllReduce per-step (ns)", computeBaseLLNs);
    cmd.AddValue("computeBaseLL128", "Base compute delay for LL128 protocol in ring AllReduce per-step (ns)", computeBaseLL128Ns);
    cmd.AddValue("computeBaseSIMPLE", "Base compute delay for SIMPLE protocol in ring AllReduce per-step (ns)", computeBaseSIMPLENs);
    cmd.AddValue("forceProtocol", "Force protocol: 0=auto, 1=LL, 2=LL128, 3=SIMPLE", forceProtocol);
    cmd.AddValue("useTransactionModel",
                 "Use event-driven protocol transactions for supported collectives",
                 useTransactionModel);
    cmd.AddValue("rootRank", "Root rank for broadcast/reduce", rootRank);
    cmd.AddValue("verbose", "Enable verbose logging", verbose);
    cmd.AddValue("contentionCollectiveWeight", "WFQ weight for collective traffic", contentionCollectiveWeight);
    cmd.AddValue("contentionMemoryWeight", "WFQ weight for memory traffic", contentionMemoryWeight);
    cmd.AddValue("contentionP2pWeight", "WFQ weight for P2P traffic", contentionP2pWeight);

    // Background P2P interferer: a self-driven P2pTraffic flow scheduled at
    // bgP2pStartTimeNs that shares the fabric with the primary collective,
    // exercising the WFQ contention model (multi-class numActive>1) and the
    // So switch VOQ arbitration. Default off (parity-identical). The flow is
    // unlimited (maxPackets=0); the sim stops when the collective completes.
    bool bgP2pEnabled = false;
    uint32_t bgP2pSrc = 0;
    uint32_t bgP2pDst = 1;
    uint64_t bgP2pSizeBytes = 1048576;   // 1 MiB
    uint64_t bgP2pIntervalNs = 0;        // 0 = back-to-back
    uint64_t bgP2pStartTimeNs = 0;       // absolute sim time (ns)
    cmd.AddValue("bgP2pEnabled", "Inject a background P2P flow as a contention interferer", bgP2pEnabled);
    cmd.AddValue("bgP2pSrc", "Background P2P source rank", bgP2pSrc);
    cmd.AddValue("bgP2pDst", "Background P2P destination rank", bgP2pDst);
    cmd.AddValue("bgP2pSizeBytes", "Background P2P packet size (bytes)", bgP2pSizeBytes);
    cmd.AddValue("bgP2pIntervalNs", "Background P2P send interval (0=back-to-back)", bgP2pIntervalNs);
    cmd.AddValue("bgP2pStartTimeNs", "Background P2P start time (ns from sim start)", bgP2pStartTimeNs);
    // Random-traffic mode: replaces the collective injector with fixed-duration
    // uniform-random cross-node bulk traffic (see the RandTrafficSend block
    // above main). Default off (parity-identical). Injection rate is the
    // per-source offered load as a fraction of the inter-node line rate.
    bool randTrafficEnabled = false;
    double randInjectionRate = 0.5;
    uint32_t randMsgBytes = 65536;
    uint64_t randWarmupNs = 50000;
    uint64_t randMeasureNs = 200000;
    cmd.AddValue("randTraffic", "Enable uniform-random cross-node traffic mode (replaces the collective; fixed-duration run)", randTrafficEnabled);
    cmd.AddValue("randInjectionRate", "Per-source offered load as a fraction of the inter-node line rate (0,1]", randInjectionRate);
    cmd.AddValue("randMsgBytes", "Random-traffic message size in bytes", randMsgBytes);
    cmd.AddValue("randWarmupNs", "Random-traffic warmup before the measurement window (ns)", randWarmupNs);
    cmd.AddValue("randMeasureNs", "Random-traffic measurement window length (ns); the sim stops at warmup+measure", randMeasureNs);
    cmd.AddValue("numNodes", "Number of GPU nodes (1=single-node, 2+=multi-node)", numNodes);
    cmd.AddValue("gpusPerNode", "GPUs per node (multi-node mode)", gpusPerNode);
    cmd.AddValue("interNodeTopology", "Inter-node topology: fullmesh/ring/host/tor/rail/leafspine/torus2d/dragonfly (tor = single shared leaf, 1 hop, ib_write_lat style; rail = one switch per local rank; leafspine = 2-tier Clos; torus2d/dragonfly = GPU-0-gateway direct topologies)", interNodeTopology);
    cmd.AddValue("interNodeSpines", "leafspine: spine switch count (0 = auto: gpusPerNode, non-blocking)", interNodeSpines);
    cmd.AddValue("interNodeTorusWidth", "torus2d: grid width (0 = auto: largest divisor <= sqrt(numNodes))", interNodeTorusWidth);
    cmd.AddValue("interNodeDragonflyGroupSize", "dragonfly: nodes per group (0 = auto: divisor closest to sqrt(numNodes))", interNodeDragonflyGroupSize);
    cmd.AddValue("interNodeBandwidth", "Inter-node link bandwidth in Gbps", interNodeBandwidthGbps);
    cmd.AddValue("interNodeLatency", "Inter-node link latency in nanoseconds", interNodeLatencyNs);
    cmd.AddValue("interNodeFabricType", "Inter-node fabric type: RoCE/ETH/NVLink", interNodeFabricTypeStr);
    cmd.AddValue("interNodeStartup", "Inter-node phase startup delay in ns", interNodeStartupNs);
    cmd.AddValue("ecnThreshold", "Ethernet (RoCEv2) ECN threshold: VOQ depth (pkts) to mark SoHeader ECN (0=off; Phase-B reaction pending)", ecnThreshold);
    cmd.AddValue("voqDropThreshold", "Lossy VOQ tail-drop threshold (pkts); 0=off. Plain ETH: always applied (default 8). RoCE: an EXPLICIT flag opts into the lossy/no-PFC regime (unset = lossless). Must be < switchVoqDepth (default 10000); keep > ecnThreshold for mark-before-drop", voqDropThreshold);
    cmd.AddValue("switchPfcMode", "PFC (802.1Qbb-style) flow control on So switches: 0=off (default), 1=static pause threshold, 2=dynamic (HPCC SwitchMmu-style, scales with remaining VOQ headroom), 3=quanta (802.1Qbb pause-time semantics via --pfcPauseTime). Keep pfcPauseThreshold < voqDropThreshold so the pause fires before the tail-drop", switchPfcMode);
    cmd.AddValue("pfcPauseThreshold", "PFC: VOQ depth (pkts) at which the switch PAUSEs the ingress feeding the congested egress (default 64; inert when switchPfcMode=0)", pfcPauseThreshold);
    cmd.AddValue("pfcResumeOffset", "PFC resume hysteresis (pkts): the pause lifts once the VOQ drains to pfcPauseThreshold - offset (default 8; inert when switchPfcMode=0)", pfcResumeOffset);
    cmd.AddValue("pfcPauseTime", "PFC mode 3 only: 802.1Qbb pause time (us) carried by each PAUSE. The endpoint auto-expires after this and the switch re-asserts at pausetime/2 while congested (qbb-net-device semantics). 0 with modes 1/2 = pause until explicit RESUME (default)", pfcPauseTimeUs);
    cmd.AddValue("dcqcn", "Enable DCQCN rate machine on So endpoints (Phase B; off=parity-safe baseline)", enableDcqcn);
    cmd.AddValue("perQpCeilingGbps", "Per-QP sustained throughput ceiling in Gbps (0=off; ~49 = single-QP ib_write_bw)", perQpCeilingGbps);
    cmd.AddValue("dcqcnTFastNs", "DCQCN fast-recovery timer interval in ns (0=default 50000)", dcqcnTFastNs);
    cmd.AddValue("dcqcnFastCycles", "DCQCN fast-recovery cycles before additive increase (0=default 5)", dcqcnFastCycles);
    cmd.AddValue("dcqcnTAiNs", "DCQCN additive-increase timer interval in ns (0=default 100000)", dcqcnTAiNs);
    cmd.AddValue("dcqcnRai", "DCQCN additive Alpha decrement per AI tick (0=default 0.01)", dcqcnRai);
    cmd.AddValue("dcqcnGInit", "DCQCN initial multiplicative step g on first CNP (0=default 0.5)", dcqcnGInit);
    cmd.AddValue("dcqcnGDecay", "DCQCN multiplicative decay of g per fast cycle (0=default 0.5)", dcqcnGDecay);
    cmd.AddValue("dcqcnAlphaFloor", "DCQCN Alpha floor, no shaping at/below (0=default 0.0)", dcqcnAlphaFloor);
    cmd.AddValue("rttcc", "Enable RTTCC (RTT-probe-based) congestion control on So endpoints (off=parity-safe baseline)", enableRttcc);
    cmd.AddValue("rttccTargetRttNs", "RTTCC target RTT in ns (0=default 10000); must exceed the base zero-queue RTT", rttccTargetRttNs);
    cmd.AddValue("rttccProbeIntervalNs", "RTTCC min spacing between RTT probes per flow in ns (0=default 5000)", rttccProbeIntervalNs);
    cmd.AddValue("intraNodeTopology", "Intra-node topology: switched/fullmesh", intraNodeTopology);
    cmd.AddValue("intraNodeAlgorithm", "Intra-node algorithm for hierarchical: ring/fullmesh", intraNodeAlgorithm);
    uint32_t ringChannels = 0;
    cmd.AddValue("ringChannels", "Channels (parallel rings) for ncclring algorithm, 0=auto(gpusPerNode)", ringChannels);
    std::string stepTraceFile = "";
    cmd.AddValue("stepTraceFile", "Path to write per-step completion CSV (gpu,step,startNs,completeNs,delayNs)", stepTraceFile);
    std::string protocolConfigPath = "";
    cmd.AddValue("protocolConfig",
                 "Path to a .cfg protocol config. When set, the [stack] profile drives the "
                 "PEX bundle (protocol/FEC/credits/LLR) and the [op] stencil drives the OTP "
                 "graph, replacing the hardcoded collective injector.",
                 protocolConfigPath);
    cmd.Parse(argc, argv);

    // Config-driven mode: load the .cfg + build the PEX bundle up front so the
    // BER fallback, protocol/FEC/payloadBuilder reassignment, and per-endpoint
    // credit/VC override below all see the profile's values. When the flag is
    // absent the validated hardcoded path is untouched.
    ProtocolConfig pconfig;
    ProtocolBundle pbundle;
    bool configMode = false;
    if (!protocolConfigPath.empty())
    {
        std::string cfgErr;
        if (!pconfig.Load(protocolConfigPath, &cfgErr))
        {
            NS_ABORT_MSG("could not load protocol config: " << cfgErr);
        }
        ProtocolProfile prof;
        if (pconfig.GetProfilePath().empty())
        {
            NS_ABORT_MSG("protocol config has no [stack] profile");
        }
        if (!prof.Load(pconfig.GetProfilePath()))
        {
            NS_ABORT_MSG("could not load profile: " << pconfig.GetProfilePath());
        }
        for (const auto& kv : pconfig.GetStackValues())
        {
            prof.Set(kv.first, kv.second);
        }
        pbundle = prof.Build();
        configMode = true;

        // Source the profile's PEX + fabric-hardware values into the same
        // local CLI variables the validated inline path consumes, so a `.cfg`
        // whose [op] declares `collective =` + `algorithm =` (topology =
        // optional) runs the calibrated inline injector with config-sourced
        // params -- bit-identical to the inline path run with the equivalent
        // CLI flags (same code, same values). numGpus/dataSize stay on the
        // CLI. Keys absent from the profile fall back to the CLI default
        // already in the variable. vcCredits/vcCount/flowControl are PEX-bundle
        // values consumed only by the stencil runner's ApplyBundle below; a
        // collective/algorithm op keeps the inline 1-VC fabric model (matching
        // the calibrated inline path), so they are intentionally NOT sourced
        // here.
        auto applyU64 = [&prof](uint64_t& v, const char* k) {
            const std::string s = prof.Get(k, "");
            if (!s.empty()) v = std::stoull(s);
        };
        auto applyU32 = [&prof](uint32_t& v, const char* k) {
            const std::string s = prof.Get(k, "");
            if (!s.empty()) v = static_cast<uint32_t>(std::stoull(s));
        };
        auto applyD = [&prof](double& v, const char* k) {
            const std::string s = prof.Get(k, "");
            if (!s.empty()) v = std::stod(s);
        };
        auto applyS = [&prof](std::string& v, const char* k) {
            const std::string s = prof.Get(k, "");
            if (!s.empty()) v = s;
        };
        // Fabric hardware.
        applyU64(bandwidthGbps, "bandwidthGbps");
        applyU64(latencyNs, "latencyNs");
        applyU32(numLanes, "numLanes");
        applyU32(linksPerGpu, "linksPerGpu");
        applyU32(sprayChunkSize, "sprayChunkSize");
        applyU32(switchVoqDepth, "switchVoqDepth");
        applyU64(switchArbIntervalNs, "switchArbIntervalNs");
        // Inter-node (So) fabric. Lets a `.cfg`/`.profile` select the So
        // fabric -- RoCE vs ETH -- plus its topology/hardware, so ONE config
        // adapts across protocols without --interNode* CLI flags (the same
        // config-primary philosophy as the intra-node axes above). Keys absent
        // fall back to the CLI default (backward-compatible: existing profiles
        // without these keys behave exactly as before). numNodes/gpusPerNode
        // stay on the CLI (scale axes, like numGpus/dataSize).
        applyS(interNodeFabricTypeStr, "interNodeFabricType");
        applyS(interNodeTopology, "interNodeTopology");
        applyU64(interNodeBandwidthGbps, "interNodeBandwidthGbps");
        applyU64(interNodeLatencyNs, "interNodeLatencyNs");
        // Protocol model + startup + thresholds.
        applyS(protocolModelType, "protocolModel");
        applyU32(forceProtocol, "forceProtocol");
        applyU64(startupLLNs, "StartupDelayLL");
        applyU64(startupLL128Ns, "StartupDelayLL128");
        applyU64(startupSIMPLENs, "StartupDelaySIMPLE");
        applyU64(startupNVLSNs, "StartupNVLS");
        applyU64(llThreshold, "LlThreshold");
        applyU64(ll128Threshold, "Ll128Threshold");
        // FEC.
        applyU32(fecN, "fecN");
        applyU32(fecK, "fecK");
        applyU32(fecT, "fecT");
        applyU64(fecEncodeLatencyNs, "fecEncodeLatencyNs");
        applyU64(fecDecodeLatencyNs, "fecDecodeLatencyNs");
        // Link BER.
        applyD(berIntraNodeElectrical, "berIntraNodeElectrical");
        applyD(berIntraRackElectrical, "berIntraRackElectrical");
        applyD(berInterRackOptical, "berInterRackOptical");
        // Link-level retry.
        {
            const std::string s = prof.Get("llrEnabled", "");
            if (!s.empty()) llrEnabled = (s == "1" || s == "true");
        }
        applyS(llrModeStr, "llrMode");
        // ApplyBundle now routes bundle.llrMode to the endpoints' LLR
        // managers (codex round 28, finding 4), so the bundle must carry
        // the RESOLVED mode (CLI --llrMode, overridden by the profile) or
        // it would clobber it with Build()'s default.
        pbundle.llrMode = llrModeStr;

        // A `.cfg` whose [op] declares `collective =` / `algorithm =` (and
        // optionally `topology =`) overrides the CLI values, then falls through
        // to the validated inline injector branches below -- bit-identical to
        // the inline path run with the equivalent CLI flags. A stencil .cfg
        // leaves these unset, so collective/algorithm/topology come from the
        // CLI exactly as before.
        {
            const std::string& cfgCollective = pconfig.GetCollective();
            if (!cfgCollective.empty()) collective = cfgCollective;
            const std::string& cfgAlgorithm = pconfig.GetAlgorithm();
            if (!cfgAlgorithm.empty()) algorithm = cfgAlgorithm;
            const std::string& cfgTopology = pconfig.GetTopology();
            if (!cfgTopology.empty()) topology = cfgTopology;
        }
    }

    if (topology == "railfattree")
    {
        NS_ABORT_MSG_IF(railCount == 0 || railNodesPerLeaf == 0 || numGpus % railCount != 0,
                        "railfattree requires a nonzero rail count, a nonzero leaf size, and divisible GPU count");
        const uint32_t enclosures = numGpus / railCount;
        const uint32_t leavesPerRail =
            (enclosures + railNodesPerLeaf - 1) / railNodesPerLeaf;
        const uint32_t totalLeaves = railCount * leavesPerRail;
        if (railSpineSwitches == 0)
        {
            if (totalLeaves <= 8) railSpineSwitches = 4;
            else if (totalLeaves <= 16) railSpineSwitches = 8;
            else if (totalLeaves <= 32) railSpineSwitches = 16;
            else if (totalLeaves <= 64) railSpineSwitches = 32;
            else railSpineSwitches = totalLeaves;
        }
        if (railCoreSwitches == 0 && totalLeaves > 64)
        {
            railCoreSwitches = totalLeaves / 2;
        }
        if (railLinksPerSpine == 0)
        {
            railLinksPerSpine = totalLeaves <= 64
                ? 32 / railSpineSwitches
                : 1;
        }
    }

    Config::SetDefault("ns3::RingAllReduce::UseTransactionModel",
                       BooleanValue(useTransactionModel));
    Config::SetDefault("ns3::SharpAllReduce::UseTransactionModel",
                       BooleanValue(useTransactionModel));

    // Phase A.2: derive effective VC credit count and LLR buffer packet count
    // from byte-based flags when set. credits = buffer_size / chunk_size.
    uint32_t effectiveVcCredits = static_cast<uint32_t>(vcCreditsBytes);
    if (vcBufferSizeBytes > 0 && sprayChunkSize > 0)
    {
        effectiveVcCredits = static_cast<uint32_t>(vcBufferSizeBytes / sprayChunkSize);
        if (effectiveVcCredits == 0) { effectiveVcCredits = 1; }  // min 1 credit
    }
    uint32_t effectiveLlrBufferSize = llrBufferSize;
    if (llrBufferSizeBytes > 0 && sprayChunkSize > 0)
    {
        effectiveLlrBufferSize = static_cast<uint32_t>(llrBufferSizeBytes / sprayChunkSize);
        if (effectiveLlrBufferSize == 0) { effectiveLlrBufferSize = 1; }
    }

    if (!stepTraceFile.empty())
    {
        CollectiveInjector::SetStepTraceFile(stepTraceFile);
    }

    if (collective != "allreduce" && collective != "alltoall" &&
        collective != "allgather" && collective != "reducescatter" &&
        collective != "broadcast" && collective != "reduce" &&
        collective != "p2p" && collective != "fanout" && collective != "incast")
    {
        NS_ABORT_MSG("Unknown collective: " << collective << ". Supported: allreduce, alltoall, allgather, reducescatter, broadcast, reduce, p2p, fanout, incast");
    }

    if (verbose)
    {
        LogComponentEnable("GpuClusterSim", LOG_LEVEL_INFO);
        LogComponentEnable("RingAllReduce", LOG_LEVEL_INFO);
        LogComponentEnable("SharpAllReduce", LOG_LEVEL_INFO);
        LogComponentEnable("NvlsAllGather", LOG_LEVEL_INFO);
        LogComponentEnable("TreeAllReduce", LOG_LEVEL_INFO);
        LogComponentEnable("RingAllGather", LOG_LEVEL_INFO);
        LogComponentEnable("RingReduceScatter", LOG_LEVEL_INFO);
        LogComponentEnable("RingBroadcast", LOG_LEVEL_INFO);
        LogComponentEnable("RingReduce", LOG_LEVEL_INFO);
        LogComponentEnable("AlltoAllInjector", LOG_LEVEL_INFO);
        LogComponentEnable("HierarchicalAllReduce", LOG_LEVEL_INFO);
        LogComponentEnable("HierarchicalAllGather", LOG_LEVEL_INFO);
        LogComponentEnable("HierarchicalReduceScatter", LOG_LEVEL_INFO);
    }

    // Configure protocol model
    FabricType fabricType = FabricTypeFromString(fabricTypeStr);
    DeviceType deviceType = DeviceTypeFromString(deviceTypeStr);

    // Configure protocol-specific startup delays
    if (protocolModelType == "ns3::NcclProtocolModel")
    {
        Config::SetDefault("ns3::NcclProtocolModel::StartupDelayLL", UintegerValue(startupLLNs));
        Config::SetDefault("ns3::NcclProtocolModel::StartupDelayLL128", UintegerValue(startupLL128Ns));
        Config::SetDefault("ns3::NcclProtocolModel::StartupDelaySIMPLE", UintegerValue(startupSIMPLENs));
        Config::SetDefault("ns3::NcclProtocolModel::LlThreshold", UintegerValue(llThreshold));
        Config::SetDefault("ns3::NcclProtocolModel::Ll128Threshold", UintegerValue(ll128Threshold));
        Config::SetDefault("ns3::NcclProtocolModel::SimpleWireEfficiency",
                           DoubleValue(protocolWireEfficiency));
        if (startupPerGpuNs > 0)
        {
            Config::SetDefault("ns3::NcclProtocolModel::PerGpuStartupDelayNs", UintegerValue(startupPerGpuNs));
        }
    }
    else if (protocolModelType == "ns3::McclProtocolModel")
    {
        Config::SetDefault("ns3::McclProtocolModel::StartupDelayNs", UintegerValue(startupSIMPLENs));
        Config::SetDefault("ns3::McclProtocolModel::WireEfficiency", DoubleValue(protocolWireEfficiency));
        if (startupPerGpuNs > 0)
        {
            Config::SetDefault("ns3::McclProtocolModel::PerGpuStartupDelayNs", UintegerValue(startupPerGpuNs));
        }
    }
    else if (protocolModelType == "ns3::UbProtocolModel")
    {
        Config::SetDefault("ns3::UbProtocolModel::StartupDelayNs", UintegerValue(startupSIMPLENs));
        if (startupPerGpuNs > 0)
        {
            Config::SetDefault("ns3::UbProtocolModel::PerGpuStartupDelayNs", UintegerValue(startupPerGpuNs));
        }
    }

    // Set large TxQueue to avoid packet drops during bursty alltoall traffic
    Config::SetDefault("ns3::DropTailQueue<Packet>::MaxSize", StringValue("1000000p"));

    // FEC model: created before topology build so helper can propagate
    // to switch ports in single-node mode. Applied to endpoints post-build.
    Ptr<FecModel> fecModel;
    if (fecN > 0 && fecK > 0)
    {
        fecModel = CreateObject<FecModel>();
        fecModel->SetFecParams(fecN, fecK, fecT);
        fecModel->SetEncodeLatency(NanoSeconds(fecEncodeLatencyNs));
        fecModel->SetDecodeLatency(NanoSeconds(fecDecodeLatencyNs));
        fecModel->SetEnabled(fecEnabled);
    }

    // Link degradation model: created before topology build so helper can
    // propagate BER to switch ports. Applied to endpoints after build.
    // Triggered by global --ber, any per-tier BER flag, or packet loss rate.
    // In config mode the profile is the source of truth for BER, so when no
    // CLI tier BER was given, fall back to the bundle's per-tier BERs.
    if (configMode
        && berIntraNodeElectrical == 0.0 && berIntraRackElectrical == 0.0
        && berInterRackElectrical == 0.0 && berInterRackOptical == 0.0)
    {
        // The profile exposes three BER tiers (intra-node electrical,
        // intra-rack electrical, inter-rack optical). The CLI's fourth tier
        // (inter-rack electrical) has no profile counterpart and stays 0.
        berIntraNodeElectrical = pbundle.berIntraNodeElectrical;
        berIntraRackElectrical = pbundle.berIntraRackElectrical;
        berInterRackOptical = pbundle.berInterRackOptical;
    }
    Ptr<LinkDegradationModel> degradation;
    if (ber > 0.0 || packetLossRate > 0.0
        || berIntraNodeElectrical > 0.0 || berIntraRackElectrical > 0.0
        || berInterRackElectrical > 0.0 || berInterRackOptical > 0.0
        || (fecModel && fecModel->IsEnabled()))
    {
        degradation = CreateObject<LinkDegradationModel>();
        // Always set BER (0 = no errors) so the shared model doesn't retain
        // its 1e-12 constructor default when only tier BERs are set.
        degradation->SetBer(ber);
        if (packetLossRate > 0.0) { degradation->SetPacketLossRate(packetLossRate); }

        if (errorModeStr == "burst")
        {
            degradation->SetErrorMode(ErrorMode::BURST);
            if (codewordSize > 0) { degradation->SetCodewordSize(codewordSize); }
            degradation->SetBurstLength(burstLength);
            degradation->SetBurstArrivalRate(burstArrivalRate);
        }
    }

    // Build topology
    std::vector<Ptr<FabricEndpoint>> endpoints;
    // RTTCC machines (one per So endpoint, --rttcc): kept for RESULT stats.
    std::vector<Ptr<RttccRateMachine>> rttccMachines;
    // RTTCC is mounted only on multi-node (So) endpoints -- the shaping gate
    // is !IsLocalRank(destRank), so a single-node run has nothing to shape.
    if (enableRttcc && numNodes <= 1)
    {
        std::cerr << "Warning: --rttcc has no effect with numNodes=1 (RTTCC "
                     "shapes only inter-node (So) flows; machines are mounted "
                     "in the multi-node path).\n";
    }

    // Create protocol model and payload builder based on type
    ObjectFactory protoFactory(protocolModelType);
    Ptr<ProtocolModel> protoModel = protoFactory.Create<ProtocolModel>();

    // Apply forced protocol if specified
    if (forceProtocol != 0)
    {
        protoModel->SetForceProtocolId(static_cast<uint8_t>(forceProtocol));
    }

    std::string payloadBuilderType;
    if (protocolModelType == "ns3::NcclProtocolModel")
    {
        payloadBuilderType = "ns3::NcclProtocolPayloadBuilder";
    }
    else if (protocolModelType == "ns3::McclProtocolModel")
    {
        payloadBuilderType = "ns3::McclPayloadBuilder";
    }
    else if (protocolModelType == "ns3::UbProtocolModel")
    {
        payloadBuilderType = "ns3::UbPayloadBuilder";
    }
    else
    {
        NS_ABORT_MSG("Unknown protocol model: " << protocolModelType);
    }

    ObjectFactory payloadFactory(payloadBuilderType);
    Ptr<ProtocolPayloadBuilder> payloadBuilder = payloadFactory.Create<ProtocolPayloadBuilder>();

    // Config mode: the [stack] profile is the source of truth for the PEX
    // bundle. Reassign the protocol model, payload builder, FEC model, and
    // LLR flag to the bundle's objects so all downstream wiring (the endpoint
    // apply loops, NVSwitch FEC, and the FEC/LLR gate above) uses them.
    if (configMode)
    {
        protoModel = pbundle.protocol;
        payloadBuilder = pbundle.payloadBuilder;
        if (pbundle.fec)
        {
            fecModel = pbundle.fec;
            // The profile key set has no fecEnabled (RESERVED only covers
            // fecN/fecK/fecT/fecEncodeLatencyNs/fecDecodeLatencyNs) and
            // FecModel defaults m_enabled=true, so an unconditional swap
            // silently re-enabled FEC after the CLI's --fecEnabled=0 had
            // disabled the model built at line ~945 — config mode then ran
            // with the encode/decode latency and BER-protection behavior
            // the user had explicitly turned off, indistinguishable in the
            // counters from --fecEnabled=1 (codex round 29, finding 4).
            // The explicit CLI flag wins over the model default.
            fecModel->SetEnabled(fecEnabled);
        }
        llrEnabled = pbundle.llrEnabled;
        if (forceProtocol != 0)
        {
            protoModel->SetForceProtocolId(static_cast<uint8_t>(forceProtocol));
        }
    }

    Ptr<ContentionModel> contentionModel;  // Created in single-node else block or later

    if (numNodes > 1)
    {
        // Multi-node mode: use MultiNodeTopologyHelper
        numGpus = numNodes * gpusPerNode;

        FabricType interNodeFabricType = FabricTypeFromString(interNodeFabricTypeStr);

        // --voqDropThreshold's CLI default is 8 (the plain-ETH legacy "lossy
        // by nature" default), so "nonzero" cannot distinguish "user opted
        // in" from "unset": scan argv for an explicit --voqDropThreshold. Only
        // an explicit threshold opts RoCE into the lossy-RoCE (no-PFC)
        // regime; an unset threshold keeps RoCE lossless (ECN-only parity).
        bool voqDropThresholdSet = false;
        for (int i = 1; i < argc; ++i)
        {
            if (std::string(argv[i]).rfind("--voqDropThreshold", 0) == 0)
            {
                voqDropThresholdSet = true;
            }
        }
        const bool lossyRoce = interNodeFabricType == FabricType::ROCE
                                   && voqDropThresholdSet
                                   && voqDropThreshold != 0;

        // Plain (non-RoCE) lossy Ethernet REQUIRES link-level retry: the
        // EthernetSwitch tail-drops at VoqDropThreshold and emits a NACK back
        // to the source (EmitNackBack), which the sender retransmits via the
        // LLR/SACK path (StorePacket -> HandleRetryRequest/HandleSackRequest).
        // Without LLR the drop is silent and the transfer stalls forever.
        // Force LLR on for the Ethernet path (a fabric precondition, like
        // RoCE's ECN/PFC infra) -- and for lossy RoCE, which relies on the
        // same NACK->LLR retransmit path once tail-drop fires. BOTH the
        // local llrEnabled var AND pbundle.llrEnabled must be set:
        // ProtocolConfigRunner::ApplyBundle (below) re-applies
        // pbundle.llrEnabled to every endpoint (protocol-config-runner.cc:53),
        // so the bundle must carry true or it clobbers this force after the
        // endpoint loop sets m_llrEnabled.
        if (interNodeFabricType == FabricType::ETHERNET || lossyRoce)
        {
            const bool wasLlr = llrEnabled;
            llrEnabled = true;
            pbundle.llrEnabled = true;
            if (!wasLlr)
            {
                std::cerr << "Note: the So fabric is lossy (VOQ tail-drop at "
                             "threshold " << voqDropThreshold
                             << ") and requires LLR retransmission; forcing "
                             "llrEnabled=1.\n";
            }
        }

        MultiNodeTopologyHelper multiTopo;
        multiTopo.SetNumNodes(numNodes);
        multiTopo.SetGpusPerNode(gpusPerNode);
        multiTopo.SetIntraNodeDataRate(std::to_string(bandwidthGbps) + "Gbps");
        multiTopo.SetIntraNodeDelay(std::to_string(latencyNs) + "ns");
        multiTopo.SetIntraNodeFabricType(fabricType);
        multiTopo.SetLinksPerGpu(linksPerGpu);
        if (numLanes > 1) { multiTopo.SetNumLanes(numLanes); }
        multiTopo.SetSprayChunkSize(sprayChunkSize);
        multiTopo.SetVcCredits(effectiveVcCredits);
        multiTopo.SetSwitchVoqDepth(switchVoqDepth);
        multiTopo.SetSwitchArbInterval(switchArbIntervalNs);
        // Always forward the value: 0 is the documented store-and-forward
        // selector (NvSwitch falls back to Send()), not "leave default".
        multiTopo.SetSwitchCutThroughDelay(switchCutThroughDelayNs);
        // Select the crossbar arbitration strategy (default roundrobin is the
        // NvSwitch's built-in default, so only override for a non-default type).
        if (arbiterType != "roundrobin" && !arbiterType.empty())
        {
            std::string tid = (arbiterType == "wfq" || arbiterType == "weighted")
                               ? "ns3::RoundRobinArbiter"  // placeholder until a WfqArbiter ships
                               : (arbiterType.find("::") == std::string::npos
                                  ? std::string("ns3::") + arbiterType
                                  : arbiterType);
            ObjectFactory af(tid);
            Ptr<Arbiter> arbiter = af.Create<Arbiter>();
            if (arbiter)
            {
                multiTopo.SetArbiter(arbiter);
            }
            else
            {
                std::cerr << "Warning: could not create arbiter '" << tid
                          << "'; using default roundrobin" << std::endl;
            }
        }
        multiTopo.SetInterNodeDataRate(std::to_string(interNodeBandwidthGbps) + "Gbps");
        multiTopo.SetInterNodeDelay(std::to_string(interNodeLatencyNs) + "ns");
        multiTopo.SetInterNodeFabricType(interNodeFabricType);
        // PFC (802.1Qbb-style, opt-in): applies to both So switch fabrics
        // (RoCE lossless backstop and plain-Ethernet drop avoidance). The
        // helper routes the attributes onto every So switch it builds; mode
        // 0 (default) leaves everything byte-identical.
        if (switchPfcMode != 0)
        {
            // Pause must fire BEFORE the tail-drop or the lossy path wins
            // the race and PFC is dead weight: pfcPauseThreshold <
            // voqDropThreshold whenever both are armed (the same
            // invariant shape as voqDropThreshold < switchVoqDepth).
            // The ARMED threshold, not the raw CLI value:
            // --voqDropThreshold defaults to 8 (the plain-ETH legacy
            // default), but RoCE stays lossless (threshold 0) unless the
            // flag was passed EXPLICITLY — testing the raw default rejected
            // `--switchPfcMode=1` alone on a lossless two-node RoCE/ToR
            // run (64 >= 8 abort; codex round 27, finding 8). Plain ETH
            // always arms the drop; for RoCE `lossyRoce` is exactly the
            // explicit-flag arm.
            const uint32_t armedDropThreshold =
                (interNodeFabricType == FabricType::ROCE && !lossyRoce)
                    ? 0
                    : voqDropThreshold;
            if (armedDropThreshold != 0 && pfcPauseThreshold >= armedDropThreshold)
            {
                NS_ABORT_MSG_IF(
                    true,
                    "pfcPauseThreshold (" << pfcPauseThreshold
                    << ") must be < voqDropThreshold (" << armedDropThreshold
                    << ") when --switchPfcMode>0 and the lossy drop is armed, "
                    << "else the tail-drop fires before the pause and PFC "
                    << "never engages.");
            }
            multiTopo.SetInterNodeSwitchPfcMode(switchPfcMode);
            multiTopo.SetInterNodeSwitchPfcPauseThreshold(pfcPauseThreshold);
            multiTopo.SetInterNodeSwitchPfcResumeOffset(pfcResumeOffset);
            multiTopo.SetInterNodeSwitchPfcPauseTime(pfcPauseTimeUs);
            // A pause needs a switch in the path to ORIGINATE it -- the same
            // inert-topology warning shape as the lossy drop below.
            if (interNodeTopology == "fullmesh" || interNodeTopology == "ring" ||
                interNodeTopology == "torus2d" || interNodeTopology == "dragonfly")
            {
                std::cerr << "Warning: --switchPfcMode=" << switchPfcMode
                          << " with --interNodeTopology=" << interNodeTopology
                          << " has no switch in the inter-node path, so PFC "
                             "is INERT. Use --interNodeTopology=host, tor, "
                             "rail, or leafspine to exercise PFC.\n";
            }
        }
        if (interNodeFabricType == FabricType::ROCE)
        {
            // Real Ethernet RoCEv2 So fabric: switched inter-node topologies
            // (e.g. "host") use the Ethernet switch subtype (a thin NvSwitch
            // derivative) instead of the NVLink NvSwitch.
            multiTopo.SetInterNodeSwitchType("ns3::EthernetSwitch");
            // ECN threshold is an EthernetSwitch Attribute; set the default
            // before the helper instantiates the So switches. ECN marking is
            // live (Phase B): the switch marks SoHeader ECN at this VOQ depth
            // on enqueue; the CNP+DCQCN reaction is gated by --dcqcn.
            Config::SetDefault("ns3::EthernetSwitch::EcnThreshold",
                               UintegerValue(ecnThreshold));
            // Lossy RoCE (no-PFC) opt-in: an explicit --voqDropThreshold=N
            // (N>0) arms the VOQ tail-drop + NACK->LLR retransmit path
            // alongside ECN marking (mark at ecnThreshold, drop at
            // voqDropThreshold; keep ecnThreshold < voqDropThreshold for
            // mark-before-drop, the real RoCEv2 ordering). Without the
            // explicit flag the switch's VoqDropThreshold stays at its
            // lossless default (0) and RoCE keeps byte-identical parity.
            if (lossyRoce)
            {
                NS_ABORT_MSG_IF(
                    voqDropThreshold >= switchVoqDepth,
                    "voqDropThreshold (" << voqDropThreshold
                    << ") must be < switchVoqDepth (" << switchVoqDepth
                    << ") or 0=off, else the base hard-cap drop fires before "
                    << "the NACK path and the lossy transfer silently stalls.");
                Config::SetDefault("ns3::EthernetSwitch::VoqDropThreshold",
                                   UintegerValue(voqDropThreshold));
                // The So switch's m_llrEnabled must be true or EmitNackBack
                // (nvswitch.cc) returns early -- the lossy drop stays silent
                // and the transfer stalls. The endpoint-side LLR force above
                // arms the sender; this arms the NACK-originating switch.
                multiTopo.SetInterNodeSwitchLlrEnabled(true);
                // The lossy-NACK path lives in EthernetSwitch::OnPacketEnqueue
                // and only engages when the inter-node topology instantiates a
                // switch (host / tor / rail / leafspine). fullmesh / ring and
                // friends build direct P2P links with no switch -- a drop
                // there is silent. Warn (not abort): a non-incast P2P run on
                // those topologies is still valid, only the lossy-retransmit
                // behaviour is unavailable.
                if (interNodeTopology == "fullmesh" || interNodeTopology == "ring" ||
                    interNodeTopology == "torus2d" || interNodeTopology == "dragonfly")
                {
                    std::cerr << "Warning: --voqDropThreshold=" << voqDropThreshold
                              << " with --interNodeTopology=" << interNodeTopology
                              << " has no switch in the inter-node path, so the "
                                 "lossy VOQ tail-drop + NACK retransmit path is "
                                 "INERT. Use --interNodeTopology=host, tor, "
                                 "rail, or leafspine to exercise the lossy "
                                 "RoCE fabric.\n";
                }
            }
        }
        else if (interNodeFabricType == FabricType::ETHERNET)
        {
            // Plain (non-RoCE) lossy Ethernet So fabric: same EthernetSwitch
            // subtype (L2 MAC-learning + VOQ + crossbar datapath), but lossy.
            // VoqDropThreshold < VoqDepth is the lossy-NACK invariant: the
            // tail-drop + EmitNackBack path (EthernetSwitch::OnPacketEnqueue)
            // must fire before the base hard-cap drop (NvSwitch at m_voqDepth),
            // else the loss is silent. The operative So VoqDepth is
            // `switchVoqDepth` (default 10000): the helper calls
            // SetVoqDepth(switchVoqDepth) on every So switch after construction,
            // which supersedes the Config::SetDefault below (a construction-
            // time fallback only). Enforce the invariant so a user passing
            // --switchVoqDepth=4 --voqDropThreshold=8 cannot silently stall.
            NS_ABORT_MSG_IF(
                voqDropThreshold != 0 && voqDropThreshold >= switchVoqDepth,
                "voqDropThreshold (" << voqDropThreshold
                << ") must be < switchVoqDepth (" << switchVoqDepth
                << ") or 0=off, else the base hard-cap drop fires before the "
                << "NACK path and the lossy transfer silently stalls.");
            multiTopo.SetInterNodeSwitchType("ns3::EthernetSwitch");
            Config::SetDefault("ns3::NvSwitch::VoqDepth",
                               UintegerValue(1024));
            Config::SetDefault("ns3::EthernetSwitch::VoqDropThreshold",
                               UintegerValue(voqDropThreshold));
            // The lossy-NACK path lives in EthernetSwitch::OnPacketEnqueue, so
            // it only engages when the inter-node topology instantiates a
            // switch (host / tor). fullmesh / ring build direct P2P links with
            // a DropTailQueue and NO switch -- a drop there is silent (no NACK,
            // no retransmit), so the lossy feature is inert under incast. Warn
            // (not abort): a non-incast P2P run on fullmesh/ring is still valid,
            // only the lossy-retransmit behaviour is unavailable.
            if (interNodeTopology == "fullmesh" || interNodeTopology == "ring" ||
                interNodeTopology == "torus2d" || interNodeTopology == "dragonfly")
            {
                std::cerr << "Warning: --interNodeFabricType=ETH with "
                             "--interNodeTopology=" << interNodeTopology
                          << " has no switch in the inter-node path, so the "
                             "lossy VOQ tail-drop + NACK retransmit path is "
                             "INERT (a P2P DropTailQueue overflow would drop "
                             "silently). Use --interNodeTopology=host, tor, "
                             "rail, or leafspine to exercise the lossy "
                             "fabric.\n";
            }
            // The So switch's m_llrEnabled must be true or EmitNackBack
            // (nvswitch.cc) returns early -- the lossy drop stays silent and
            // the transfer stalls. The endpoint-side LLR force above arms the
            // sender (StorePacket + ProcessNackPacket); this arms the switch
            // that ORIGINATES the NACK on tail-drop. Default false on the
            // helper keeps the lossless RoCE path byte-identical (RoCE avoids
            // drops via ECN, so it never relies on switch-emitted NACKs).
            multiTopo.SetInterNodeSwitchLlrEnabled(true);
        }
        multiTopo.SetInterNodeTopology(interNodeTopology);
        multiTopo.SetInterNodeSpines(interNodeSpines);
        multiTopo.SetInterNodeTorusWidth(interNodeTorusWidth);
        multiTopo.SetInterNodeDragonflyGroupSize(interNodeDragonflyGroupSize);
        multiTopo.SetIntraNodeTopology(intraNodeTopology);

        NodeContainer nodes = multiTopo.Build();
        multiTopo.PopulateInterNodeRouting();

        // Scale-out (So) RoCEv2 protocol model for inter-node timing. When the
        // So fabric is RoCE, each endpoint gets a RoceProtocolModel for
        // remote-rank traffic (Ethernet ~0.95 wire efficiency) while local-rank
        // traffic keeps the scale-up (NCCL) model. With no So model wired,
        // GetProtocolModelForDest falls back to the Su model (parity-safe).
        Ptr<RoceProtocolModel> roceModel;
        Ptr<EthernetProtocolModel> ethModel;
        if (interNodeFabricType == FabricType::ROCE)
        {
            roceModel = CreateObject<RoceProtocolModel>();
        }
        else if (interNodeFabricType == FabricType::ETHERNET)
        {
            // Plain (non-RoCE) lossy Ethernet So model: WireEfficiency=1.0
            // (no SoHeader encapsulation, no DCQCN/per-QP ceiling -- those are
            // RDMA concepts), store-and-forward per-step latency. No DcqcnRate
            // Machine is wired for the Ethernet path below.
            ethModel = CreateObject<EthernetProtocolModel>();
        }

        // DCQCN rate machine (Phase B) + opt-in per-QP ceiling pacer. One shared
        // machine across all So endpoints: state is keyed per flow
        // (destRank,vcId,flowId), so a single instance is correct and matches a
        // real NIC's per-flow tables. Null when both --dcqcn and
        // --perQpCeilingGbps are off -> CNP no-ops + zero shaping/pacing
        // (parity-safe). The per-QP ceiling uses the machine's true per-flow
        // pacer (not the DCQCN alpha path), so it mounts the machine even with
        // --dcqcn off; PerQpCeilingGbps=0 there keeps ShapingDelay==0.
        Ptr<DcqcnRateMachine> dcqcnMachine;
        if (interNodeFabricType == FabricType::ROCE && (enableDcqcn || perQpCeilingGbps > 0.0))
        {
            dcqcnMachine = CreateObject<DcqcnRateMachine>();
            if (perQpCeilingGbps > 0.0)
            {
                dcqcnMachine->SetAttribute("PerQpCeilingGbps", DoubleValue(perQpCeilingGbps));
            }
            // MaxRateBps anchors the alpha shaping math (extra serialization
            // time at the reduced rate) to the So NIC rate; the TypeId
            // default 50e9 B/s is only right for 400G links. Inert until a
            // CNP fires (alpha=0 -> ShapingDelay==0) and unused by the
            // per-QP pacer, so this backfill is parity-neutral.
            if (enableDcqcn && interNodeBandwidthGbps > 0)
            {
                dcqcnMachine->SetAttribute(
                    "MaxRateBps",
                    DoubleValue(interNodeBandwidthGbps * 1.25e8));
            }
            // User overrides: only SetAttribute when the flag was given, so
            // the default path keeps the TypeId defaults byte-identically.
            if (dcqcnTFastNs > 0)
            {
                dcqcnMachine->SetAttribute("TFast", TimeValue(NanoSeconds(dcqcnTFastNs)));
            }
            if (dcqcnFastCycles > 0)
            {
                dcqcnMachine->SetAttribute("FastRecoveryMaxCycles",
                                           UintegerValue(dcqcnFastCycles));
            }
            if (dcqcnTAiNs > 0)
            {
                dcqcnMachine->SetAttribute("TAi", TimeValue(NanoSeconds(dcqcnTAiNs)));
            }
            if (dcqcnRai > 0.0)
            {
                dcqcnMachine->SetAttribute("RAI", DoubleValue(dcqcnRai));
            }
            if (dcqcnGInit > 0.0)
            {
                dcqcnMachine->SetAttribute("GInit", DoubleValue(dcqcnGInit));
            }
            if (dcqcnGDecay > 0.0)
            {
                dcqcnMachine->SetAttribute("GDecay", DoubleValue(dcqcnGDecay));
            }
            if (dcqcnAlphaFloor != 0.0)
            {
                dcqcnMachine->SetAttribute("AlphaFloor", DoubleValue(dcqcnAlphaFloor));
            }
        }

        // So payload builder: opting into DCQCN implies RoCE SoHeader framing
        // on inter-node sends -- the switch ECN-mark and receiver CNP-echo
        // guards key on RoceProtocol::SIMPLE + SoHeader, which inline runs'
        // NCCL builder never stamps (config-mode RoCE profiles mount the RoCE
        // builder globally; inline collectives would otherwise leave the whole
        // ECN->CNP->DCQCN chain dormant). The dedicated So builder leaves Su
        // (intra-node) framing/timing untouched. Off (--dcqcn=0, default) ->
        // null -> byte-identical.
        Ptr<RocePayloadBuilder> soPayloadBuilder;
        if (interNodeFabricType == FabricType::ROCE && enableDcqcn)
        {
            soPayloadBuilder = CreateObject<RocePayloadBuilder>();
        }

        ApplicationContainer apps = multiTopo.GetAllEndpoints();
        for (uint32_t i = 0; i < apps.GetN(); ++i)
        {
            Ptr<FabricEndpoint> ep = DynamicCast<FabricEndpoint>(apps.Get(i));
            ep->SetNumVirtualChannels(1);
            ep->SetVcCredits(0, effectiveVcCredits);
            ep->SetBypassReorderBuffer(true);
            ep->SetProtocolModel(protoModel);
            ep->SetProtocolPayloadBuilder(payloadBuilder);
            if (roceModel)
            {
                ep->SetInterNodeProtocolModel(roceModel);
            }
            if (ethModel)
            {
                ep->SetInterNodeProtocolModel(ethModel);
            }
            if (dcqcnMachine)
            {
                ep->SetDcqcnRateMachine(dcqcnMachine);
            }
            if (soPayloadBuilder)
            {
                ep->SetSoPayloadBuilder(soPayloadBuilder);
            }
            // RTTCC: one machine PER endpoint (not shared like DCQCN's) —
            // the flow key (destRank,vc,flowId) carries no source rank, so a
            // shared machine would merge the per-flow pacer/probe state of
            // two incast senders targeting the same flow coordinates. The
            // line rate is the So NIC rate (the link RTTCC paces onto).
            if (enableRttcc)
            {
                Ptr<RttccRateMachine> rttccMachine = CreateObject<RttccRateMachine>();
                rttccMachine->SetAttribute("LineRateGbps",
                                           DoubleValue(static_cast<double>(interNodeBandwidthGbps)));
                if (rttccTargetRttNs > 0)
                {
                    rttccMachine->SetAttribute("TargetRtt", TimeValue(NanoSeconds(rttccTargetRttNs)));
                }
                if (rttccProbeIntervalNs > 0)
                {
                    rttccMachine->SetAttribute("ProbeInterval",
                                               TimeValue(NanoSeconds(rttccProbeIntervalNs)));
                }
                ep->SetRttccRateMachine(rttccMachine);
                rttccMachines.push_back(rttccMachine);
            }
            ep->SetBulkChunkSize(bulkChunkSize);
            ep->SetDeviceType(deviceType);

            if (syncMemLatencyNs > 0) { ep->SetSyncMemLatencyNs(syncMemLatencyNs); }
            if (asyncMemLatencyNs > 0) { ep->SetAsyncMemLatencyNs(asyncMemLatencyNs); }
            ep->SetLlrEnabled(llrEnabled);

            // Per-endpoint ContentionModel: each endpoint gets an INDEPENDENT
            // instance (its WFQ backlog is local to this endpoint's injection
            // side). This DIFFERS from the single-node GpuClusterTopologyHelper,
            // whose AttachContentionModelToEndpoints creates ONE clone and shares
            // it across all endpoints (backlog aggregates node-wide) — two
            // different layers, not a correctness difference. The bandwidth is
            // the intra-node NVLink injection rate (bandwidthGbps), also for
            // So-bound packets. The WFQ penalty only fires at numActive>1
            // (SendPacketOnDevice), so pure-collective So traffic stays
            // byte-identical (single COLLECTIVE class); the model only engages
            // when a second class (e.g. a background P2P flow) is concurrently
            // active on the same endpoint. Note the penalty is an extra delay on
            // the send path, off the wire's own FIFO queueing: for a cross-node
            // background flow the So NIC/switch link sharing dominates and the
            // WFQ term may be entirely off the critical path.
            Ptr<ContentionModel> cm = CreateObject<ContentionModel>();
            cm->SetCollectiveWeight(contentionCollectiveWeight);
            cm->SetMemoryWeight(contentionMemoryWeight);
            cm->SetP2pWeight(contentionP2pWeight);
            cm->SetBandwidth(bandwidthGbps * 1000000000ULL / 8);  // Gbps -> bytes/sec
            ep->SetContentionModel(cm);

            endpoints.push_back(ep);
        }

        // Configure NVSwitches for LLR and SHARP/NVLS
        for (uint32_t nodeId = 0; nodeId < numNodes; ++nodeId)
        {
            Ptr<NvSwitch> nvSw = multiTopo.GetNvSwitch(nodeId);
            if (!nvSw)
            {
                continue;
            }
            nvSw->SetLlrEnabled(llrEnabled);
            if (fecModel)
            {
                nvSw->SetFecModel(fecModel);
            }
            bool nvlsMayRun = (algorithm == "sharp" || algorithm == "nvls" || algorithm == "auto");
            if (nvlsMayRun)
            {
                nvSw->SetAllReduceEnabled(true);
                nvSw->SetAllReduceThreshold(gpusPerNode);
                nvSw->SetAllReduceAggregationDelay(sharpAggregationDelayNs);
                nvSw->SetAllReduceDataSize(dataSize);
                nvSw->SetAllReduceNumPartitions(nvlsNumPartitions);
                if (nvlsMayRun && collective == "allgather")
                {
                    nvSw->SetAllGatherEnabled(true);
                    nvSw->SetAllGatherThreshold(gpusPerNode);
                    nvSw->SetAllGatherChunkSize(dataSize / gpusPerNode);
                    nvSw->SetAllGatherDataSize(dataSize);
                }
            }
        }

        for (auto& ep : endpoints)
        {
            ep->Initialize();
        }
    }
    else
    {
        // Single-node mode: use existing GpuClusterTopologyHelper
        uint32_t numSwitches = 0;
        if (topology == "switched" || topology == "fattree" || topology == "railfattree" || topology == "leafspine"
            || topology == "nvl72" || topology == "3levelhierarchical"
            || topology == "dragonflyplus" || topology == "multiplane")
        {
            if (topology == "leafspine") numSwitches = leafSwitches + spineSwitches;
            else if (topology == "railfattree")
            {
                const uint32_t enclosures = numGpus / railCount;
                const uint32_t leavesPerRail =
                    (enclosures + railNodesPerLeaf - 1) / railNodesPerLeaf;
                numSwitches = railCount * leavesPerRail +
                              railSpineSwitches + railCoreSwitches;
            }
            else if (topology == "nvl72") numSwitches = nvl72SwitchPlanes;
            else if (topology == "3levelhierarchical") numSwitches = leafSwitches + hier3MidSwitches + spineSwitches;
            else if (topology == "dragonflyplus") numSwitches = dflyGroups * dflyRoutersPerGroup;
            else if (topology == "multiplane") numSwitches = multiPlaneCount;
            else numSwitches = 1;
        }

        // For torus/mesh/hypercube, override numGpus based on topology dimensions
        uint32_t effectiveNumGpus = numGpus;
        if (topology == "torus")
        {
            effectiveNumGpus = torusDimX * torusDimY * torusDimZ;
        }
        else if (topology == "mesh")
        {
            effectiveNumGpus = meshRows * meshCols;
        }
        else if (topology == "hypercube")
        {
            effectiveNumGpus = 1 << hypercubeDims;
        }
        else if (topology == "2dfullmesh")
        {
            effectiveNumGpus = rackRows * rackCols;
        }
        else if (topology == "2dfullmeshclos")
        {
            effectiveNumGpus = rackRows * rackCols * numRacks;
        }
        numGpus = effectiveNumGpus;

        GpuClusterTopologyHelper topoHelper(numGpus, numSwitches);
        // For switched/leafspine topology, use hardware per-link data rate if specified;
        // otherwise fall back to bandwidth (endpoint effective BW) which conflates the two.
        uint64_t effectiveLinkRate = linkDataRateGbps > 0 ? linkDataRateGbps : bandwidthGbps;
        topoHelper.SetLinkDataRate(std::to_string(effectiveLinkRate) + "Gbps");
        topoHelper.SetLinkDelay(std::to_string(latencyNs) + "ns");
        topoHelper.SetFabricType(fabricType);
        topoHelper.SetInterSwitchMedium(interSwitchMedium);
        topoHelper.SetFecOpticalOnly(fecScope == "optical");

        if (topology == "switched" || topology == "railfattree" || topology == "leafspine" || topology == "nvl72"
            || topology == "3levelhierarchical" || topology == "dragonflyplus"
            || topology == "multiplane")
        {
            topoHelper.SetLinksPerGpu(linksPerGpu);
            topoHelper.SetSprayChunkSize(sprayChunkSize);
        }

        if (numLanes > 1)
        {
            topoHelper.SetNumLanes(numLanes);
        }

        for (uint32_t i = 0; i < numGpus; ++i)
        {
            topoHelper.SetDeviceType(static_cast<uint16_t>(i), deviceType);
        }

        // Propagate degradation to switch ports via helper (must be before Build*)
        if (degradation && (topology == "switched" || topology == "fattree" || topology == "railfattree" || topology == "leafspine"
            || topology == "nvl72" || topology == "torus" || topology == "mesh"
            || topology == "hypercube" || topology == "2dfullmesh"
            || topology == "2dfullmeshclos"
            || topology == "3levelhierarchical" || topology == "dragonflyplus"
            || topology == "multiplane"))
        {
            topoHelper.SetLinkDegradationModel(degradation);
            // Per-tier BER overrides (Phase A.1). When set (>0), Build* functions
            // pick the BER for each channel based on its link class and medium.
            topoHelper.SetBerTiers(berIntraNodeElectrical,
                                   berIntraRackElectrical,
                                   berInterRackElectrical,
                                   berInterRackOptical);
        }

        // Propagate FEC model to helper so switch ports also get FEC decode
        // (NVLink FEC operates at link level: both GPU and NVSwitch endpoints
        // have FEC encode/decode on their NVLink ports)
        if (fecModel)
        {
            topoHelper.SetFecModel(fecModel);
        }

        // Enable LLR on switch NVLink ports: NVSwitch detects CRC/FEC failure
        // on ingress and sends NACK back, matching real NVLink behavior
        topoHelper.SetSwitchLlrEnabled(llrEnabled);

        // Contention model: configured once as a template; the helper's Build*
        // clones it ONCE and shares that single instance across all endpoints
        // (backlog aggregates node-wide). Multi-node mounts one per endpoint.
        contentionModel = CreateObject<ContentionModel>();
        contentionModel->SetCollectiveWeight(contentionCollectiveWeight);
        contentionModel->SetMemoryWeight(contentionMemoryWeight);
        contentionModel->SetP2pWeight(contentionP2pWeight);
        contentionModel->SetBandwidth(bandwidthGbps * 1000000000ULL / 8);  // Gbps -> bytes/sec
        topoHelper.SetContentionModel(contentionModel);

        NodeContainer nodes;
        if (topology == "ring")
        {
            nodes = topoHelper.BuildRing();
        }
        else if (topology == "fullmesh")
        {
            nodes = topoHelper.BuildFullMesh();
        }
        else if (topology == "switched")
        {
            nodes = topoHelper.BuildFullyConnected();
        }
        else if (topology == "fattree")
        {
            nodes = topoHelper.BuildFatTree(fattreeRadix);
        }
        else if (topology == "railfattree")
        {
            nodes = topoHelper.BuildRailOptimizedFatTree(
                railCount, railNodesPerLeaf, railSpineSwitches,
                railLinksPerSpine, railCoreSwitches);
        }
        else if (topology == "leafspine")
        {
            nodes = topoHelper.BuildLeafSpine(leafSwitches, spineSwitches);
        }
        else if (topology == "nvl72")
        {
            nodes = topoHelper.BuildNvl72(nvl72SwitchPlanes, nvl72GpusPerGroup);
        }
        else if (topology == "torus")
        {
            nodes = topoHelper.BuildTorus(torusDimX, torusDimY, torusDimZ);
        }
        else if (topology == "mesh")
        {
            nodes = topoHelper.BuildMesh2D(meshRows, meshCols);
        }
        else if (topology == "hypercube")
        {
            nodes = topoHelper.BuildHypercube(hypercubeDims);
        }
        else if (topology == "2dfullmesh")
        {
            nodes = topoHelper.Build2DFullMesh(rackRows, rackCols);
        }
        else if (topology == "2dfullmeshclos")
        {
            nodes = topoHelper.Build2DFullMeshClos(rackRows, rackCols, numRacks, numSpineSwitchesUb);
        }
        else if (topology == "3levelhierarchical")
        {
            nodes = topoHelper.Build3LevelHierarchical(leafSwitches, hier3MidSwitches, spineSwitches);
        }
        else if (topology == "dragonflyplus")
        {
            nodes = topoHelper.BuildDragonflyPlus(dflyGroups, dflyRoutersPerGroup);
        }
        else if (topology == "multiplane")
        {
            nodes = topoHelper.BuildMultiPlane(multiPlaneCount);
        }
        else
        {
            NS_ABORT_MSG("Unknown topology: " << topology);
        }

        opticalInterSwitchLinks = topoHelper.GetOpticalInterSwitchLinkCount();
        if (failedOpticalLink >= 0)
        {
            NS_ABORT_MSG_IF(static_cast<uint32_t>(failedOpticalLink)
                                >= opticalInterSwitchLinks,
                            "failedOpticalLink=" << failedOpticalLink
                            << " is outside the " << opticalInterSwitchLinks
                            << " discovered optical inter-switch links");
            failureRoutesComplete = topoHelper.FailOpticalInterSwitchLink(
                static_cast<uint32_t>(failedOpticalLink));
            failureUnreachableGpuPairs = topoHelper.GetUnreachableGpuPairs();
            failedOpticalLinkBer = topoHelper.GetFailedOpticalLinkBer();
            failedOpticalLinkDescription =
                topoHelper.GetFailedOpticalLinkDescription();
        }
        else if (failureAwareRouting)
        {
            failureRoutesComplete = topoHelper.RecomputeFailureAwareRoutes();
            failureUnreachableGpuPairs = topoHelper.GetUnreachableGpuPairs();
        }
        operationalOpticalInterSwitchLinks =
            topoHelper.GetOperationalOpticalInterSwitchLinkCount();
        operationalOpticalBerMin =
            topoHelper.GetOperationalOpticalInterSwitchBerMin();
        operationalOpticalBerMax =
            topoHelper.GetOperationalOpticalInterSwitchBerMax();

        // Install topology-aware ring/tree collective embedding so ring/tree
        // send targets follow the physical locality of the fabric (Gray code
        // on hypercube, serpentine on torus/mesh, NodeId-sorted on switched).
        // Injectors fall back to rank arithmetic when an entry is unset.
        {
            std::string family;
            std::vector<uint32_t> dims;
            if (topology == "hypercube") { family = "hypercube"; }
            else if (topology == "railfattree") { family = "railfattree"; dims = {railCount, railNodesPerLeaf}; }
            else if (topology == "torus") { family = "torus"; dims = {torusDimX, torusDimY, torusDimZ}; }
            else if (topology == "mesh") { family = "mesh2d"; dims = {meshRows, meshCols}; }
            else if (topology == "2dfullmesh") { family = "2dfullmesh"; dims = {rackRows, rackCols}; }
            else if (topology == "ring") { family = "ring"; }
            else if (topology == "fullmesh") { family = "fullmesh"; }
            else { family = "switched"; }  // leafspine/fattree/nvl72/dragonflyplus/multiplane/3levelhierarchical/2dfullmeshclos/switched
            topoHelper.InstallCollectiveEmbedding(family, dims);
        }

        ApplicationContainer apps = topoHelper.GetEndpoints();

        for (uint32_t i = 0; i < apps.GetN(); ++i)
        {
            Ptr<FabricEndpoint> ep = DynamicCast<FabricEndpoint>(apps.Get(i));
            ep->SetNumVirtualChannels(1);
            ep->SetVcCredits(0, effectiveVcCredits);
            ep->SetBypassReorderBuffer(true);
            ep->SetProtocolModel(protoModel);
            ep->SetProtocolPayloadBuilder(payloadBuilder);
            ep->SetBulkChunkSize(bulkChunkSize);

            if (syncMemLatencyNs > 0) { ep->SetSyncMemLatencyNs(syncMemLatencyNs); }
            if (asyncMemLatencyNs > 0) { ep->SetAsyncMemLatencyNs(asyncMemLatencyNs); }
            ep->SetLlrEnabled(llrEnabled);

            endpoints.push_back(ep);
        }

        // Apply packet-path settings to every topology built from NvSwitch devices.
        if (topology == "switched" || topology == "fattree" || topology == "railfattree" || topology == "leafspine"
            || topology == "nvl72" || topology == "3levelhierarchical"
            || topology == "dragonflyplus" || topology == "multiplane")
        {
            NodeContainer switchNodes = topoHelper.GetSwitchNodes();
            for (uint32_t sw = 0; sw < switchNodes.GetN(); ++sw)
            {
                Ptr<NvSwitch> nvSw = nullptr;
                for (uint32_t d = 0; d < switchNodes.Get(sw)->GetNDevices(); ++d)
                {
                    nvSw = DynamicCast<NvSwitch>(switchNodes.Get(sw)->GetDevice(d));
                    if (nvSw) break;
                }
                if (nvSw)
                {
                    nvSw->SetVoqDepth(switchVoqDepth);
                    nvSw->SetArbitrationInterval(switchArbIntervalNs);
                    nvSw->SetCutThroughDelay(switchCutThroughDelayNs);
                    // Enable in-switch SHARP/NVLS when explicitly requested,
                    // or under "auto" so per-phase/per-collective resolution
                    // can pick nvls/sharp without re-building the fabric.
                    bool nvlsMayRun = (algorithm == "sharp" || algorithm == "nvls" || algorithm == "auto");
                    if (topology == "switched" && nvlsMayRun)
                    {
                        nvSw->SetAllReduceEnabled(true);
                        nvSw->SetAllReduceThreshold(numGpus);
                        nvSw->SetAllReduceAggregationDelay(sharpAggregationDelayNs);
                        nvSw->SetAllReduceDataSize(dataSize);
                        nvSw->SetAllReduceNumPartitions(nvlsNumPartitions);
                    }
                    if (topology == "switched" && nvlsMayRun && collective == "allgather")
                    {
                        nvSw->SetAllGatherEnabled(true);
                        nvSw->SetAllGatherThreshold(numGpus);
                        nvSw->SetAllGatherChunkSize(dataSize / numGpus);
                        nvSw->SetAllGatherDataSize(dataSize);
                    }
                }
            }
        }

        for (auto& ep : endpoints)
        {
            ep->Initialize();
        }

    }
    // ======================================================================

    // Apply degradation model to endpoints (switch ports already configured
    // via helper before topology build; endpoints need manual attachment)
    if (degradation)
    {
        for (auto& ep : endpoints)
        {
            ep->SetLinkDegradationModel(degradation);
        }
    }

    // FEC model (already created at outer scope for switch propagation)
    if (fecModel)
    {
        for (auto& ep : endpoints)
        {
            ep->SetFecModel(fecModel);
            ep->SetFecOpticalOnly(fecScope == "optical");
        }
        // FEC model already propagated to NVSwitches via:
        // - single-node: topoHelper.SetFecModel() before build
        // - multi-node: nvSw->SetFecModel() in multi-node block
    }

    // LLR mode configuration
    if (llrEnabled)
    {
        LlrMode llrMode = (llrModeStr == "sack") ? LlrMode::SACK : LlrMode::GO_BACK_N;
        LlrOverflowPolicy overflowPolicy = (llrOverflowPolicyStr == "dropnewest")
            ? LlrOverflowPolicy::DROP_NEWEST : LlrOverflowPolicy::DROP_OLDEST;

        for (auto& ep : endpoints)
        {
            Ptr<LlrManager> llrMgr = ep->GetLlrManager();
            if (llrMgr)
            {
                llrMgr->SetLlrMode(llrMode);
                llrMgr->SetRetryLimit(llrRetryLimit);
                llrMgr->SetMaxBufferSize(effectiveLlrBufferSize);
                llrMgr->SetOverflowPolicy(overflowPolicy);
                llrMgr->SetSourceReloadBandwidth(llrReloadBandwidthGBps * 1000000000ULL);
                llrMgr->SetSourceReloadLatency(NanoSeconds(llrReloadLatencyNs));
                if (llrRetryTimeoutNs > 0)
                {
                    llrMgr->SetRetryTimeout(NanoSeconds(llrRetryTimeoutNs));
                }
                if (llrTimeoutRetransmit)
                {
                    ep->EnableLlrTimeoutRetransmit();
                }
                if (soTxWindow > 0)
                {
                    ep->SetSoTxWindow(soTxWindow);
                }
                if (soGbnStrict)
                {
                    ep->SetSoGbnStrict(true);
                }
            }
        }
    }




    std::cout << "CONFIG: ber=" << ber
              << " berTiers=[intraNodeE=" << berIntraNodeElectrical
              << ",intraRackE=" << berIntraRackElectrical
              << ",interRackE=" << berInterRackElectrical
              << ",interRackO=" << berInterRackOptical << "]"
              << " plr=" << packetLossRate
              << " errMode=" << errorModeStr << " fec=" << fecN << "/" << fecK << "/" << fecT
              << " fecScope=" << fecScope
              << " interSwitchMedium=" << interSwitchMedium
              << " llr=" << (llrEnabled ? llrModeStr : "off")
              << " vcCredits=" << effectiveVcCredits
              << " llrBuf=" << effectiveLlrBufferSize
              << " llrReload=" << llrReloadBandwidthGBps << "GB/s+"
              << llrReloadLatencyNs << "ns" << std::endl;

    Ptr<CollectiveInjector> injector;
    ProtocolConfigRunner configRunner;  // used only in config mode
    // Resolve "auto" for the single-shot path (trace path resolves per-phase
    // inside CreateTraceCollectiveInjector). hasNvls follows the same rule.
    bool hasNvls = (topology == "switched");
    std::string algo = (algorithm == "auto")
        ? ResolveAutoAlgorithm(collective, topology, dataSize, numGpus, hasNvls)
        : algorithm;
    if (algorithm == "auto")
    {
        std::cout << "[algo] " << collective << " size=" << dataSize
                  << " N=" << numGpus << " topo=" << topology
                  << " -> " << algo << std::endl;
    }
    // Stencil path: only when the .cfg declares a transfer stencil (no
    // `collective =` / `algorithm =` in [op]). A three-axis .cfg has already
    // sourced the profile's values + the op axes into the local vars above,
    // so it falls through to the validated inline injector branches below
    // (bit-identical to the inline path run with the equivalent CLI flags).
    // Random-traffic mode replaces the collective injector entirely: every
    // rank self-schedules fixed-interval messages to uniform-random ranks on
    // other nodes, and the run is fixed-duration (stop at warmup+measure).
    if (randTrafficEnabled)
    {
        NS_ABORT_MSG_IF(numNodes <= 1,
                        "--randTraffic needs numNodes > 1 (traffic is cross-node only)");
        NS_ABORT_MSG_IF(randInjectionRate <= 0.0 || randInjectionRate > 1.0,
                        "--randInjectionRate must be in (0, 1]");
        NS_ABORT_MSG_IF(interNodeBandwidthGbps == 0,
                        "--randTraffic needs interNodeBandwidth > 0");
        // Fixed per-source message interval: msgBytes at the requested fraction
        // of the inter-node line rate. Clamped to >= 1 ns (same stall footgun
        // as bgP2pIntervalNs=0: a 0 ns self-reschedule never advances time).
        double intervalNsF = static_cast<double>(randMsgBytes) * 8.0 /
                             (randInjectionRate * static_cast<double>(interNodeBandwidthGbps));
        uint64_t intervalNs = std::max<uint64_t>(1, static_cast<uint64_t>(intervalNsF + 0.5));
        g_randWindowStartNs = randWarmupNs;
        g_randWindowEndNs = randWarmupNs + randMeasureNs;
        g_randTrafficRan = true;

        // DCQCN/ECN chain requirements: EthernetSwitch::MarkEcnIfCongested and
        // the receiver's CNP echo both require the FabricHeader protocol to be
        // RoceProtocol::SIMPLE(1) AND an inner SoHeader, and
        // SendBulkWireTransferSize stamps that SoHeader only when the mounted
        // payload builder is RocePayloadBuilder (Seam B guard). Inline runs
        // mount the NCCL builder, so on the RoCE fabric remount the RoCE
        // builder here — safe because randTraffic replaces the collective and
        // all its traffic is cross-node So (the Su builder is unused).
        uint8_t protocolId = static_cast<uint8_t>(NcclProtocol::SIMPLE);
        Ptr<RocePayloadBuilder> roceBuilder;
        if (FabricTypeFromString(interNodeFabricTypeStr) == FabricType::ROCE)
        {
            protocolId = static_cast<uint8_t>(RoceProtocol::SIMPLE);
            roceBuilder = CreateObject<RocePayloadBuilder>();
        }
        // Per-source RNG streams: destination pick + initial phase offset
        // (random phase avoids all sources injecting in lockstep). Both are
        // RngRun-seeded, so a run is reproducible.
        for (uint16_t rank = 0; rank < numGpus; ++rank)
        {
            Ptr<FabricEndpoint> ep = endpoints[rank];
            if (!ep)
            {
                continue;
            }
            if (roceBuilder)
            {
                ep->SetProtocolPayloadBuilder(roceBuilder);
            }
            ep->SetReceiveCallback(MakeCallback(&RandTrafficReceive));
            std::vector<uint16_t> remoteRanks;
            for (uint16_t r = 0; r < numGpus; ++r)
            {
                if (endpoints[r] && endpoints[r]->GetNodeId() != ep->GetNodeId())
                {
                    remoteRanks.push_back(r);
                }
            }
            if (remoteRanks.empty())
            {
                continue;
            }
            Ptr<UniformRandomVariable> destPick = CreateObject<UniformRandomVariable>();
            Ptr<UniformRandomVariable> phase = CreateObject<UniformRandomVariable>();
            uint64_t startNs = phase->GetInteger(0, intervalNs - 1);
            Simulator::Schedule(NanoSeconds(startNs), &RandTrafficSend, ep,
                                remoteRanks, destPick, randMsgBytes, intervalNs,
                                protocolId);
        }
        Simulator::Stop(NanoSeconds(g_randWindowEndNs));
    }
    else if (configMode && pconfig.GetCollective().empty() && pconfig.GetAlgorithm().empty())
    {
        // Apply the profile's PEX bundle to the endpoints (overrides the inline
        // 1-VC convenience credit setup with the profile's VC count + per-VC
        // credits + flow-control policy), compile the [op] stencil into a
        // transaction graph, and run it via the generic runner. The runner
        // lives in main scope so it outlives Simulator::Run() below.
        ProtocolConfigRunner::ApplyBundle(endpoints, pbundle, bulkChunkSize);
        std::string cfgErr;
        if (!configRunner.Initialize(endpoints, pbundle, pconfig, numGpus, dataSize, &cfgErr))
        {
            NS_ABORT_MSG("protocol config compile error: " << cfgErr);
        }
        configRunner.SetCompletionCallback(
            [](uint64_t durationNs) { OnCollectiveComplete(durationNs); });
        configRunner.Start();
    }
    else if (collective == "allreduce")
    {
        if (algo == "hierarchical")
        {
            // For leafspine topology, derive numNodes/gpusPerNode from leaf groups
            uint32_t effectiveNumNodes = numNodes;
            uint32_t effectiveGpusPerNode = gpusPerNode;
            if (topology == "leafspine")
            {
                effectiveNumNodes = leafSwitches;
                effectiveGpusPerNode = numGpus / leafSwitches;
            }
            else if (topology == "nvl72")
            {
                uint32_t gpg = nvl72GpusPerGroup > 0 ? nvl72GpusPerGroup : numGpus / 6;
                effectiveNumNodes = numGpus / gpg;
                effectiveGpusPerNode = gpg;
            }
            else if (numNodes <= 1)
            {
                NS_ABORT_MSG("Hierarchical AllReduce requires numNodes > 1 (or use leafspine topology)");
            }
            Ptr<HierarchicalAllReduce> ar = CreateObject<HierarchicalAllReduce>();
            ar->SetGpusPerNode(effectiveGpusPerNode);
            ar->SetNumNodes(effectiveNumNodes);
            // Set node IDs from endpoint leaf-group info (set by BuildLeafSpine)
            for (uint16_t rank = 0; rank < numGpus; ++rank)
            {
                if (endpoints[rank])
                {
                    ar->SetNodeIdForRank(rank, endpoints[rank]->GetNodeId());
                }
            }
            ar->SetIntraNodeAlgorithm(intraNodeAlgorithm);
            ar->Initialize(numGpus, dataSize, endpoints);
            ar->SetCompletionCallback(MakeCallback(&OnCollectiveComplete));
            // Protocol-aware startup delays (LL/LL128/SIMPLE per-phase)
            ar->SetStartupDelays(NanoSeconds(startupLLNs), NanoSeconds(startupLL128Ns), NanoSeconds(startupSIMPLENs));
            ar->SetStartupPerGpuNs(startupPerGpuNs);
            ar->SetLlThreshold(llThreshold);
            ar->SetLl128Threshold(ll128Threshold);
            if (localComputeDelayNs > 0)
            {
                ar->SetLocalComputeDelay(NanoSeconds(localComputeDelayNs));
            }
            if (interNodeComputeDelayNs > 0)
            {
                ar->SetInterNodeComputeDelay(NanoSeconds(interNodeComputeDelayNs));
            }
            if (localComputePerByteNs > 0)
            {
                ar->SetLocalComputePerByteNs(localComputePerByteNs);
            }
            if (interNodeComputePerByteNs > 0)
            {
                ar->SetInterNodeComputePerByteNs(interNodeComputePerByteNs);
            }
            if (localComputeBaseLLNs > 0 || localComputeBaseLL128Ns > 0 || localComputeBaseSIMPLENs > 0)
            {
                ar->SetLocalComputeBaseDelays(NanoSeconds(localComputeBaseLLNs), NanoSeconds(localComputeBaseLL128Ns), NanoSeconds(localComputeBaseSIMPLENs));
            }
            if (interNodeComputeBaseLLNs > 0 || interNodeComputeBaseLL128Ns > 0 || interNodeComputeBaseSIMPLENs > 0)
            {
                ar->SetInterNodeComputeBaseDelays(NanoSeconds(interNodeComputeBaseLLNs), NanoSeconds(interNodeComputeBaseLL128Ns), NanoSeconds(interNodeComputeBaseSIMPLENs));
            }
            if (interNodeBwRampNs > 0 && interNodeBwRampThreshold > 0)
            {
                ar->SetInterNodeBwRamp(NanoSeconds(interNodeBwRampNs), interNodeBwRampThreshold);
            }
            if (fullmeshPerStepLLNs > 0 || fullmeshPerStepLL128Ns > 0 || fullmeshPerStepSIMPLENs > 0)
            {
                ar->SetFullmeshPerStepStartupDelays(NanoSeconds(fullmeshPerStepLLNs), NanoSeconds(fullmeshPerStepLL128Ns), NanoSeconds(fullmeshPerStepSIMPLENs));
            }
            if (interNodeStartupLLNs > 0 || interNodeStartupLL128Ns > 0 || interNodeStartupSIMPLENs > 0)
            {
                ar->SetInterNodeStartupDelays(NanoSeconds(interNodeStartupLLNs), NanoSeconds(interNodeStartupLL128Ns), NanoSeconds(interNodeStartupSIMPLENs));
            }
            if (localStartupLLNs > 0 || localStartupLL128Ns > 0 || localStartupSIMPLENs > 0)
            {
                ar->SetLocalStartupDelays(NanoSeconds(localStartupLLNs), NanoSeconds(localStartupLL128Ns), NanoSeconds(localStartupSIMPLENs));
            }
            if (startupDelayNs > 0)
            {
                ar->SetStartupDelay(NanoSeconds(startupDelayNs));
            }
            // Inter-node startup delay
            if (interNodeStartupNs > 0)
            {
                ar->SetInterNodeStartupDelay(NanoSeconds(interNodeStartupNs));
            }
            else
            {
                ar->SetInterNodeStartupDelay(NanoSeconds(interNodeLatencyNs));
            }
            ar->Start();
            injector = ar;
        }
        else if (algo == "ncclring")
        {
            // Real-NCCL global pipelined ring: C rail-spread rings spanning all
            // ranks, chunk-level pipelining (no per-phase barriers). Effective
            // node geometry derivation mirrors the hierarchical branch.
            uint32_t effectiveNumNodes = numNodes;
            uint32_t effectiveGpusPerNode = gpusPerNode;
            if (topology == "leafspine")
            {
                effectiveNumNodes = leafSwitches;
                effectiveGpusPerNode = numGpus / leafSwitches;
            }
            else if (topology == "nvl72")
            {
                uint32_t gpg = nvl72GpusPerGroup > 0 ? nvl72GpusPerGroup : numGpus / 6;
                effectiveNumNodes = numGpus / gpg;
                effectiveGpusPerNode = gpg;
            }
            else if (numNodes <= 1)
            {
                effectiveNumNodes = 1;
                effectiveGpusPerNode = numGpus;
            }
            Ptr<NcclRingAllReduce> ar = CreateObject<NcclRingAllReduce>();
            ar->SetGpusPerNode(effectiveGpusPerNode);
            ar->SetNumNodes(effectiveNumNodes);
            ar->SetNumChannels(ringChannels);
            ar->SetLlThreshold(llThreshold);
            ar->SetLl128Threshold(ll128Threshold);
            if (startupDelayNs > 0)
            {
                ar->SetStartupDelay(NanoSeconds(startupDelayNs));
            }
            if (perStepSwOverheadNs > 0)
            {
                ar->SetPerStepSwOverhead(NanoSeconds(perStepSwOverheadNs));
            }
            if (swOverheadPerByteNs > 0)
            {
                ar->SetSwOverheadPerByteNs(swOverheadPerByteNs);
            }
            if (computeBaseLLNs > 0 || computeBaseLL128Ns > 0 || computeBaseSIMPLENs > 0)
            {
                ar->SetComputeBaseDelays(NanoSeconds(computeBaseLLNs), NanoSeconds(computeBaseLL128Ns), NanoSeconds(computeBaseSIMPLENs));
            }
            ar->Initialize(numGpus, dataSize, endpoints);
            ar->SetCompletionCallback(MakeCallback(&OnCollectiveComplete));
            ar->Start();
            injector = ar;
        }
        else if (algo == "sharp" || algo == "nvls")
        {
            if (topology != "switched")
            {
                NS_ABORT_MSG("SHARP/NVLS algorithm requires switched topology (NVSwitch)");
            }
            Ptr<SharpAllReduce> ar = CreateObject<SharpAllReduce>();
            ar->Initialize(numGpus, dataSize, endpoints);
            ar->SetCompletionCallback(MakeCallback(&OnCollectiveComplete));
            if (startupDelayNs > 0)
            {
                ar->SetStartupDelay(NanoSeconds(startupDelayNs));
            }
            else if (algo == "nvls")
            {
                ar->SetStartupDelay(NanoSeconds(startupNVLSNs));
            }
            ar->Start();
            injector = ar;
        }
        else if (algo == "tree")
        {
            Ptr<TreeAllReduce> ar = CreateObject<TreeAllReduce>();
            ar->Initialize(numGpus, dataSize, endpoints);
            ar->SetCompletionCallback(MakeCallback(&OnCollectiveComplete));
            if (startupDelayNs > 0)
            {
                ar->SetStartupDelay(NanoSeconds(startupDelayNs));
            }
            if (perStepSwOverheadNs > 0)
            {
                ar->SetPerStepSwOverhead(NanoSeconds(perStepSwOverheadNs));
            }
            if (swOverheadPerByteNs > 0)
            {
                ar->SetSwOverheadPerByteNs(swOverheadPerByteNs);
            }
            ar->Start();
            injector = ar;
        }
        else if (algo == "fullmesh")
        {
            Ptr<FullMeshAllReduce> ar = CreateObject<FullMeshAllReduce>();
            ar->Initialize(numGpus, dataSize, endpoints);
            ar->SetCompletionCallback(MakeCallback(&OnCollectiveComplete));
            if (startupDelayNs > 0)
            {
                ar->SetStartupDelay(NanoSeconds(startupDelayNs));
            }
            ar->Start();
            injector = ar;
        }
        else
        {
            Ptr<RingAllReduce> ar = CreateObject<RingAllReduce>();
            ar->Initialize(numGpus, dataSize, endpoints);
            ar->SetCompletionCallback(MakeCallback(&OnCollectiveComplete));
            if (startupDelayNs > 0)
            {
                ar->SetStartupDelay(NanoSeconds(startupDelayNs));
            }
            if (perStepSwOverheadNs > 0)
            {
                ar->SetPerStepSwOverhead(NanoSeconds(perStepSwOverheadNs));
            }
            if (swOverheadPerByteNs > 0)
            {
                ar->SetSwOverheadPerByteNs(swOverheadPerByteNs);
            }
            if (computeBaseLLNs > 0 || computeBaseLL128Ns > 0 || computeBaseSIMPLENs > 0)
            {
                ar->SetComputeBaseDelays(NanoSeconds(computeBaseLLNs), NanoSeconds(computeBaseLL128Ns), NanoSeconds(computeBaseSIMPLENs));
            }
            ar->Start();
            injector = ar;
        }
    }
    else if (collective == "alltoall")
    {
        if (algo == "hierarchical")
        {
            uint32_t effectiveNumNodes = numNodes;
            uint32_t effectiveGpusPerNode = gpusPerNode;
            if (topology == "leafspine")
            {
                effectiveNumNodes = leafSwitches;
                effectiveGpusPerNode = numGpus / leafSwitches;
            }
            else if (topology == "nvl72")
            {
                uint32_t gpg = nvl72GpusPerGroup > 0 ? nvl72GpusPerGroup : numGpus / 6;
                effectiveNumNodes = numGpus / gpg;
                effectiveGpusPerNode = gpg;
            }
            else if (numNodes <= 1)
            {
                NS_ABORT_MSG("Hierarchical AlltoAll requires numNodes > 1 (or use leafspine/nvl72 topology)");
            }
            Ptr<HierarchicalAlltoAll> a2a = CreateObject<HierarchicalAlltoAll>();
            a2a->SetGpusPerNode(effectiveGpusPerNode);
            a2a->SetNumNodes(effectiveNumNodes);
            a2a->SetIntraNodeAlgorithm(intraNodeAlgorithm);
            a2a->Initialize(numGpus, dataSize, endpoints);
            a2a->SetCompletionCallback(MakeCallback(&OnCollectiveComplete));
            a2a->SetStartupDelays(NanoSeconds(startupLLNs), NanoSeconds(startupLL128Ns), NanoSeconds(startupSIMPLENs));
            a2a->SetStartupPerGpuNs(startupPerGpuNs);
            a2a->SetLlThreshold(llThreshold);
            a2a->SetLl128Threshold(ll128Threshold);
            if (interNodeStartupNs > 0)
            {
                a2a->SetInterNodeStartupDelay(NanoSeconds(interNodeStartupNs));
            }
            else
            {
                a2a->SetInterNodeStartupDelay(NanoSeconds(interNodeLatencyNs));
            }
            if (fullmeshPerStepLLNs > 0 || fullmeshPerStepLL128Ns > 0 || fullmeshPerStepSIMPLENs > 0)
            {
                a2a->SetFullmeshPerStepStartupDelays(NanoSeconds(fullmeshPerStepLLNs), NanoSeconds(fullmeshPerStepLL128Ns), NanoSeconds(fullmeshPerStepSIMPLENs));
            }
            if (interNodeStartupLLNs > 0 || interNodeStartupLL128Ns > 0 || interNodeStartupSIMPLENs > 0)
            {
                a2a->SetInterNodeStartupDelays(NanoSeconds(interNodeStartupLLNs), NanoSeconds(interNodeStartupLL128Ns), NanoSeconds(interNodeStartupSIMPLENs));
            }
            if (localStartupLLNs > 0 || localStartupLL128Ns > 0 || localStartupSIMPLENs > 0)
            {
                a2a->SetLocalStartupDelays(NanoSeconds(localStartupLLNs), NanoSeconds(localStartupLL128Ns), NanoSeconds(localStartupSIMPLENs));
            }
            // --startupDelay/--startupAlltoAll override the protocol-derived
            // local startup, exactly as in the plain AlltoAllInjector branch
            // below (codex round 21, finding 3 — this wiring was missing, and
            // the injector itself ignored the override).
            if (startupAlltoAllNs > 0)
            {
                a2a->SetStartupDelay(NanoSeconds(startupAlltoAllNs));
            }
            else if (startupDelayNs > 0)
            {
                a2a->SetStartupDelay(NanoSeconds(startupDelayNs));
            }
            a2a->Start();
            injector = a2a;
        }
        else
        {
            // collnetdirect/collnetchain are aliases for alltoall on switched topology
            if (algo == "collnetdirect" || algo == "collnetchain")
            {
                if (topology != "switched")
                {
                    NS_ABORT_MSG("CollNet algorithms require switched topology");
                }
            }
            Ptr<AlltoAllInjector> a2a = CreateObject<AlltoAllInjector>();
            a2a->Initialize(numGpus, dataSize, endpoints);
            a2a->SetCompletionCallback(MakeCallback(&OnCollectiveComplete));
            // Use AlltoAll-specific startup if provided, otherwise use protocol startup
            if (startupAlltoAllNs > 0)
            {
                a2a->SetStartupDelay(NanoSeconds(startupAlltoAllNs));
            }
            else if (startupDelayNs > 0)
            {
                a2a->SetStartupDelay(NanoSeconds(startupDelayNs));
            }
            if (topology == "2dfullmesh")
            {
                a2a->SetTwoDimensionalRouting(rackRows, rackCols);
            }
            else
            {
                // Switched/fullmesh topologies issue all destination chunks together.
                bool concurrent = (topology == "switched" || topology == "fullmesh");
                a2a->SetConcurrentMode(concurrent);
            }
            a2a->Start();
            injector = a2a;
        }
    }
    else if (collective == "allgather")
    {
        if (algo == "hierarchical")
        {
            uint32_t effectiveNumNodes = numNodes;
            uint32_t effectiveGpusPerNode = gpusPerNode;
            if (topology == "leafspine")
            {
                effectiveNumNodes = leafSwitches;
                effectiveGpusPerNode = numGpus / leafSwitches;
            }
            else if (topology == "nvl72")
            {
                uint32_t gpg = nvl72GpusPerGroup > 0 ? nvl72GpusPerGroup : numGpus / 6;
                effectiveNumNodes = numGpus / gpg;
                effectiveGpusPerNode = gpg;
            }
            else if (numNodes <= 1)
            {
                NS_ABORT_MSG("Hierarchical AllGather requires numNodes > 1 (or use leafspine/nvl72 topology)");
            }
            Ptr<HierarchicalAllGather> ag = CreateObject<HierarchicalAllGather>();
            ag->SetGpusPerNode(effectiveGpusPerNode);
            ag->SetNumNodes(effectiveNumNodes);
            ag->SetIntraNodeAlgorithm(intraNodeAlgorithm);
            ag->Initialize(numGpus, dataSize, endpoints);
            ag->SetCompletionCallback(MakeCallback(&OnCollectiveComplete));
            // Protocol-aware startup delays (LL/LL128/SIMPLE per-phase)
            ag->SetStartupDelays(NanoSeconds(startupLLNs), NanoSeconds(startupLL128Ns), NanoSeconds(startupSIMPLENs));
            ag->SetStartupPerGpuNs(startupPerGpuNs);
            ag->SetLlThreshold(llThreshold);
            ag->SetLl128Threshold(ll128Threshold);
            if (startupAllGatherNs > 0)
            {
                ag->SetStartupDelay(NanoSeconds(startupAllGatherNs));
            }
            else if (startupDelayNs > 0)
            {
                ag->SetStartupDelay(NanoSeconds(startupDelayNs));
            }
            if (interNodeStartupNs > 0)
            {
                ag->SetInterNodeStartupDelay(NanoSeconds(interNodeStartupNs));
            }
            else
            {
                ag->SetInterNodeStartupDelay(NanoSeconds(interNodeLatencyNs));
            }
            if (interNodeBwRampNs > 0 && interNodeBwRampThreshold > 0)
            {
                ag->SetInterNodeBwRamp(NanoSeconds(interNodeBwRampNs), interNodeBwRampThreshold);
            }
            if (fullmeshPerStepLLNs > 0 || fullmeshPerStepLL128Ns > 0 || fullmeshPerStepSIMPLENs > 0)
            {
                ag->SetFullmeshPerStepStartupDelays(NanoSeconds(fullmeshPerStepLLNs), NanoSeconds(fullmeshPerStepLL128Ns), NanoSeconds(fullmeshPerStepSIMPLENs));
            }
            if (interNodeStartupLLNs > 0 || interNodeStartupLL128Ns > 0 || interNodeStartupSIMPLENs > 0)
            {
                ag->SetInterNodeStartupDelays(NanoSeconds(interNodeStartupLLNs), NanoSeconds(interNodeStartupLL128Ns), NanoSeconds(interNodeStartupSIMPLENs));
            }
            if (localStartupLLNs > 0 || localStartupLL128Ns > 0 || localStartupSIMPLENs > 0)
            {
                ag->SetLocalStartupDelays(NanoSeconds(localStartupLLNs), NanoSeconds(localStartupLL128Ns), NanoSeconds(localStartupSIMPLENs));
            }
            ag->Start();
            injector = ag;
        }
        else if (algo == "nvls")
        {
            if (topology != "switched")
            {
                NS_ABORT_MSG("NVLS AllGather requires switched topology (NVSwitch)");
            }
            Ptr<NvlsAllGather> ag = CreateObject<NvlsAllGather>();
            ag->Initialize(numGpus, dataSize, endpoints);
            ag->SetCompletionCallback(MakeCallback(&OnCollectiveComplete));
            if (startupAllGatherNs > 0)
            {
                ag->SetStartupDelay(NanoSeconds(startupAllGatherNs));
            }
            else if (startupDelayNs > 0)
            {
                ag->SetStartupDelay(NanoSeconds(startupDelayNs));
            }
            else
            {
                ag->SetStartupDelay(NanoSeconds(startupNVLSNs));
            }
            ag->Start();
            injector = ag;
        }
        else if (algo == "fullmesh")
        {
            Ptr<FullMeshAllGather> ag = CreateObject<FullMeshAllGather>();
            ag->Initialize(numGpus, dataSize, endpoints);
            ag->SetCompletionCallback(MakeCallback(&OnCollectiveComplete));
            if (startupAllGatherNs > 0)
            {
                ag->SetStartupDelay(NanoSeconds(startupAllGatherNs));
            }
            else if (startupDelayNs > 0)
            {
                ag->SetStartupDelay(NanoSeconds(startupDelayNs));
            }
            ag->Start();
            injector = ag;
        }
        else
        {
            Ptr<RingAllGather> ag = CreateObject<RingAllGather>();
            ag->Initialize(numGpus, dataSize, endpoints);
            ag->SetCompletionCallback(MakeCallback(&OnCollectiveComplete));
            if (startupAllGatherNs > 0)
            {
                ag->SetStartupDelay(NanoSeconds(startupAllGatherNs));
            }
            else if (startupDelayNs > 0)
            {
                ag->SetStartupDelay(NanoSeconds(startupDelayNs));
            }
            if (perStepSwOverheadNs > 0)
            {
                ag->SetPerStepSwOverhead(NanoSeconds(perStepSwOverheadNs));
            }
            ag->Start();
            injector = ag;
        }
    }
    else if (collective == "reducescatter")
    {
        if (algo == "hierarchical")
        {
            uint32_t effectiveNumNodes = numNodes;
            uint32_t effectiveGpusPerNode = gpusPerNode;
            if (topology == "leafspine")
            {
                effectiveNumNodes = leafSwitches;
                effectiveGpusPerNode = numGpus / leafSwitches;
            }
            else if (topology == "nvl72")
            {
                uint32_t gpg = nvl72GpusPerGroup > 0 ? nvl72GpusPerGroup : numGpus / 6;
                effectiveNumNodes = numGpus / gpg;
                effectiveGpusPerNode = gpg;
            }
            else if (numNodes <= 1)
            {
                NS_ABORT_MSG("Hierarchical ReduceScatter requires numNodes > 1 (or use leafspine topology)");
            }
            Ptr<HierarchicalReduceScatter> rs = CreateObject<HierarchicalReduceScatter>();
            rs->SetGpusPerNode(effectiveGpusPerNode);
            rs->SetNumNodes(effectiveNumNodes);
            rs->SetIntraNodeAlgorithm(intraNodeAlgorithm);
            rs->Initialize(numGpus, dataSize, endpoints);
            rs->SetCompletionCallback(MakeCallback(&OnCollectiveComplete));
            // Protocol-aware startup delays (LL/LL128/SIMPLE per-phase)
            rs->SetStartupDelays(NanoSeconds(startupLLNs), NanoSeconds(startupLL128Ns), NanoSeconds(startupSIMPLENs));
            rs->SetStartupPerGpuNs(startupPerGpuNs);
            rs->SetLlThreshold(llThreshold);
            rs->SetLl128Threshold(ll128Threshold);
            if (localComputeDelayNs > 0)
            {
                rs->SetLocalComputeDelay(NanoSeconds(localComputeDelayNs));
            }
            if (interNodeComputeDelayNs > 0)
            {
                rs->SetInterNodeComputeDelay(NanoSeconds(interNodeComputeDelayNs));
            }
            if (localComputePerByteNs > 0)
            {
                rs->SetLocalComputePerByteNs(localComputePerByteNs);
            }
            if (interNodeComputePerByteNs > 0)
            {
                rs->SetInterNodeComputePerByteNs(interNodeComputePerByteNs);
            }
            if (localComputeBaseLLNs > 0 || localComputeBaseLL128Ns > 0 || localComputeBaseSIMPLENs > 0)
            {
                rs->SetLocalComputeBaseDelays(NanoSeconds(localComputeBaseLLNs), NanoSeconds(localComputeBaseLL128Ns), NanoSeconds(localComputeBaseSIMPLENs));
            }
            if (interNodeComputeBaseLLNs > 0 || interNodeComputeBaseLL128Ns > 0 || interNodeComputeBaseSIMPLENs > 0)
            {
                rs->SetInterNodeComputeBaseDelays(NanoSeconds(interNodeComputeBaseLLNs), NanoSeconds(interNodeComputeBaseLL128Ns), NanoSeconds(interNodeComputeBaseSIMPLENs));
            }
            if (interNodeBwRampNs > 0 && interNodeBwRampThreshold > 0)
            {
                rs->SetInterNodeBwRamp(NanoSeconds(interNodeBwRampNs), interNodeBwRampThreshold);
            }
            if (fullmeshPerStepLLNs > 0 || fullmeshPerStepLL128Ns > 0 || fullmeshPerStepSIMPLENs > 0)
            {
                rs->SetFullmeshPerStepStartupDelays(NanoSeconds(fullmeshPerStepLLNs), NanoSeconds(fullmeshPerStepLL128Ns), NanoSeconds(fullmeshPerStepSIMPLENs));
            }
            if (interNodeStartupLLNs > 0 || interNodeStartupLL128Ns > 0 || interNodeStartupSIMPLENs > 0)
            {
                rs->SetInterNodeStartupDelays(NanoSeconds(interNodeStartupLLNs), NanoSeconds(interNodeStartupLL128Ns), NanoSeconds(interNodeStartupSIMPLENs));
            }
            if (localStartupLLNs > 0 || localStartupLL128Ns > 0 || localStartupSIMPLENs > 0)
            {
                rs->SetLocalStartupDelays(NanoSeconds(localStartupLLNs), NanoSeconds(localStartupLL128Ns), NanoSeconds(localStartupSIMPLENs));
            }
            if (startupDelayNs > 0)
            {
                rs->SetStartupDelay(NanoSeconds(startupDelayNs));
            }
            if (interNodeStartupNs > 0)
            {
                rs->SetInterNodeStartupDelay(NanoSeconds(interNodeStartupNs));
            }
            else
            {
                rs->SetInterNodeStartupDelay(NanoSeconds(interNodeLatencyNs));
            }
            rs->Start();
            injector = rs;
        }
        else if (algo == "fullmesh")
        {
            Ptr<FullMeshReduceScatter> rs = CreateObject<FullMeshReduceScatter>();
            rs->Initialize(numGpus, dataSize, endpoints);
            rs->SetCompletionCallback(MakeCallback(&OnCollectiveComplete));
            if (startupDelayNs > 0)
            {
                rs->SetStartupDelay(NanoSeconds(startupDelayNs));
            }
            rs->Start();
            injector = rs;
        }
        else
        {
            Ptr<RingReduceScatter> rs = CreateObject<RingReduceScatter>();
            rs->Initialize(numGpus, dataSize, endpoints);
            rs->SetCompletionCallback(MakeCallback(&OnCollectiveComplete));
            if (startupDelayNs > 0)
            {
                rs->SetStartupDelay(NanoSeconds(startupDelayNs));
            }
            if (perStepSwOverheadNs > 0)
            {
                rs->SetPerStepSwOverhead(NanoSeconds(perStepSwOverheadNs));
            }
            rs->Start();
            injector = rs;
        }
    }
    else if (collective == "broadcast")
    {
        Ptr<RingBroadcast> bc = CreateObject<RingBroadcast>();
        bc->Initialize(numGpus, dataSize, endpoints);
        bc->SetCompletionCallback(MakeCallback(&OnCollectiveComplete));
        if (startupDelayNs > 0)
        {
            bc->SetStartupDelay(NanoSeconds(startupDelayNs));
        }
        bc->SetRootRank(rootRank);
        bc->Start();
        injector = bc;
    }
    else if (collective == "reduce")
    {
        Ptr<RingReduce> rd = CreateObject<RingReduce>();
        rd->Initialize(numGpus, dataSize, endpoints);
        rd->SetCompletionCallback(MakeCallback(&OnCollectiveComplete));
        if (startupDelayNs > 0)
        {
            rd->SetStartupDelay(NanoSeconds(startupDelayNs));
        }
        rd->SetRootRank(rootRank);
        rd->Start();
        injector = rd;
    }

    else if (collective == "p2p" || collective == "fanout" || collective == "incast")
    {
        // Bare wire-transfer patterns on the credit-gated bulk path (see
        // PatternReceive above). One SendBulkWireTransferSize per flow; the
        // wire protocol is picked from the per-flow size exactly as the ring
        // injectors do (SIMPLE at bulk sizes).
        Ptr<ProtocolModel> protoModel = endpoints[0]->GetProtocolModel();
        uint8_t protocolId = static_cast<uint8_t>(
            protoModel->GetProtocolId(dataSize));
        std::vector<std::pair<uint16_t, uint16_t>> flows;
        if (collective == "p2p")
        {
            flows.emplace_back(rootRank, (rootRank + 1) % numGpus);
        }
        else
        {
            for (uint32_t r = 0; r < numGpus; ++r)
            {
                if (r == rootRank)
                {
                    continue;
                }
                if (collective == "fanout")
                {
                    flows.emplace_back(rootRank, static_cast<uint16_t>(r));
                }
                else  // incast
                {
                    flows.emplace_back(static_cast<uint16_t>(r), rootRank);
                }
            }
        }
        g_patternExpectedBytes = static_cast<uint64_t>(flows.size()) * dataSize;
        g_patternReceivedBytes = 0;
        g_patternStartNs = Simulator::Now().GetNanoSeconds();
        // Zero-byte pattern (codex round 20, finding 10): SendBulkWire-
        // TransferSize emits nothing at dataSize == 0, so no receive ever
        // fires and the pattern stalls to the 1-hour cap. A zero-byte
        // transfer is trivially complete.
        if (dataSize == 0)
        {
            OnCollectiveComplete(0);
        }
        for (auto& ep : endpoints)
        {
            ep->SetReceiveCallback(MakeCallback(&PatternReceive));
        }
        for (uint16_t i = 0; i < flows.size(); ++i)
        {
            Simulator::Schedule(NanoSeconds(0),
                                [src = flows[i].first, dst = flows[i].second,
                                 endpoints, dataSize, protocolId, i]()
                                {
                                    endpoints[src]->SendBulkWireTransferSize(
                                        dst, dataSize,
                                        protocolId, static_cast<uint16_t>(i + 1), 0);
                                });
        }
    }

    // Background P2P interferer: a self-driven P2pTraffic flow that shares the
    // fabric with the primary collective. Scheduled at bgP2pStartTimeNs so it
    // can overlap (or precede) the collective, exercising multi-class WFQ
    // contention at the endpoint injection side and So switch VOQ arbitration.
    // Unlimited (maxPackets=0): the sim stops via OnCollectiveComplete when the
    // collective finishes, terminating the background flow alongside it.
    // Declared in main scope so it outlives Simulator::Run() for the report below.
    Ptr<P2pTraffic> bgP2p;
    if (bgP2pEnabled
        && bgP2pSrc < endpoints.size() && bgP2pDst < endpoints.size()
        && bgP2pSrc != bgP2pDst
        && endpoints[bgP2pSrc] && endpoints[bgP2pDst])
    {
        bgP2p = CreateObject<P2pTraffic>();
        bgP2p->SetEndpoint(endpoints[bgP2pSrc]);
        bgP2p->SetDestRank(static_cast<uint16_t>(bgP2pDst));
        bgP2p->SetPacketSize(bgP2pSizeBytes);
        uint64_t intervalNs = bgP2pIntervalNs;
        if (intervalNs == 0)
        {
            // back-to-back = one packet serialization time at the intra-node
            // bandwidth, NOT a literal 0 ns: P2pTraffic::ScheduleNext schedules
            // the next burst at Now()+interval, so a 0 interval would loop
            // forever at a fixed sim-time and starve every later event (incl.
            // the collective completion). ns = bytes*8 / Gbps, using the
            // intra-node rate even for a cross-node flow (a So flow is then
            // over-driven by the intra/inter ratio and queues at the NIC; pass
            // --bgP2pIntervalNs explicitly to pace at the inter-node rate).
            // Integer division floors sub-ns serialization (<100 B at 800G) to
            // 0, which would hit the same stall — clamp to >= 1 ns.
            NS_ABORT_MSG_IF(bandwidthGbps == 0,
                            "bgP2pIntervalNs=0 needs bandwidthGbps>0 to derive a serialization interval");
            intervalNs = std::max<uint64_t>(1, bgP2pSizeBytes * 8ULL / bandwidthGbps);
        }
        bgP2p->SetInterval(NanoSeconds(intervalNs));
        bgP2p->SetMaxPackets(0);  // 0 = unlimited background flow
        Simulator::Schedule(NanoSeconds(bgP2pStartTimeNs), &P2pTraffic::Start, bgP2p);
    }


    Simulator::Stop(Seconds(3600));
    Simulator::Run();

    double durationUs;
    std::string statusStr;
    if (g_collectiveCompleted) {
        durationUs = g_durationNs / 1000.0;
        statusStr = "complete";
    } else if (g_randTrafficRan) {
        // Fixed-duration random-traffic run: completing the measurement window
        // IS the success criterion (there is no collective to finish).
        durationUs = Simulator::Now().GetMicroSeconds();
        statusStr = "complete";
    } else {
        durationUs = Simulator::Now().GetMicroSeconds();
        statusStr = "incomplete";
    }

    // Print machine-parseable result
    std::cout << "RESULT_START" << std::endl;
    std::cout << "status=" << statusStr << std::endl;
    std::cout << "topology=" << topology << std::endl;
    std::cout << "collective=" << collective << std::endl;
    std::cout << "algorithm=" << algorithm << std::endl;
    std::cout << "protocolModel=" << protocolModelType << std::endl;
    std::cout << "transactionModel=" << (useTransactionModel ? "enabled" : "legacy")
              << std::endl;
    std::cout << "fabricType=" << fabricTypeStr << std::endl;
    std::cout << "deviceType=" << deviceTypeStr << std::endl;
    std::cout << "numGpus=" << numGpus << std::endl;
    std::cout << "dataSize=" << dataSize << std::endl;
    std::cout << "bandwidthGbps=" << bandwidthGbps << std::endl;
    std::cout << "latencyNs=" << latencyNs << std::endl;
    std::cout << "opticalInterSwitchLinks=" << opticalInterSwitchLinks << std::endl;
    std::cout << "operationalOpticalInterSwitchLinks="
              << operationalOpticalInterSwitchLinks << std::endl;
    std::cout << "operationalOpticalBerMin=" << operationalOpticalBerMin << std::endl;
    std::cout << "operationalOpticalBerMax=" << operationalOpticalBerMax << std::endl;
    std::cout << "failedOpticalLink=" << failedOpticalLink << std::endl;
    std::cout << "failureAwareRouting="
              << ((failureAwareRouting || failedOpticalLink >= 0) ? 1 : 0)
              << std::endl;
    std::cout << "failedOpticalLinkDescription="
              << failedOpticalLinkDescription << std::endl;
    std::cout << "failedOpticalLinkBer=" << failedOpticalLinkBer << std::endl;
    std::cout << "failureRoutesComplete="
              << (failureRoutesComplete ? 1 : 0) << std::endl;
    std::cout << "failureUnreachableGpuPairs="
              << failureUnreachableGpuPairs << std::endl;
    std::cout << "startupDelayNs=" << startupDelayNs << std::endl;
    std::cout << "startupLLNs=" << startupLLNs << std::endl;
    std::cout << "startupLL128Ns=" << startupLL128Ns << std::endl;
    std::cout << "startupSIMPLENs=" << startupSIMPLENs << std::endl;
    std::cout << "startupNVLSNs=" << startupNVLSNs << std::endl;
    std::cout << "llThreshold=" << llThreshold << std::endl;
    std::cout << "ll128Threshold=" << ll128Threshold << std::endl;
    if (numNodes > 1)
    {
        std::cout << "numNodes=" << numNodes << std::endl;
        std::cout << "gpusPerNode=" << gpusPerNode << std::endl;
        std::cout << "interNodeTopology=" << interNodeTopology << std::endl;
        std::cout << "interNodeBandwidthGbps=" << interNodeBandwidthGbps << std::endl;
        std::cout << "interNodeLatencyNs=" << interNodeLatencyNs << std::endl;
        std::cout << "interNodeFabricType=" << interNodeFabricTypeStr << std::endl;
        std::cout << "interNodeStartupNs=" << interNodeStartupNs << std::endl;
    }
    std::cout << std::fixed << std::setprecision(1);
    std::cout << "simTimeUs=" << durationUs << std::endl;
    std::cout << "simTimeNs="
              << (g_collectiveCompleted
                      ? g_durationNs
                      : static_cast<uint64_t>(Simulator::Now().GetNanoSeconds()))
              << std::endl;
    if (!g_collectiveCompleted)
    {
        Ptr<RingAllGather> allGather = DynamicCast<RingAllGather>(injector);
        if (allGather)
        {
            std::cout << "ringAllGatherProgress=";
            const auto& steps = allGather->GetGpuCompletedSteps();
            const auto& bytes = allGather->GetGpuStepReceivedBytes();
            for (uint32_t gpu = 0; gpu < steps.size(); ++gpu)
            {
                std::cout << (gpu == 0 ? "" : ",")
                          << gpu << ":" << steps[gpu] << ":" << bytes[gpu];
            }
            std::cout << std::endl;
        }
        Ptr<RingReduceScatter> reduceScatter =
            DynamicCast<RingReduceScatter>(injector);
        if (reduceScatter)
        {
            std::cout << "ringReduceScatterProgress=";
            const auto& steps = reduceScatter->GetGpuCompletedSteps();
            const auto& bytes = reduceScatter->GetGpuStepReceivedBytes();
            for (uint32_t gpu = 0; gpu < steps.size(); ++gpu)
            {
                std::cout << (gpu == 0 ? "" : ",")
                          << gpu << ":" << steps[gpu] << ":" << bytes[gpu];
            }
            std::cout << std::endl;
        }
        Ptr<TreeAllReduce> tree = DynamicCast<TreeAllReduce>(injector);
        if (tree)
        {
            std::vector<uint32_t> stepCounts(tree->GetTotalSteps() + 1, 0);
            for (uint32_t step : tree->GetGpuCurrentSteps())
            {
                stepCounts[std::min(step, tree->GetTotalSteps())]++;
            }
            std::cout << "treeCompletedGpus=" << tree->GetCompletedGpuCount() << std::endl;
            std::cout << "treeStepHistogram=";
            for (uint32_t step = 0; step < stepCounts.size(); ++step)
            {
                if (stepCounts[step] == 0) continue;
                std::cout << (step == 0 ? "" : ",") << step << ":" << stepCounts[step];
            }
            std::cout << std::endl;
        }
    }
    // Aggregate reorder buffer statistics across all endpoints
    uint32_t totalReorderEvents = 0;
    uint32_t maxReorderOccupancy = 0;
    for (auto& ep : endpoints)
    {
        uint32_t events = 0;
        uint32_t maxOcc = 0;
        ep->GetReorderBufferStats(events, maxOcc);
        totalReorderEvents += events;
        if (maxOcc > maxReorderOccupancy)
        {
            maxReorderOccupancy = maxOcc;
        }
    }
    std::cout << "reorderEvents=" << totalReorderEvents << std::endl;
    std::cout << "reorderMaxOccupancy=" << maxReorderOccupancy << std::endl;
    uint64_t endpointTxPackets = 0;
    uint64_t endpointRxPackets = 0;
    uint64_t cnpPacketsSent = 0;
    uint64_t cnpPacketsReceived = 0;
    for (const auto& ep : endpoints)
    {
        endpointTxPackets += ep->GetTxPackets();
        endpointRxPackets += ep->GetRxPackets();
        cnpPacketsSent += ep->GetCnpPacketsSent();
        cnpPacketsReceived += ep->GetCnpPacketsReceived();
    }
    uint64_t switchTxPackets = 0;
    uint64_t switchRxPackets = 0;
    uint64_t switchDroppedPackets = 0;
    uint64_t switchUnknownPortDrops = 0;
    uint64_t switchLinkErrorDrops = 0;
    uint64_t switchTtlDrops = 0;
    uint64_t switchVoqDrops = 0;
    uint64_t switchRouteUnavailableDrops = 0;
    uint64_t ecnMarkedPackets = 0;
    uint64_t pfcPausesSent = 0;
    uint64_t pfcResumesSent = 0;
    for (auto node = NodeList::Begin(); node != NodeList::End(); ++node)
    {
        for (uint32_t device = 0; device < (*node)->GetNDevices(); ++device)
        {
            Ptr<NvSwitch> nvSwitch = DynamicCast<NvSwitch>((*node)->GetDevice(device));
            if (!nvSwitch) continue;
            switchTxPackets += nvSwitch->GetTxPackets();
            switchRxPackets += nvSwitch->GetRxPackets();
            switchDroppedPackets += nvSwitch->GetDroppedPackets();
            switchUnknownPortDrops += nvSwitch->GetUnknownPortDrops();
            switchLinkErrorDrops += nvSwitch->GetLinkErrorDrops();
            switchTtlDrops += nvSwitch->GetTtlDrops();
            switchVoqDrops += nvSwitch->GetVoqDrops();
            switchRouteUnavailableDrops += nvSwitch->GetRouteUnavailableDrops();
            Ptr<EthernetSwitch> ethSwitch = DynamicCast<EthernetSwitch>(nvSwitch);
            if (ethSwitch)
            {
                ecnMarkedPackets += ethSwitch->GetEcnMarkedPackets();
                pfcPausesSent += ethSwitch->GetPfcPausesSent();
                pfcResumesSent += ethSwitch->GetPfcResumesSent();
            }
        }
    }
    std::cout << "endpointTxPackets=" << endpointTxPackets << std::endl;
    std::cout << "endpointRxPackets=" << endpointRxPackets << std::endl;
    std::cout << "switchTxPackets=" << switchTxPackets << std::endl;
    std::cout << "switchRxPackets=" << switchRxPackets << std::endl;
    std::cout << "switchDroppedPackets=" << switchDroppedPackets << std::endl;
    std::cout << "switchUnknownPortDrops=" << switchUnknownPortDrops << std::endl;
    std::cout << "switchLinkErrorDrops=" << switchLinkErrorDrops << std::endl;
    std::cout << "switchTtlDrops=" << switchTtlDrops << std::endl;
    std::cout << "switchVoqDrops=" << switchVoqDrops << std::endl;
    std::cout << "switchRouteUnavailableDrops="
              << switchRouteUnavailableDrops << std::endl;
    std::cout << "pfcPausesSent=" << pfcPausesSent << std::endl;
    std::cout << "pfcResumesSent=" << pfcResumesSent << std::endl;
    uint64_t retransmittedPackets = 0;
    uint64_t retryBufferOverflows = 0;
    uint64_t permanentLosses = 0;
    uint64_t retryMisses = 0;
    uint64_t retryBufferCurrent = 0;
    uint32_t retryBufferPeak = 0;
    uint64_t retrySourceReloads = 0;
    uint64_t retrySourceReloadBytes = 0;
    uint64_t retrySourceServiceNs = 0;
    for (auto& ep : endpoints)
    {
        Ptr<LlrManager> llr = ep->GetLlrManager();
        if (!llr) continue;
        retransmittedPackets += llr->GetRetransmittedPackets();
        retryBufferOverflows += llr->GetOverflowCount();
        permanentLosses += llr->GetPermanentLossCount();
        retryMisses += llr->GetRetryMissCount();
        retryBufferCurrent += llr->GetBufferSize();
        retryBufferPeak = std::max(retryBufferPeak, llr->GetPeakBufferSize());
        retrySourceReloads += llr->GetSourceReloadCount();
        retrySourceReloadBytes += llr->GetSourceReloadBytes();
        retrySourceServiceNs += llr->GetSourceReloadServiceTime().GetNanoSeconds();
    }
    std::cout << "retransmittedPackets=" << retransmittedPackets << std::endl;
    std::cout << "retryBufferOverflows=" << retryBufferOverflows << std::endl;
    std::cout << "retryBufferPeak=" << retryBufferPeak << std::endl;
    std::cout << "retryBufferCurrent=" << retryBufferCurrent << std::endl;
    std::cout << "retryMisses=" << retryMisses << std::endl;
    std::cout << "retrySourceReloads=" << retrySourceReloads << std::endl;
    std::cout << "retrySourceReloadBytes=" << retrySourceReloadBytes << std::endl;
    std::cout << "retrySourceServiceNs=" << retrySourceServiceNs << std::endl;
    std::cout << "permanentLosses=" << permanentLosses << std::endl;
    std::cout << "fecNoError=" << (fecModel ? fecModel->GetNoErrorCount() : 0) << std::endl;
    std::cout << "fecCorrectable=" << (fecModel ? fecModel->GetCorrectableCount() : 0) << std::endl;
    std::cout << "fecUncorrectable=" << (fecModel ? fecModel->GetUncorrectableCount() : 0) << std::endl;
    std::cout << "bgP2pEnabled=" << (bgP2pEnabled ? "true" : "false") << std::endl;
    std::cout << "rttccEnabled=" << (enableRttcc ? "true" : "false") << std::endl;
    if (!rttccMachines.empty())
    {
        uint64_t rttccProbesSent = 0;
        uint64_t rttccRttSamples = 0;
        int64_t rttccMinRttNs = std::numeric_limits<int64_t>::max();
        int64_t rttccMaxRttNs = 0;
        double rttccMinRateGbps = std::numeric_limits<double>::max();
        for (const auto& rm : rttccMachines)
        {
            rttccProbesSent += rm->GetProbesSent();
            rttccRttSamples += rm->GetRttSamples();
            rttccMinRttNs = std::min(rttccMinRttNs, rm->GetMinRttNs());
            rttccMaxRttNs = std::max(rttccMaxRttNs, rm->GetMaxRttNs());
            rttccMinRateGbps = std::min(rttccMinRateGbps, rm->GetMinFlowRateGbps());
        }
        std::cout << "rttccProbesSent=" << rttccProbesSent << std::endl;
        std::cout << "rttccRttSamples=" << rttccRttSamples << std::endl;
        std::cout << "rttccMinRttNs="
                  << (rttccRttSamples > 0 ? rttccMinRttNs : 0) << std::endl;
        std::cout << "rttccMaxRttNs=" << rttccMaxRttNs << std::endl;
        std::cout << "rttccMinRateGbps=" << rttccMinRateGbps << std::endl;
    }
    if (bgP2p)
    {
        std::cout << "bgP2pBytesSent=" << bgP2p->GetBytesSent() << std::endl;
        std::cout << "bgP2pPacketsSent=" << bgP2p->GetPacketsSent() << std::endl;
    }
    std::cout << "ecnMarkedPackets=" << ecnMarkedPackets << std::endl;
    std::cout << "cnpPacketsSent=" << cnpPacketsSent << std::endl;
    std::cout << "cnpPacketsReceived=" << cnpPacketsReceived << std::endl;
    std::cout << "randTrafficEnabled=" << (randTrafficEnabled ? "true" : "false") << std::endl;
    if (randTrafficEnabled)
    {
        double measureSec = static_cast<double>(g_randWindowEndNs - g_randWindowStartNs) * 1e-9;
        std::cout << "randMsgsSent=" << g_randMsgsSent << std::endl;
        std::cout << "randOfferedBytes=" << g_randOfferedBytes << std::endl;
        std::cout << "randDeliveredBytes=" << g_randDeliveredBytes << std::endl;
        std::cout << std::setprecision(3);
        std::cout << "randOfferedGbps="
                  << (measureSec > 0 ? g_randOfferedBytes * 8.0 / measureSec * 1e-9 : 0)
                  << std::endl;
        std::cout << "randDeliveredGbps="
                  << (measureSec > 0 ? g_randDeliveredBytes * 8.0 / measureSec * 1e-9 : 0)
                  << std::endl;
    }
    std::cout << "RESULT_END" << std::endl;

    Simulator::Destroy();
    return 0;
}
