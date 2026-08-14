/*
 * hierarchical-reducescatter.cc
 *
 * Hierarchical ReduceScatter implementation for multi-node GPU clusters.
 */

#include "hierarchical-reducescatter.h"
#include "ns3/log.h"
#include "ns3/simulator.h"
#include "ns3/nccl-protocol.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("HierarchicalReduceScatter");

NS_OBJECT_ENSURE_REGISTERED(HierarchicalReduceScatter);

TypeId
HierarchicalReduceScatter::GetTypeId()
{
    static TypeId tid = TypeId("ns3::HierarchicalReduceScatter")
        .SetParent<CollectiveInjector>()
        .SetGroupName("GpuCluster");
    return tid;
}

HierarchicalReduceScatter::HierarchicalReduceScatter()
    : m_numGpus(0),
      m_dataSize(0),
      m_flowId(0),
      m_protocolId(static_cast<uint8_t>(NcclProtocol::SIMPLE)),
      m_gpusPerNode(8),
      m_numNodes(1),
      m_intraNodeAlgorithm("fullmesh"),
      m_segmentSize(0),
      m_interNodeChunk(0),
      m_state(HierReduceScatterState::IDLE),
      m_currentPhase(HierReduceScatterPhase::LOCAL_REDUCESCATTER),
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

HierarchicalReduceScatter::~HierarchicalReduceScatter()
{}

void HierarchicalReduceScatter::SetGpusPerNode(uint32_t gpusPerNode) { m_gpusPerNode = gpusPerNode; }
void HierarchicalReduceScatter::SetNumNodes(uint32_t numNodes) { m_numNodes = numNodes; }
void HierarchicalReduceScatter::SetIntraNodeAlgorithm(const std::string& algo) { m_intraNodeAlgorithm = algo; }
void HierarchicalReduceScatter::SetInterNodeStartupDelay(Time delay) { m_interNodeStartupDelayNs = delay.GetNanoSeconds(); }
void HierarchicalReduceScatter::SetInterNodeStartupDelays(Time ll, Time ll128, Time simple)
{
    m_interNodeStartupLLNs = ll.GetNanoSeconds();
    m_interNodeStartupLL128Ns = ll128.GetNanoSeconds();
    m_interNodeStartupSIMPLENs = simple.GetNanoSeconds();
}
uint64_t HierarchicalReduceScatter::GetInterNodeStartupDelayNs(uint64_t chunkSize) const
{
    if (m_interNodeStartupLLNs > 0 || m_interNodeStartupLL128Ns > 0 || m_interNodeStartupSIMPLENs > 0)
    {
        if (chunkSize < m_llThreshold)
            return m_interNodeStartupLLNs;
        else if (chunkSize < m_ll128Threshold)
            return m_interNodeStartupLL128Ns;
        else
            return m_interNodeStartupSIMPLENs;
    }
    if (m_interNodeStartupDelayNs > 0)
        return m_interNodeStartupDelayNs;
    return GetPhaseStartupDelayNs(chunkSize);
}
void HierarchicalReduceScatter::SetLocalStartupDelays(Time ll, Time ll128, Time simple)
{
    m_localStartupLLNs = ll.GetNanoSeconds();
    m_localStartupLL128Ns = ll128.GetNanoSeconds();
    m_localStartupSIMPLENs = simple.GetNanoSeconds();
}
uint64_t HierarchicalReduceScatter::GetLocalStartupDelayNs(uint64_t chunkSize) const
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
void HierarchicalReduceScatter::SetStartupDelays(Time ll, Time ll128, Time simple)
{
    m_startupLLNs = ll.GetNanoSeconds();
    m_startupLL128Ns = ll128.GetNanoSeconds();
    m_startupSIMPLENs = simple.GetNanoSeconds();
}
void HierarchicalReduceScatter::SetStartupPerGpuNs(uint64_t perGpuNs) { m_startupPerGpuNs = perGpuNs; }
void HierarchicalReduceScatter::SetLlThreshold(uint64_t threshold) { m_llThreshold = threshold; }
void HierarchicalReduceScatter::SetLl128Threshold(uint64_t threshold) { m_ll128Threshold = threshold; }
void HierarchicalReduceScatter::SetLocalComputeDelay(Time delay) { m_localComputeDelayNs = delay.GetNanoSeconds(); }
void HierarchicalReduceScatter::SetInterNodeComputeDelay(Time delay) { m_interNodeComputeDelayNs = delay.GetNanoSeconds(); }
void HierarchicalReduceScatter::SetLocalComputePerByteNs(uint64_t perByteNs) { m_localComputePerByteNs = perByteNs; }
void HierarchicalReduceScatter::SetInterNodeComputePerByteNs(uint64_t perByteNs) { m_interNodeComputePerByteNs = perByteNs; }
void HierarchicalReduceScatter::SetLocalComputeBaseDelays(Time ll, Time ll128, Time simple)
{
    m_localComputeBaseLLNs = ll.GetNanoSeconds();
    m_localComputeBaseLL128Ns = ll128.GetNanoSeconds();
    m_localComputeBaseSIMPLENs = simple.GetNanoSeconds();
}
void HierarchicalReduceScatter::SetInterNodeComputeBaseDelays(Time ll, Time ll128, Time simple)
{
    m_interNodeComputeBaseLLNs = ll.GetNanoSeconds();
    m_interNodeComputeBaseLL128Ns = ll128.GetNanoSeconds();
    m_interNodeComputeBaseSIMPLENs = simple.GetNanoSeconds();
}
uint64_t HierarchicalReduceScatter::GetLocalComputeDelayNs(uint64_t chunkSize) const
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
uint64_t HierarchicalReduceScatter::GetInterNodeComputeDelayNs(uint64_t chunkSize) const
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
void HierarchicalReduceScatter::SetInterNodeBwRamp(Time rampDelay, uint64_t rampThresholdBytes)
{
    m_interNodeBwRampDelayNs = rampDelay.GetNanoSeconds();
    m_interNodeBwRampThreshold = rampThresholdBytes;
}
uint64_t HierarchicalReduceScatter::GetInterNodeRampDelayNs(uint64_t chunkSize) const
{
    if (m_interNodeBwRampDelayNs == 0 || m_interNodeBwRampThreshold == 0) return 0;
    return m_interNodeBwRampDelayNs / (1 + chunkSize / m_interNodeBwRampThreshold);
}
void HierarchicalReduceScatter::SetFullmeshPerStepStartupDelays(Time ll, Time ll128, Time simple)
{
    m_fullmeshPerStepLLNs = ll.GetNanoSeconds();
    m_fullmeshPerStepLL128Ns = ll128.GetNanoSeconds();
    m_fullmeshPerStepSIMPLENs = simple.GetNanoSeconds();
}
uint64_t HierarchicalReduceScatter::GetFullmeshStepDelayNs(uint64_t chunkSize) const
{
    if (m_intraNodeAlgorithm != "fullmesh") return 0;
    uint64_t perStepDelay;
    if (chunkSize < m_llThreshold)
        perStepDelay = m_fullmeshPerStepLLNs;
    else if (chunkSize < m_ll128Threshold)
        perStepDelay = m_fullmeshPerStepLL128Ns;
    else
        perStepDelay = m_fullmeshPerStepSIMPLENs;
    if (perStepDelay == 0) return 0;
    return (m_gpusPerNode - 1) * perStepDelay;
}

uint64_t
HierarchicalReduceScatter::GetPhaseStartupDelayNs(uint64_t chunkSize) const
{
    uint64_t baseDelay;
    if (chunkSize < m_llThreshold)
        baseDelay = m_startupLLNs;
    else if (chunkSize < m_ll128Threshold)
        baseDelay = m_startupLL128Ns;
    else
        baseDelay = m_startupSIMPLENs;
    return baseDelay + m_startupPerGpuNs * m_numGpus;
}

uint32_t HierarchicalReduceScatter::GetNodeId(uint16_t rank) const { return rank / m_gpusPerNode; }
uint16_t HierarchicalReduceScatter::GetLocalIdx(uint16_t globalRank) const { return globalRank % m_gpusPerNode; }
uint16_t HierarchicalReduceScatter::GetGlobalRank(uint32_t nodeId, uint16_t localIdx) const
{
    return static_cast<uint16_t>(nodeId * m_gpusPerNode + localIdx);
}

CollectiveType HierarchicalReduceScatter::GetCollectiveType() const { return CollectiveType::REDUCESCATTER; }
bool HierarchicalReduceScatter::IsCompleted() const { return m_state == HierReduceScatterState::COMPLETED; }

void
HierarchicalReduceScatter::Initialize(uint16_t numGpus, uint64_t dataSize,
                                       const std::vector<Ptr<FabricEndpoint>>& endpoints)
{
    NS_LOG_FUNCTION(this << numGpus << dataSize);
    m_numGpus = numGpus;
    m_dataSize = dataSize;
    m_endpoints = endpoints;
    m_segmentSize = dataSize / m_gpusPerNode;
    m_interNodeChunk = dataSize / numGpus;

    m_gpuReceivedBytes.resize(numGpus, 0);
    m_gpuPhaseComplete.resize(numGpus, false);
}

void HierarchicalReduceScatter::SetCompletionCallback(std::function<void(uint64_t durationNs)> cb)
{
    m_completionCallback = cb;
}

void HierarchicalReduceScatter::SetStartupDelay(Time delay)
{
    m_startupDelayNs = delay.GetNanoSeconds();
}

void HierarchicalReduceScatter::DoDispose()
{
    m_endpoints.clear();
    m_completionCallback = nullptr;
    Object::DoDispose();
}

void HierarchicalReduceScatter::SetupReceiveCallbacks()
{
    for (uint16_t i = 0; i < m_numGpus; ++i)
    {
        if (m_endpoints[i])
        {
            m_endpoints[i]->SetReceiveCallback(
                MakeCallback(&HierarchicalReduceScatter::OnPacketReceived, this));
        }
    }
}

void HierarchicalReduceScatter::Start()
{
    NS_LOG_FUNCTION(this);
    m_state = HierReduceScatterState::RUNNING;
    m_startTimeNs = Simulator::Now().GetNanoSeconds();

    SetupReceiveCallbacks();

    uint64_t startupNs = GetLocalStartupDelayNs(m_segmentSize);
    uint64_t fullmeshDelay = GetFullmeshStepDelayNs(m_segmentSize);
    Simulator::Schedule(NanoSeconds(startupNs + fullmeshDelay),
                        &HierarchicalReduceScatter::StartLocalReduceScatter, this);
}

// Phase 1: Local reduce-scatter (NVLink/MetaXLink within each node)
void
HierarchicalReduceScatter::StartLocalReduceScatter()
{
    NS_LOG_FUNCTION(this);
    m_currentPhase = HierReduceScatterPhase::LOCAL_REDUCESCATTER;

    std::fill(m_gpuReceivedBytes.begin(), m_gpuReceivedBytes.end(), 0);
    std::fill(m_gpuPhaseComplete.begin(), m_gpuPhaseComplete.end(), false);
    m_completedGpuCount = 0;

    if (m_intraNodeAlgorithm == "fullmesh")
    {
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
                    m_endpoints[gpu]->SendBulkWireTransferSize(
                        peerRank, m_segmentSize, m_protocolId, m_flowId, 0);
                }
            }
        }
    }
    else
    {
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
                m_endpoints[gpu]->SendBulkWireTransferSize(
                    nextRank, chunkSize, m_protocolId, m_flowId, 0);
            }
        }
    }
}

void
HierarchicalReduceScatter::OnLocalReduceScatterChunkReceived(uint16_t destRank, uint32_t size)
{
    m_gpuReceivedBytes[destRank] += size;
    CheckLocalReduceScatterComplete(destRank);
}

void
HierarchicalReduceScatter::CheckLocalReduceScatterComplete(uint16_t gpu)
{
    uint64_t expectedBytes;
    if (m_intraNodeAlgorithm == "fullmesh")
    {
        expectedBytes = m_segmentSize * (m_gpusPerNode - 1);
    }
    else
    {
        expectedBytes = m_segmentSize / m_gpusPerNode;
    }
    if (expectedBytes == 0) expectedBytes = 1;

    if (m_gpuReceivedBytes[gpu] >= expectedBytes && !m_gpuPhaseComplete[gpu])
    {
        m_gpuPhaseComplete[gpu] = true;
        m_completedGpuCount++;

        if (m_completedGpuCount >= m_numGpus)
        {
            NS_LOG_INFO("Phase1 (local reduce-scatter) complete for all GPUs");
            uint64_t computeDelay = GetLocalComputeDelayNs(m_segmentSize);
            uint64_t interStartup = GetInterNodeStartupDelayNs(m_interNodeChunk);
            uint64_t rampDelay = GetInterNodeRampDelayNs(m_interNodeChunk);
            Simulator::Schedule(NanoSeconds(computeDelay + interStartup + rampDelay),
                                &HierarchicalReduceScatter::StartInterNodeReduceScatter, this);
        }
    }
}

// Phase 2: Inter-node reduce-scatter (RDMA across nodes)
void
HierarchicalReduceScatter::StartInterNodeReduceScatter()
{
    NS_LOG_FUNCTION(this);
    m_currentPhase = HierReduceScatterPhase::INTER_REDUCESCATTER;

    std::fill(m_gpuReceivedBytes.begin(), m_gpuReceivedBytes.end(), 0);
    std::fill(m_gpuPhaseComplete.begin(), m_gpuPhaseComplete.end(), false);
    m_completedGpuCount = 0;

    // Each GPU sends its interNodeChunk to same-local-rank GPUs on other nodes
    for (uint16_t gpu = 0; gpu < m_numGpus; ++gpu)
    {
        uint32_t myNodeId = GetNodeId(gpu);
        uint16_t localIdx = GetLocalIdx(gpu);

        for (uint32_t remoteNode = 0; remoteNode < m_numNodes; ++remoteNode)
        {
            if (remoteNode == myNodeId) continue;
            uint16_t remoteRank = GetGlobalRank(remoteNode, localIdx);

            if (m_endpoints[gpu] && m_interNodeChunk > 0)
            {
                m_endpoints[gpu]->SendBulkWireTransferSize(
                    remoteRank, m_interNodeChunk, m_protocolId, m_flowId + 1, 0);
            }
        }
    }
}

void
HierarchicalReduceScatter::OnInterNodeReduceScatterChunkReceived(uint16_t destRank, uint32_t size)
{
    m_gpuReceivedBytes[destRank] += size;
    CheckInterNodeReduceScatterComplete(destRank);
}

void
HierarchicalReduceScatter::CheckInterNodeReduceScatterComplete(uint16_t gpu)
{
    uint64_t expectedBytes = m_interNodeChunk * (m_numNodes - 1);
    if (expectedBytes == 0) expectedBytes = 1;

    if (m_gpuReceivedBytes[gpu] >= expectedBytes && !m_gpuPhaseComplete[gpu])
    {
        m_gpuPhaseComplete[gpu] = true;
        m_completedGpuCount++;

        if (m_completedGpuCount >= m_numGpus)
        {
            NS_LOG_INFO("Phase2 (inter-node reduce-scatter) complete — hierarchical ReduceScatter done!");
            uint64_t computeDelay = GetInterNodeComputeDelayNs(m_segmentSize);
            Simulator::Schedule(NanoSeconds(computeDelay),
                                &HierarchicalReduceScatter::Complete, this);
        }
    }
}

void
HierarchicalReduceScatter::Complete()
{
    m_state = HierReduceScatterState::COMPLETED;
    m_currentPhase = HierReduceScatterPhase::COMPLETED;
    m_endTimeNs = Simulator::Now().GetNanoSeconds();
    uint64_t durationNs = m_endTimeNs - m_startTimeNs;

    NS_LOG_INFO("Hierarchical ReduceScatter completed in " << durationNs / 1000.0 << "µs");

    if (m_completionCallback)
    {
        m_completionCallback(durationNs);
    }
}

void
HierarchicalReduceScatter::OnPacketReceived(uint16_t srcRank, Ptr<Packet> packet, FabricHeader header)
{
    uint16_t destRank = header.GetDestRank();
    uint32_t effectiveDataSize = header.GetEffectiveDataSize();
    uint32_t payloadSize = header.GetPayloadSize();
    uint32_t dataBytes = (effectiveDataSize > 0) ? effectiveDataSize : payloadSize;

    switch (m_currentPhase)
    {
    case HierReduceScatterPhase::LOCAL_REDUCESCATTER:
        OnLocalReduceScatterChunkReceived(destRank, dataBytes);
        break;
    case HierReduceScatterPhase::INTER_REDUCESCATTER:
        OnInterNodeReduceScatterChunkReceived(destRank, dataBytes);
        break;
    default:
        break;
    }
}

} // namespace ns3
