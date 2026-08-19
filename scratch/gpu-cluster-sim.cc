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
#include "ns3/multi-node-topology-helper.h"
#include "ns3/device-type.h"
#include "ns3/fabric-type.h"
#include "ns3/protocol-model.h"
#include "ns3/protocol-profile.h"
#include "ns3/protocol-config.h"
#include "ns3/protocol-config-runner.h"
#include "ns3/nccl-protocol-model.h"
#include "ns3/ub-protocol-model.h"
#include "ns3/protocol-payload-builder.h"
#include "ns3/nccl-protocol-payload-builder.h"
#include "ns3/ub-payload-builder.h"

#include "ns3/link-degradation.h"
#include "ns3/fec-model.h"
#include "ns3/llr-manager.h"
#include "ns3/contention-model.h"
#include "ns3/point-to-point-helper.h"
#include "ns3/point-to-point-net-device.h"
#include "ns3/point-to-point-channel.h"
#include "ns3/drop-tail-queue.h"

#include <algorithm>
#include <iostream>
#include <iomanip>
#include <queue>
#include <unordered_map>
#include <vector>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("GpuClusterSim");

static uint64_t g_durationNs = 0;
static bool g_collectiveCompleted = false;

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
    uint64_t interNodeBandwidthGbps = 400;
    uint64_t interNodeLatencyNs = 10000;
    std::string interNodeFabricTypeStr = "RoCE";
    uint64_t interNodeStartupNs = 40000;
    std::string intraNodeTopology = "switched";
    std::string intraNodeAlgorithm = "fullmesh";


    CommandLine cmd;
    cmd.AddValue("topology", "Topology: ring/fullmesh/switched/fattree/railfattree/leafspine/nvl72/torus/mesh/hypercube/2dfullmesh/ndfullmesh/2dfullmeshclos", topology);
    cmd.AddValue("collective", "Collective: allreduce/alltoall/allgather/reducescatter/broadcast/reduce", collective);
    cmd.AddValue("algorithm", "Algorithm: ring/sharp/nvls/tree/fullmesh/collnetdirect/collnetchain", algorithm);
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
    cmd.AddValue("numNodes", "Number of GPU nodes (1=single-node, 2+=multi-node)", numNodes);
    cmd.AddValue("gpusPerNode", "GPUs per node (multi-node mode)", gpusPerNode);
    cmd.AddValue("interNodeTopology", "Inter-node topology: fullmesh/ring/host", interNodeTopology);
    cmd.AddValue("interNodeBandwidth", "Inter-node link bandwidth in Gbps", interNodeBandwidthGbps);
    cmd.AddValue("interNodeLatency", "Inter-node link latency in nanoseconds", interNodeLatencyNs);
    cmd.AddValue("interNodeFabricType", "Inter-node fabric type: RoCE/ETH/NVLink", interNodeFabricTypeStr);
    cmd.AddValue("interNodeStartup", "Inter-node phase startup delay in ns", interNodeStartupNs);
    cmd.AddValue("intraNodeTopology", "Intra-node topology: switched/fullmesh", intraNodeTopology);
    cmd.AddValue("intraNodeAlgorithm", "Intra-node algorithm for hierarchical: ring/fullmesh", intraNodeAlgorithm);
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
        collective != "p2p")
    {
        NS_ABORT_MSG("Unknown collective: " << collective << ". Supported: allreduce, alltoall, allgather, reducescatter, broadcast, reduce, p2p");
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
        if (pbundle.fec) { fecModel = pbundle.fec; }
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
        if (switchCutThroughDelayNs > 0) { multiTopo.SetSwitchCutThroughDelay(switchCutThroughDelayNs); }
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
        multiTopo.SetInterNodeTopology(interNodeTopology);
        multiTopo.SetIntraNodeTopology(intraNodeTopology);

        NodeContainer nodes = multiTopo.Build();
        multiTopo.PopulateInterNodeRouting();

        ApplicationContainer apps = multiTopo.GetAllEndpoints();
        for (uint32_t i = 0; i < apps.GetN(); ++i)
        {
            Ptr<FabricEndpoint> ep = DynamicCast<FabricEndpoint>(apps.Get(i));
            ep->SetNumVirtualChannels(1);
            ep->SetVcCredits(0, effectiveVcCredits);
            ep->SetBypassReorderBuffer(true);
            ep->SetProtocolModel(protoModel);
            ep->SetProtocolPayloadBuilder(payloadBuilder);
            ep->SetBulkChunkSize(bulkChunkSize);
            ep->SetDeviceType(deviceType);

            if (syncMemLatencyNs > 0) { ep->SetSyncMemLatencyNs(syncMemLatencyNs); }
            if (asyncMemLatencyNs > 0) { ep->SetAsyncMemLatencyNs(asyncMemLatencyNs); }
            ep->SetLlrEnabled(llrEnabled);

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

        // Contention model: configured once, shared via helper for GPU endpoints
        // and manually attached to memory endpoints (created outside the helper)
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
    if (configMode && pconfig.GetCollective().empty() && pconfig.GetAlgorithm().empty())
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


    Simulator::Stop(Seconds(3600));
    Simulator::Run();

    double durationUs;
    std::string statusStr;
    if (g_collectiveCompleted) {
        durationUs = g_durationNs / 1000.0;
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
    for (const auto& ep : endpoints)
    {
        endpointTxPackets += ep->GetTxPackets();
        endpointRxPackets += ep->GetRxPackets();
    }
    uint64_t switchTxPackets = 0;
    uint64_t switchRxPackets = 0;
    uint64_t switchDroppedPackets = 0;
    uint64_t switchUnknownPortDrops = 0;
    uint64_t switchLinkErrorDrops = 0;
    uint64_t switchTtlDrops = 0;
    uint64_t switchVoqDrops = 0;
    uint64_t switchRouteUnavailableDrops = 0;
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
    std::cout << "RESULT_END" << std::endl;

    Simulator::Destroy();
    return 0;
}
