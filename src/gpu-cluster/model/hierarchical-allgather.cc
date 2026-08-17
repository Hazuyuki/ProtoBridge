/*
 * SPDX-License-Identifier: GPL-2.0-only
 * hierarchical-allgather.cc
 *
 * Hierarchical AllGather implementation for multi-node GPU clusters.
 */

#include "hierarchical-allgather.h"
#include "ns3/log.h"
#include "ns3/simulator.h"
#include "ns3/nccl-protocol.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("HierarchicalAllGather");

NS_OBJECT_ENSURE_REGISTERED(HierarchicalAllGather);

TypeId
HierarchicalAllGather::GetTypeId()
{
    static TypeId tid = TypeId("ns3::HierarchicalAllGather")
        .SetParent<CollectiveInjector>()
        .SetGroupName("GpuCluster");
    return tid;
}

HierarchicalAllGather::HierarchicalAllGather()
    : m_numGpus(0),
      m_dataSize(0),
      m_flowId(0),
      m_protocolId(static_cast<uint8_t>(NcclProtocol::SIMPLE)),
      m_gpusPerNode(8),
      m_numNodes(1),
      m_intraNodeAlgorithm("fullmesh"),
      m_chunkSize(0),
      m_nodeChunkSize(0),
      m_state(HierAllGatherState::IDLE),
      m_currentPhase(HierAllGatherPhase::LOCAL_ALLGATHER),
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
      m_interNodeBwRampDelayNs(0),
      m_interNodeBwRampThreshold(0),
      m_fullmeshPerStepLLNs(0),
      m_fullmeshPerStepLL128Ns(0),
      m_fullmeshPerStepSIMPLENs(0),
      m_completedGpuCount(0),
      m_startTimeNs(0),
      m_endTimeNs(0)
{}

HierarchicalAllGather::~HierarchicalAllGather()
{}

void HierarchicalAllGather::SetGpusPerNode(uint32_t gpusPerNode) { m_gpusPerNode = gpusPerNode; }
void HierarchicalAllGather::SetNumNodes(uint32_t numNodes) { m_numNodes = numNodes; }
void HierarchicalAllGather::SetIntraNodeAlgorithm(const std::string& algo) { m_intraNodeAlgorithm = algo; }
void HierarchicalAllGather::SetInterNodeStartupDelay(Time delay) { m_interNodeStartupDelayNs = delay.GetNanoSeconds(); }
void HierarchicalAllGather::SetInterNodeStartupDelays(Time ll, Time ll128, Time simple)
{
    m_interNodeStartupLLNs = ll.GetNanoSeconds();
    m_interNodeStartupLL128Ns = ll128.GetNanoSeconds();
    m_interNodeStartupSIMPLENs = simple.GetNanoSeconds();
}
uint64_t HierarchicalAllGather::GetInterNodeStartupDelayNs(uint64_t chunkSize) const
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
void HierarchicalAllGather::SetLocalStartupDelays(Time ll, Time ll128, Time simple)
{
    m_localStartupLLNs = ll.GetNanoSeconds();
    m_localStartupLL128Ns = ll128.GetNanoSeconds();
    m_localStartupSIMPLENs = simple.GetNanoSeconds();
}
uint64_t HierarchicalAllGather::GetLocalStartupDelayNs(uint64_t chunkSize) const
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
void HierarchicalAllGather::SetStartupDelays(Time ll, Time ll128, Time simple)
{
    m_startupLLNs = ll.GetNanoSeconds();
    m_startupLL128Ns = ll128.GetNanoSeconds();
    m_startupSIMPLENs = simple.GetNanoSeconds();
}
void HierarchicalAllGather::SetStartupPerGpuNs(uint64_t perGpuNs) { m_startupPerGpuNs = perGpuNs; }
void HierarchicalAllGather::SetLlThreshold(uint64_t threshold) { m_llThreshold = threshold; }
void HierarchicalAllGather::SetLl128Threshold(uint64_t threshold) { m_ll128Threshold = threshold; }
void HierarchicalAllGather::SetInterNodeBwRamp(Time rampDelay, uint64_t rampThresholdBytes)
{
    m_interNodeBwRampDelayNs = rampDelay.GetNanoSeconds();
    m_interNodeBwRampThreshold = rampThresholdBytes;
}
uint64_t HierarchicalAllGather::GetInterNodeRampDelayNs(uint64_t chunkSize) const
{
    if (m_interNodeBwRampDelayNs == 0 || m_interNodeBwRampThreshold == 0) return 0;
    return m_interNodeBwRampDelayNs / (1 + chunkSize / m_interNodeBwRampThreshold);
}
void HierarchicalAllGather::SetFullmeshPerStepStartupDelays(Time ll, Time ll128, Time simple)
{
    m_fullmeshPerStepLLNs = ll.GetNanoSeconds();
    m_fullmeshPerStepLL128Ns = ll128.GetNanoSeconds();
    m_fullmeshPerStepSIMPLENs = simple.GetNanoSeconds();
}
uint64_t HierarchicalAllGather::GetFullmeshStepDelayNs(uint64_t chunkSize) const
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
HierarchicalAllGather::GetPhaseStartupDelayNs(uint64_t chunkSize) const
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

uint32_t HierarchicalAllGather::GetNodeId(uint16_t rank) const { return rank / m_gpusPerNode; }
uint16_t HierarchicalAllGather::GetLocalIdx(uint16_t globalRank) const { return globalRank % m_gpusPerNode; }
uint16_t HierarchicalAllGather::GetGlobalRank(uint32_t nodeId, uint16_t localIdx) const
{
    return static_cast<uint16_t>(nodeId * m_gpusPerNode + localIdx);
}

CollectiveType HierarchicalAllGather::GetCollectiveType() const { return CollectiveType::ALLGATHER; }
bool HierarchicalAllGather::IsCompleted() const { return m_state == HierAllGatherState::COMPLETED; }

void
HierarchicalAllGather::Initialize(uint16_t numGpus, uint64_t dataSize,
                                   const std::vector<Ptr<FabricEndpoint>>& endpoints)
{
    NS_LOG_FUNCTION(this << numGpus << dataSize);
    m_numGpus = numGpus;
    m_dataSize = dataSize;
    m_endpoints = endpoints;
    m_chunkSize = dataSize / numGpus;
    m_nodeChunkSize = m_chunkSize * m_gpusPerNode;

    m_gpuReceivedBytes.resize(numGpus, 0);
    m_gpuPhaseComplete.resize(numGpus, false);
}

void HierarchicalAllGather::SetCompletionCallback(std::function<void(uint64_t durationNs)> cb)
{
    m_completionCallback = cb;
}

void HierarchicalAllGather::SetStartupDelay(Time delay)
{
    m_startupDelayNs = delay.GetNanoSeconds();
}

void HierarchicalAllGather::DoDispose()
{
    m_endpoints.clear();
    m_completionCallback = nullptr;
    Object::DoDispose();
}

void HierarchicalAllGather::SetupReceiveCallbacks()
{
    for (uint16_t i = 0; i < m_numGpus; ++i)
    {
        if (m_endpoints[i])
        {
            m_endpoints[i]->SetReceiveCallback(
                MakeCallback(&HierarchicalAllGather::OnPacketReceived, this));
        }
    }
}

void HierarchicalAllGather::Start()
{
    NS_LOG_FUNCTION(this);
    m_state = HierAllGatherState::RUNNING;
    m_startTimeNs = Simulator::Now().GetNanoSeconds();

    SetupReceiveCallbacks();

    uint64_t startupNs = GetLocalStartupDelayNs(m_chunkSize);
    uint64_t fullmeshDelay = GetFullmeshStepDelayNs(m_chunkSize);
    Simulator::Schedule(NanoSeconds(startupNs + fullmeshDelay),
                        &HierarchicalAllGather::StartLocalAllGather, this);
}

// Phase 1: Local all-gather (NVLink/MetaXLink within each node)
// Each GPU sends its chunkSize to all local peers
void
HierarchicalAllGather::StartLocalAllGather()
{
    NS_LOG_FUNCTION(this);
    m_currentPhase = HierAllGatherPhase::LOCAL_ALLGATHER;

    std::fill(m_gpuReceivedBytes.begin(), m_gpuReceivedBytes.end(), 0);
    std::fill(m_gpuPhaseComplete.begin(), m_gpuPhaseComplete.end(), false);
    m_completedGpuCount = 0;

    for (uint16_t gpu = 0; gpu < m_numGpus; ++gpu)
    {
        uint32_t myNodeId = GetNodeId(gpu);
        uint16_t localIdx = GetLocalIdx(gpu);

        for (uint16_t peerLocal = 0; peerLocal < m_gpusPerNode; ++peerLocal)
        {
            if (peerLocal == localIdx) continue;
            uint16_t peerRank = GetGlobalRank(myNodeId, peerLocal);

            if (m_endpoints[gpu] && m_chunkSize > 0)
            {
                m_endpoints[gpu]->SendBulkWireTransferSize(
                    peerRank, m_chunkSize, m_protocolId, m_flowId, 0);
            }
        }
    }
}

void
HierarchicalAllGather::OnLocalAllGatherChunkReceived(uint16_t destRank, uint32_t size)
{
    m_gpuReceivedBytes[destRank] += size;
    CheckLocalAllGatherComplete(destRank);
}

void
HierarchicalAllGather::CheckLocalAllGatherComplete(uint16_t gpu)
{
    // Each GPU receives chunkSize from (gpusPerNode-1) local peers
    uint64_t expectedBytes = m_chunkSize * (m_gpusPerNode - 1);
    if (expectedBytes == 0) expectedBytes = 1;

    if (m_gpuReceivedBytes[gpu] >= expectedBytes && !m_gpuPhaseComplete[gpu])
    {
        m_gpuPhaseComplete[gpu] = true;
        m_completedGpuCount++;

        if (m_completedGpuCount >= m_numGpus)
        {
            NS_LOG_INFO("Phase1 (local all-gather) complete for all GPUs");
            uint64_t interStartup = GetInterNodeStartupDelayNs(m_nodeChunkSize);
            uint64_t rampDelay = GetInterNodeRampDelayNs(m_nodeChunkSize);
            Simulator::Schedule(NanoSeconds(interStartup + rampDelay),
                                &HierarchicalAllGather::StartInterNodeAllGather, this);
        }
    }
}

// Phase 2: Inter-node all-gather (RDMA across nodes)
// Each GPU sends nodeChunkSize (= chunkSize * gpusPerNode) to same-local-rank GPUs on other nodes
void
HierarchicalAllGather::StartInterNodeAllGather()
{
    NS_LOG_FUNCTION(this);
    m_currentPhase = HierAllGatherPhase::INTER_ALLGATHER;

    std::fill(m_gpuReceivedBytes.begin(), m_gpuReceivedBytes.end(), 0);
    std::fill(m_gpuPhaseComplete.begin(), m_gpuPhaseComplete.end(), false);
    m_completedGpuCount = 0;

    for (uint16_t gpu = 0; gpu < m_numGpus; ++gpu)
    {
        uint32_t myNodeId = GetNodeId(gpu);
        uint16_t localIdx = GetLocalIdx(gpu);

        for (uint32_t remoteNode = 0; remoteNode < m_numNodes; ++remoteNode)
        {
            if (remoteNode == myNodeId) continue;
            uint16_t remoteRank = GetGlobalRank(remoteNode, localIdx);

            if (m_endpoints[gpu] && m_nodeChunkSize > 0)
            {
                m_endpoints[gpu]->SendBulkWireTransferSize(
                    remoteRank, m_nodeChunkSize, m_protocolId, m_flowId + 1, 0);
            }
        }
    }
}

void
HierarchicalAllGather::OnInterNodeAllGatherChunkReceived(uint16_t destRank, uint32_t size)
{
    m_gpuReceivedBytes[destRank] += size;
    CheckInterNodeAllGatherComplete(destRank);
}

void
HierarchicalAllGather::CheckInterNodeAllGatherComplete(uint16_t gpu)
{
    // Each GPU receives nodeChunkSize from (numNodes-1) remote same-local-rank GPUs
    uint64_t expectedBytes = m_nodeChunkSize * (m_numNodes - 1);
    if (expectedBytes == 0) expectedBytes = 1;

    if (m_gpuReceivedBytes[gpu] >= expectedBytes && !m_gpuPhaseComplete[gpu])
    {
        m_gpuPhaseComplete[gpu] = true;
        m_completedGpuCount++;

        if (m_completedGpuCount >= m_numGpus)
        {
            NS_LOG_INFO("Phase2 (inter-node all-gather) complete — hierarchical AllGather done!");
            Complete();
        }
    }
}

void
HierarchicalAllGather::Complete()
{
    m_state = HierAllGatherState::COMPLETED;
    m_currentPhase = HierAllGatherPhase::COMPLETED;
    m_endTimeNs = Simulator::Now().GetNanoSeconds();
    uint64_t durationNs = m_endTimeNs - m_startTimeNs;

    NS_LOG_INFO("Hierarchical AllGather completed in " << durationNs / 1000.0 << "µs");

    if (m_completionCallback)
    {
        m_completionCallback(durationNs);
    }
}

void
HierarchicalAllGather::OnPacketReceived(uint16_t srcRank, Ptr<Packet> packet, FabricHeader header)
{
    uint16_t destRank = header.GetDestRank();
    uint32_t effectiveDataSize = header.GetEffectiveDataSize();
    uint32_t payloadSize = header.GetPayloadSize();
    uint32_t dataBytes = (effectiveDataSize > 0) ? effectiveDataSize : payloadSize;

    switch (m_currentPhase)
    {
    case HierAllGatherPhase::LOCAL_ALLGATHER:
        OnLocalAllGatherChunkReceived(destRank, dataBytes);
        break;
    case HierAllGatherPhase::INTER_ALLGATHER:
        OnInterNodeAllGatherChunkReceived(destRank, dataBytes);
        break;
    default:
        break;
    }
}

} // namespace ns3
