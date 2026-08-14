/*
 * hierarchical-allreduce.cc
 *
 * Hierarchical AllReduce implementation for multi-node GPU clusters.
 */

#include "hierarchical-allreduce.h"
#include "ns3/log.h"
#include "ns3/simulator.h"
#include "ns3/nccl-protocol.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("HierarchicalAllReduce");

NS_OBJECT_ENSURE_REGISTERED(HierarchicalAllReduce);

TypeId
HierarchicalAllReduce::GetTypeId()
{
    static TypeId tid = TypeId("ns3::HierarchicalAllReduce")
        .SetParent<CollectiveInjector>()
        .SetGroupName("GpuCluster");
    return tid;
}

HierarchicalAllReduce::HierarchicalAllReduce()
    : m_numGpus(0),
      m_dataSize(0),
      m_flowId(0),
      m_protocolId(static_cast<uint8_t>(NcclProtocol::SIMPLE)),
      m_gpusPerNode(8),
      m_numNodes(1),
      m_intraNodeAlgorithm("ring"),
      m_segmentSize(0),
      m_interNodeChunk(0),
      m_state(HierarchicalState::IDLE),
      m_currentPhase(HierarchicalPhase::LOCAL_REDUCESCATTER),
      m_startupDelayNs(0),
      m_interNodeStartupDelayNs(0),
      m_interNodeStartupLLNs(0),
      m_interNodeStartupLL128Ns(0),
      m_interNodeStartupSIMPLENs(0),
      m_localStartupLLNs(0),
      m_localStartupLL128Ns(0),
      m_localStartupSIMPLENs(0),
      m_startupLLNs(5000),
      m_startupLL128Ns(5000),
      m_startupSIMPLENs(10000),
      m_startupPerGpuNs(750),
      m_llThreshold(8192),
      m_ll128Threshold(2 * 1024 * 1024),
      m_localComputeDelayNs(0),
      m_interNodeComputeDelayNs(0),
      m_localComputePerByteNs(0),
      m_interNodeComputePerByteNs(0),
      m_localComputeBaseLLNs(0),
      m_localComputeBaseLL128Ns(0),
      m_localComputeBaseSIMPLENs(0),
      m_interNodeComputeBaseLLNs(0),
      m_interNodeComputeBaseLL128Ns(0),
      m_interNodeComputeBaseSIMPLENs(0),
      m_interNodeBwRampDelayNs(0),
      m_interNodeBwRampThreshold(0),
      m_fullmeshPerStepLLNs(0),
      m_fullmeshPerStepLL128Ns(0),
      m_fullmeshPerStepSIMPLENs(0),
      m_completedGpuCount(0),
      m_startTimeNs(0),
      m_endTimeNs(0)
{}

HierarchicalAllReduce::~HierarchicalAllReduce()
{}

void HierarchicalAllReduce::SetGpusPerNode(uint32_t gpusPerNode) { m_gpusPerNode = gpusPerNode; }
void HierarchicalAllReduce::SetNumNodes(uint32_t numNodes) { m_numNodes = numNodes; }
void HierarchicalAllReduce::SetNodeIdForRank(uint16_t rank, uint32_t nodeId) { m_rankToNodeId[rank] = nodeId; }
void HierarchicalAllReduce::SetIntraNodeAlgorithm(const std::string& algo) { m_intraNodeAlgorithm = algo; }
void HierarchicalAllReduce::SetInterNodeStartupDelay(Time delay) { m_interNodeStartupDelayNs = delay.GetNanoSeconds(); }
void HierarchicalAllReduce::SetInterNodeStartupDelays(Time ll, Time ll128, Time simple)
{
    m_interNodeStartupLLNs = ll.GetNanoSeconds();
    m_interNodeStartupLL128Ns = ll128.GetNanoSeconds();
    m_interNodeStartupSIMPLENs = simple.GetNanoSeconds();
}
uint64_t HierarchicalAllReduce::GetInterNodeStartupDelayNs(uint64_t chunkSize) const
{
    // If explicit protocol-aware inter-node startup is set, use it
    if (m_interNodeStartupLLNs > 0 || m_interNodeStartupLL128Ns > 0 || m_interNodeStartupSIMPLENs > 0)
    {
        if (chunkSize < m_llThreshold)
            return m_interNodeStartupLLNs;
        else if (chunkSize < m_ll128Threshold)
            return m_interNodeStartupLL128Ns;
        else
            return m_interNodeStartupSIMPLENs;
    }
    // Fallback: use fixed interNodeStartup or protocol-aware global startup
    if (m_interNodeStartupDelayNs > 0)
        return m_interNodeStartupDelayNs;
    return GetPhaseStartupDelayNs(chunkSize);
}
void HierarchicalAllReduce::SetLocalStartupDelays(Time ll, Time ll128, Time simple)
{
    m_localStartupLLNs = ll.GetNanoSeconds();
    m_localStartupLL128Ns = ll128.GetNanoSeconds();
    m_localStartupSIMPLENs = simple.GetNanoSeconds();
}
uint64_t HierarchicalAllReduce::GetLocalStartupDelayNs(uint64_t chunkSize) const
{
    if (m_localStartupLLNs > 0 || m_localStartupLL128Ns > 0 || m_localStartupSIMPLENs > 0)
    {
        if (chunkSize < m_llThreshold)
            return m_localStartupLLNs;
        else if (chunkSize < m_ll128Threshold)
            return m_localStartupLL128Ns;
        else
            return m_localStartupSIMPLENs;
    }
    return GetPhaseStartupDelayNs(chunkSize);
}
void HierarchicalAllReduce::SetStartupDelays(Time ll, Time ll128, Time simple)
{
    m_startupLLNs = ll.GetNanoSeconds();
    m_startupLL128Ns = ll128.GetNanoSeconds();
    m_startupSIMPLENs = simple.GetNanoSeconds();
}
void HierarchicalAllReduce::SetStartupPerGpuNs(uint64_t perGpuNs) { m_startupPerGpuNs = perGpuNs; }
void HierarchicalAllReduce::SetLlThreshold(uint64_t threshold) { m_llThreshold = threshold; }
void HierarchicalAllReduce::SetLl128Threshold(uint64_t threshold) { m_ll128Threshold = threshold; }
void HierarchicalAllReduce::SetLocalComputeDelay(Time delay) { m_localComputeDelayNs = delay.GetNanoSeconds(); }
void HierarchicalAllReduce::SetInterNodeComputeDelay(Time delay) { m_interNodeComputeDelayNs = delay.GetNanoSeconds(); }
void HierarchicalAllReduce::SetLocalComputePerByteNs(uint64_t perByteNs) { m_localComputePerByteNs = perByteNs; }
void HierarchicalAllReduce::SetInterNodeComputePerByteNs(uint64_t perByteNs) { m_interNodeComputePerByteNs = perByteNs; }
void HierarchicalAllReduce::SetLocalComputeBaseDelays(Time ll, Time ll128, Time simple)
{
    m_localComputeBaseLLNs = ll.GetNanoSeconds();
    m_localComputeBaseLL128Ns = ll128.GetNanoSeconds();
    m_localComputeBaseSIMPLENs = simple.GetNanoSeconds();
}
void HierarchicalAllReduce::SetInterNodeComputeBaseDelays(Time ll, Time ll128, Time simple)
{
    m_interNodeComputeBaseLLNs = ll.GetNanoSeconds();
    m_interNodeComputeBaseLL128Ns = ll128.GetNanoSeconds();
    m_interNodeComputeBaseSIMPLENs = simple.GetNanoSeconds();
}
uint64_t HierarchicalAllReduce::GetLocalComputeDelayNs(uint64_t chunkSize) const
{
    if (m_localComputePerByteNs > 0 ||
        m_localComputeBaseLLNs > 0 || m_localComputeBaseLL128Ns > 0 || m_localComputeBaseSIMPLENs > 0)
    {
        uint64_t baseDelay;
        if (chunkSize < m_llThreshold)
            baseDelay = m_localComputeBaseLLNs;
        else if (chunkSize < m_ll128Threshold)
            baseDelay = m_localComputeBaseLL128Ns;
        else
            baseDelay = m_localComputeBaseSIMPLENs;
        return baseDelay + m_localComputePerByteNs * chunkSize;
    }
    return m_localComputeDelayNs;
}
uint64_t HierarchicalAllReduce::GetInterNodeComputeDelayNs(uint64_t chunkSize) const
{
    if (m_interNodeComputePerByteNs > 0 ||
        m_interNodeComputeBaseLLNs > 0 || m_interNodeComputeBaseLL128Ns > 0 || m_interNodeComputeBaseSIMPLENs > 0)
    {
        uint64_t baseDelay;
        if (chunkSize < m_llThreshold)
            baseDelay = m_interNodeComputeBaseLLNs;
        else if (chunkSize < m_ll128Threshold)
            baseDelay = m_interNodeComputeBaseLL128Ns;
        else
            baseDelay = m_interNodeComputeBaseSIMPLENs;
        return baseDelay + m_interNodeComputePerByteNs * chunkSize;
    }
    return m_interNodeComputeDelayNs;
}
void HierarchicalAllReduce::SetInterNodeBwRamp(Time rampDelay, uint64_t rampThresholdBytes)
{
    m_interNodeBwRampDelayNs = rampDelay.GetNanoSeconds();
    m_interNodeBwRampThreshold = rampThresholdBytes;
}
uint64_t HierarchicalAllReduce::GetInterNodeRampDelayNs(uint64_t chunkSize) const
{
    if (m_interNodeBwRampDelayNs == 0 || m_interNodeBwRampThreshold == 0) return 0;
    return m_interNodeBwRampDelayNs / (1 + chunkSize / m_interNodeBwRampThreshold);
}
void HierarchicalAllReduce::SetFullmeshPerStepStartupDelays(Time ll, Time ll128, Time simple)
{
    m_fullmeshPerStepLLNs = ll.GetNanoSeconds();
    m_fullmeshPerStepLL128Ns = ll128.GetNanoSeconds();
    m_fullmeshPerStepSIMPLENs = simple.GetNanoSeconds();
}
uint64_t HierarchicalAllReduce::GetFullmeshStepDelayNs(uint64_t chunkSize) const
{
    if (m_intraNodeAlgorithm != "fullmesh") return 0;
    // Use same LL/LL128/SIMPLE protocol thresholds as GetPhaseStartupDelayNs
    uint64_t perStepDelay;
    if (chunkSize < m_llThreshold)
    {
        perStepDelay = m_fullmeshPerStepLLNs;
    }
    else if (chunkSize < m_ll128Threshold)
    {
        perStepDelay = m_fullmeshPerStepLL128Ns;
    }
    else
    {
        perStepDelay = m_fullmeshPerStepSIMPLENs;
    }
    if (perStepDelay == 0) return 0;
    return (m_gpusPerNode - 1) * perStepDelay;
}

uint64_t
HierarchicalAllReduce::GetPhaseStartupDelayNs(uint64_t chunkSize) const
{
    uint64_t baseDelay;
    if (chunkSize < m_llThreshold)
    {
        baseDelay = m_startupLLNs;
    }
    else if (chunkSize < m_ll128Threshold)
    {
        baseDelay = m_startupLL128Ns;
    }
    else
    {
        baseDelay = m_startupSIMPLENs;
    }
    return baseDelay + m_startupPerGpuNs * m_numGpus;
}

uint32_t HierarchicalAllReduce::GetNodeId(uint16_t rank) const
{
    auto it = m_rankToNodeId.find(rank);
    if (it != m_rankToNodeId.end()) return it->second;
    return rank / m_gpusPerNode;
}

bool HierarchicalAllReduce::IsLocalRank(uint16_t rank) const
{
    if (m_endpoints.empty()) return true;
    Ptr<FabricEndpoint> ep = m_endpoints[0];
    return ep->IsLocalRank(rank);
}

uint16_t HierarchicalAllReduce::GetLocalIdx(uint16_t globalRank) const
{
    return globalRank % m_gpusPerNode;
}

uint16_t HierarchicalAllReduce::GetGlobalRank(uint32_t nodeId, uint16_t localIdx) const
{
    return static_cast<uint16_t>(nodeId * m_gpusPerNode + localIdx);
}

CollectiveType HierarchicalAllReduce::GetCollectiveType() const { return CollectiveType::ALLREDUCE; }
bool HierarchicalAllReduce::IsCompleted() const { return m_state == HierarchicalState::COMPLETED; }

void
HierarchicalAllReduce::Initialize(uint16_t numGpus, uint64_t dataSize,
                                   const std::vector<Ptr<FabricEndpoint>>& endpoints)
{
    NS_LOG_FUNCTION(this << numGpus << dataSize);
    m_numGpus = numGpus;
    m_dataSize = dataSize;
    m_endpoints = endpoints;
    m_segmentSize = dataSize / m_gpusPerNode;
    m_interNodeChunk = m_segmentSize / m_numNodes;

    m_gpuReceivedBytes.resize(numGpus, 0);
    m_gpuPhaseComplete.resize(numGpus, false);
    m_gpuInterNodeSendDone.resize(numGpus, false);
    m_dummyData.resize(dataSize, 0xAB);
}

void
HierarchicalAllReduce::SetCompletionCallback(std::function<void(uint64_t durationNs)> cb)
{
    m_completionCallback = cb;
}

void
HierarchicalAllReduce::SetStartupDelay(Time delay)
{
    m_startupDelayNs = delay.GetNanoSeconds();
}

void
HierarchicalAllReduce::DoDispose()
{
    m_endpoints.clear();
    m_completionCallback = nullptr;
    Object::DoDispose();
}

void
HierarchicalAllReduce::SetupReceiveCallbacks()
{
    for (uint16_t i = 0; i < m_numGpus; ++i)
    {
        if (m_endpoints[i])
        {
            m_endpoints[i]->SetReceiveCallback(
                MakeCallback(&HierarchicalAllReduce::OnPacketReceived, this));
        }
    }
}

void
HierarchicalAllReduce::Start()
{
    NS_LOG_FUNCTION(this);
    m_state = HierarchicalState::RUNNING;
    m_startTimeNs = Simulator::Now().GetNanoSeconds();

    SetupReceiveCallbacks();

    // Schedule first phase after startup delay based on initial data size
    uint64_t startupNs = GetLocalStartupDelayNs(m_dataSize);
    uint64_t fullmeshDelay = GetFullmeshStepDelayNs(m_segmentSize);
    Simulator::Schedule(NanoSeconds(startupNs + fullmeshDelay),
                        &HierarchicalAllReduce::StartLocalReduceScatter, this);
}

// Phase 1: Local reduce-scatter (NVLink/MetaXLink within each node)
// Ring mode: each GPU sends segmentSize/gpusPerNode to next local GPU in a ring pattern
// Fullmesh mode: each GPU sends segmentSize to all local peers simultaneously
void
HierarchicalAllReduce::StartLocalReduceScatter()
{
    NS_LOG_FUNCTION(this);
    m_currentPhase = HierarchicalPhase::LOCAL_REDUCESCATTER;

    std::fill(m_gpuReceivedBytes.begin(), m_gpuReceivedBytes.end(), 0);
    std::fill(m_gpuPhaseComplete.begin(), m_gpuPhaseComplete.end(), false);
    m_completedGpuCount = 0;

    if (m_intraNodeAlgorithm == "fullmesh")
    {
        // Fullmesh: each GPU sends segmentSize to all local peers
        for (uint16_t gpu = 0; gpu < m_numGpus; ++gpu)
        {
            uint32_t myNodeId = GetNodeId(gpu);
            uint16_t localIdx = GetLocalIdx(gpu);

            for (uint16_t peerLocal = 0; peerLocal < m_gpusPerNode; ++peerLocal)
            {
                if (peerLocal == localIdx) continue;
                uint16_t peerRank = GetGlobalRank(myNodeId, peerLocal);

                if (m_endpoints[gpu] && m_segmentSize > 0)
                {
                    m_endpoints[gpu]->SendBulkWireTransfer(peerRank, m_dummyData.data(), m_segmentSize,
                                                            m_protocolId, m_flowId, 0);
                }
            }
        }
    }
    else
    {
        // Ring: each GPU sends segmentSize/gpusPerNode to next local peer
        for (uint16_t gpu = 0; gpu < m_numGpus; ++gpu)
        {
            uint32_t myNodeId = GetNodeId(gpu);
            uint16_t localIdx = GetLocalIdx(gpu);
            uint16_t nextLocalIdx = (localIdx + 1) % m_gpusPerNode;
            uint16_t nextRank = GetGlobalRank(myNodeId, nextLocalIdx);

            uint64_t chunkSize = m_segmentSize / m_gpusPerNode;
            if (chunkSize == 0) chunkSize = 1;

            if (m_endpoints[gpu])
            {
                m_endpoints[gpu]->SendBulkWireTransfer(nextRank, m_dummyData.data(), chunkSize,
                                                        m_protocolId, m_flowId, 0);
            }
        }
    }
}

void
HierarchicalAllReduce::OnLocalReduceScatterChunkReceived(uint16_t destRank, uint32_t size)
{
    m_gpuReceivedBytes[destRank] += size;
    CheckLocalReduceScatterComplete(destRank);
}

void
HierarchicalAllReduce::CheckLocalReduceScatterComplete(uint16_t gpu)
{
    uint64_t expectedBytes;
    if (m_intraNodeAlgorithm == "fullmesh")
    {
        // Each GPU receives segmentSize from (gpusPerNode-1) local peers
        expectedBytes = m_segmentSize * (m_gpusPerNode - 1);
    }
    else
    {
        // Ring: each GPU receives one chunk of segmentSize/gpusPerNode from predecessor
        expectedBytes = m_segmentSize / m_gpusPerNode;
    }
    if (expectedBytes == 0) expectedBytes = 1;

    if (m_gpuReceivedBytes[gpu] >= expectedBytes && !m_gpuPhaseComplete[gpu])
    {
        m_gpuPhaseComplete[gpu] = true;
        m_completedGpuCount++;
        NS_LOG_DEBUG("Phase1: GPU " << gpu << " complete, " << m_completedGpuCount << "/" << m_numGpus);

        if (m_completedGpuCount >= m_numGpus)
        {
            NS_LOG_INFO("Phase1 (local reduce-scatter) complete for all GPUs");
            uint64_t computeDelay = GetLocalComputeDelayNs(m_segmentSize);
            uint64_t interStartup = GetInterNodeStartupDelayNs(m_interNodeChunk);
            uint64_t rampDelay = GetInterNodeRampDelayNs(m_interNodeChunk);
            Simulator::Schedule(NanoSeconds(computeDelay + interStartup + rampDelay),
                                &HierarchicalAllReduce::StartInterNodeReduceScatter, this);
        }
    }
}

// Phase 2: Inter-node reduce-scatter (RDMA fullmesh between same-local-rank GPUs)
// GPU i on each node sends its locally-reduced segment chunk to GPU i on other nodes
void
HierarchicalAllReduce::StartInterNodeReduceScatter()
{
    NS_LOG_FUNCTION(this);
    m_currentPhase = HierarchicalPhase::INTER_REDUCESCATTER;

    std::fill(m_gpuReceivedBytes.begin(), m_gpuReceivedBytes.end(), 0);
    std::fill(m_gpuPhaseComplete.begin(), m_gpuPhaseComplete.end(), false);
    m_completedGpuCount = 0;

    // Each GPU sends its interNodeChunk to same-local-rank GPUs on all other nodes
    for (uint16_t gpu = 0; gpu < m_numGpus; ++gpu)
    {
        uint32_t myNodeId = GetNodeId(gpu);
        uint16_t localIdx = GetLocalIdx(gpu);

        for (uint32_t remoteNode = 0; remoteNode < m_numNodes; ++remoteNode)
        {
            if (remoteNode == myNodeId) continue;

            uint16_t remoteRank = GetGlobalRank(remoteNode, localIdx);

            NS_LOG_DEBUG("Phase2: GPU " << gpu << " sending " << m_interNodeChunk << "B to "
                        << remoteRank << " (node" << remoteNode << "/local" << localIdx << ")");

            if (m_endpoints[gpu] && m_interNodeChunk > 0)
            {
                m_endpoints[gpu]->SendBulkWireTransfer(remoteRank, m_dummyData.data(), m_interNodeChunk,
                                                        m_protocolId, m_flowId + 1, 0);
            }
        }
    }
}

void
HierarchicalAllReduce::OnInterNodeReduceScatterChunkReceived(uint16_t destRank, uint32_t size)
{
    m_gpuReceivedBytes[destRank] += size;
    CheckInterNodeReduceScatterComplete(destRank);
}

void
HierarchicalAllReduce::CheckInterNodeReduceScatterComplete(uint16_t gpu)
{
    // Each GPU receives interNodeChunk from (numNodes-1) remote same-local-rank GPUs
    uint64_t expectedBytes = m_interNodeChunk * (m_numNodes - 1);
    if (expectedBytes == 0) expectedBytes = 1;

    if (m_gpuReceivedBytes[gpu] >= expectedBytes && !m_gpuPhaseComplete[gpu])
    {
        m_gpuPhaseComplete[gpu] = true;
        m_completedGpuCount++;
        NS_LOG_DEBUG("Phase2: GPU " << gpu << " complete, " << m_completedGpuCount << "/" << m_numGpus);

        if (m_completedGpuCount >= m_numGpus)
        {
            NS_LOG_INFO("Phase2 (inter-node reduce-scatter) complete for all GPUs");
            uint64_t computeDelay = GetInterNodeComputeDelayNs(m_segmentSize);
            uint64_t interStartup = GetInterNodeStartupDelayNs(m_interNodeChunk);
            uint64_t rampDelay = GetInterNodeRampDelayNs(m_interNodeChunk);
            Simulator::Schedule(NanoSeconds(computeDelay + interStartup + rampDelay),
                                &HierarchicalAllReduce::StartInterNodeAllGather, this);
        }
    }
}

// Phase 3: Inter-node all-gather (RDMA fullmesh)
// Each GPU sends its fully-reduced chunk to same-local-rank GPUs on all other nodes
void
HierarchicalAllReduce::StartInterNodeAllGather()
{
    NS_LOG_FUNCTION(this);
    m_currentPhase = HierarchicalPhase::INTER_ALLGATHER;

    std::fill(m_gpuReceivedBytes.begin(), m_gpuReceivedBytes.end(), 0);
    std::fill(m_gpuPhaseComplete.begin(), m_gpuPhaseComplete.end(), false);
    m_completedGpuCount = 0;

    // Each GPU sends its interNodeChunk to same-local-rank GPUs on all other nodes
    // (Same pattern as Phase 2, but with all-gather semantics)
    for (uint16_t gpu = 0; gpu < m_numGpus; ++gpu)
    {
        uint32_t myNodeId = GetNodeId(gpu);
        uint16_t localIdx = GetLocalIdx(gpu);

        for (uint32_t remoteNode = 0; remoteNode < m_numNodes; ++remoteNode)
        {
            if (remoteNode == myNodeId) continue;

            uint16_t remoteRank = GetGlobalRank(remoteNode, localIdx);

            NS_LOG_DEBUG("Phase3: GPU " << gpu << " sending " << m_interNodeChunk << "B to "
                        << remoteRank);

            if (m_endpoints[gpu] && m_interNodeChunk > 0)
            {
                m_endpoints[gpu]->SendBulkWireTransfer(remoteRank, m_dummyData.data(), m_interNodeChunk,
                                                        m_protocolId, m_flowId + 2, 0);
            }
        }
    }
}

void
HierarchicalAllReduce::OnInterNodeAllGatherChunkReceived(uint16_t destRank, uint32_t size)
{
    m_gpuReceivedBytes[destRank] += size;
    CheckInterNodeAllGatherComplete(destRank);
}

void
HierarchicalAllReduce::CheckInterNodeAllGatherComplete(uint16_t gpu)
{
    uint64_t expectedBytes = m_interNodeChunk * (m_numNodes - 1);
    if (expectedBytes == 0) expectedBytes = 1;

    if (m_gpuReceivedBytes[gpu] >= expectedBytes && !m_gpuPhaseComplete[gpu])
    {
        m_gpuPhaseComplete[gpu] = true;
        m_completedGpuCount++;
        NS_LOG_DEBUG("Phase3: GPU " << gpu << " complete, " << m_completedGpuCount << "/" << m_numGpus);

        if (m_completedGpuCount >= m_numGpus)
        {
            NS_LOG_INFO("Phase3 (inter-node all-gather) complete for all GPUs");
            uint64_t localStartup = GetLocalStartupDelayNs(m_segmentSize);
            uint64_t fullmeshDelay = GetFullmeshStepDelayNs(m_segmentSize);
            Simulator::Schedule(NanoSeconds(localStartup + fullmeshDelay),
                                &HierarchicalAllReduce::StartLocalAllGather, this);
        }
    }
}

// Phase 4: Local all-gather (NVLink ring within each node)
// Each GPU shares its fully-reduced segment with all local peers
void
HierarchicalAllReduce::StartLocalAllGather()
{
    NS_LOG_FUNCTION(this);
    m_currentPhase = HierarchicalPhase::LOCAL_ALLGATHER;

    std::fill(m_gpuReceivedBytes.begin(), m_gpuReceivedBytes.end(), 0);
    std::fill(m_gpuPhaseComplete.begin(), m_gpuPhaseComplete.end(), false);
    m_completedGpuCount = 0;

    // Each GPU sends its segment to all local peers
    for (uint16_t gpu = 0; gpu < m_numGpus; ++gpu)
    {
        uint32_t myNodeId = GetNodeId(gpu);
        uint16_t localIdx = GetLocalIdx(gpu);

        for (uint16_t peerLocal = 0; peerLocal < m_gpusPerNode; ++peerLocal)
        {
            if (peerLocal == localIdx) continue;

            uint16_t peerRank = GetGlobalRank(myNodeId, peerLocal);

            NS_LOG_DEBUG("Phase4: GPU " << gpu << " sending " << m_segmentSize << "B to local peer " << peerRank);

            if (m_endpoints[gpu] && m_segmentSize > 0)
            {
                m_endpoints[gpu]->SendBulkWireTransfer(peerRank, m_dummyData.data(), m_segmentSize,
                                                        m_protocolId, m_flowId + 3, 0);
            }
        }
    }
}

void
HierarchicalAllReduce::OnLocalAllGatherChunkReceived(uint16_t destRank, uint32_t size)
{
    m_gpuReceivedBytes[destRank] += size;
    CheckLocalAllGatherComplete(destRank);
}

void
HierarchicalAllReduce::CheckLocalAllGatherComplete(uint16_t gpu)
{
    // Each GPU receives segmentSize from (gpusPerNode-1) local peers
    uint64_t expectedBytes = m_segmentSize * (m_gpusPerNode - 1);
    if (expectedBytes == 0) expectedBytes = 1;

    if (m_gpuReceivedBytes[gpu] >= expectedBytes && !m_gpuPhaseComplete[gpu])
    {
        m_gpuPhaseComplete[gpu] = true;
        m_completedGpuCount++;
        NS_LOG_DEBUG("Phase4: GPU " << gpu << " complete, " << m_completedGpuCount << "/" << m_numGpus);

        if (m_completedGpuCount >= m_numGpus)
        {
            NS_LOG_INFO("Phase4 (local all-gather) complete — hierarchical AllReduce done!");
            Complete();
        }
    }
}

void
HierarchicalAllReduce::Complete()
{
    m_state = HierarchicalState::COMPLETED;
    m_currentPhase = HierarchicalPhase::COMPLETED;
    m_endTimeNs = Simulator::Now().GetNanoSeconds();
    uint64_t durationNs = m_endTimeNs - m_startTimeNs;

    NS_LOG_INFO("Hierarchical AllReduce completed in " << durationNs / 1000.0 << "µs");

    if (m_completionCallback)
    {
        m_completionCallback(durationNs);
    }
}

void
HierarchicalAllReduce::OnPacketReceived(uint16_t srcRank, Ptr<Packet> packet, FabricHeader header)
{
    NS_LOG_FUNCTION(this << srcRank << packet->GetSize());

    uint16_t destRank = header.GetDestRank();
    uint32_t effectiveDataSize = header.GetEffectiveDataSize();
    uint32_t payloadSize = header.GetPayloadSize();
    // For SIMPLE protocol (efficiency >= 1), payloadSize = dataSize; effectiveDataSize is unset (0)
    // For LL/LL128 (efficiency < 1), effectiveDataSize = dataSize; payloadSize = wireSize
    uint32_t dataBytes = (effectiveDataSize > 0) ? effectiveDataSize : payloadSize;

    NS_LOG_DEBUG("OnPacketReceived: srcRank=" << srcRank << " destRank=" << destRank
                 << " dataBytes=" << dataBytes << " payloadSize=" << payloadSize);

    switch (m_currentPhase)
    {
    case HierarchicalPhase::LOCAL_REDUCESCATTER:
        OnLocalReduceScatterChunkReceived(destRank, dataBytes);
        break;
    case HierarchicalPhase::INTER_REDUCESCATTER:
        OnInterNodeReduceScatterChunkReceived(destRank, dataBytes);
        break;
    case HierarchicalPhase::INTER_ALLGATHER:
        OnInterNodeAllGatherChunkReceived(destRank, dataBytes);
        break;
    case HierarchicalPhase::LOCAL_ALLGATHER:
        OnLocalAllGatherChunkReceived(destRank, dataBytes);
        break;
    default:
        NS_LOG_WARN("Received packet in unexpected phase " << static_cast<int>(m_currentPhase));
        break;
    }
}

} // namespace ns3