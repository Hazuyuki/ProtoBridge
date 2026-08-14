/*
 * hierarchical-alltoall.cc
 *
 * Hierarchical AlltoAll implementation for multi-node GPU clusters.
 */

#include "hierarchical-alltoall.h"
#include "ns3/log.h"
#include "ns3/simulator.h"
#include "ns3/nccl-protocol.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("HierarchicalAlltoAll");

NS_OBJECT_ENSURE_REGISTERED(HierarchicalAlltoAll);

TypeId
HierarchicalAlltoAll::GetTypeId()
{
    static TypeId tid = TypeId("ns3::HierarchicalAlltoAll")
        .SetParent<CollectiveInjector>()
        .SetGroupName("GpuCluster");
    return tid;
}

HierarchicalAlltoAll::HierarchicalAlltoAll()
    : m_numGpus(0),
      m_dataSize(0),
      m_flowId(0),
      m_protocolId(static_cast<uint8_t>(NcclProtocol::SIMPLE)),
      m_gpusPerNode(8),
      m_numNodes(1),
      m_intraNodeAlgorithm("fullmesh"),
      m_chunkSize(0),
      m_state(HierAlltoAllState::IDLE),
      m_currentPhase(HierAlltoAllPhase::LOCAL_ALLTOALL),
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
      m_fullmeshPerStepLLNs(0),
      m_fullmeshPerStepLL128Ns(0),
      m_fullmeshPerStepSIMPLENs(0),
      m_completedGpuCount(0),
      m_startTimeNs(0),
      m_endTimeNs(0)
{}

HierarchicalAlltoAll::~HierarchicalAlltoAll()
{}

void HierarchicalAlltoAll::SetGpusPerNode(uint32_t gpusPerNode) { m_gpusPerNode = gpusPerNode; }
void HierarchicalAlltoAll::SetNumNodes(uint32_t numNodes) { m_numNodes = numNodes; }
void HierarchicalAlltoAll::SetIntraNodeAlgorithm(const std::string& algo) { m_intraNodeAlgorithm = algo; }
void HierarchicalAlltoAll::SetInterNodeStartupDelay(Time delay) { m_interNodeStartupDelayNs = delay.GetNanoSeconds(); }
void HierarchicalAlltoAll::SetStartupDelays(Time ll, Time ll128, Time simple)
{
    m_startupLLNs = ll.GetNanoSeconds();
    m_startupLL128Ns = ll128.GetNanoSeconds();
    m_startupSIMPLENs = simple.GetNanoSeconds();
}
void HierarchicalAlltoAll::SetStartupPerGpuNs(uint64_t perGpuNs) { m_startupPerGpuNs = perGpuNs; }
void HierarchicalAlltoAll::SetLlThreshold(uint64_t threshold) { m_llThreshold = threshold; }
void HierarchicalAlltoAll::SetLl128Threshold(uint64_t threshold) { m_ll128Threshold = threshold; }

void HierarchicalAlltoAll::SetInterNodeStartupDelays(Time ll, Time ll128, Time simple)
{
    m_interNodeStartupLLNs = ll.GetNanoSeconds();
    m_interNodeStartupLL128Ns = ll128.GetNanoSeconds();
    m_interNodeStartupSIMPLENs = simple.GetNanoSeconds();
}
uint64_t HierarchicalAlltoAll::GetInterNodeStartupDelayNs(uint64_t chunkSize) const
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

void HierarchicalAlltoAll::SetLocalStartupDelays(Time ll, Time ll128, Time simple)
{
    m_localStartupLLNs = ll.GetNanoSeconds();
    m_localStartupLL128Ns = ll128.GetNanoSeconds();
    m_localStartupSIMPLENs = simple.GetNanoSeconds();
}
uint64_t HierarchicalAlltoAll::GetLocalStartupDelayNs(uint64_t chunkSize) const
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

void HierarchicalAlltoAll::SetFullmeshPerStepStartupDelays(Time ll, Time ll128, Time simple)
{
    m_fullmeshPerStepLLNs = ll.GetNanoSeconds();
    m_fullmeshPerStepLL128Ns = ll128.GetNanoSeconds();
    m_fullmeshPerStepSIMPLENs = simple.GetNanoSeconds();
}
uint64_t HierarchicalAlltoAll::GetFullmeshStepDelayNs(uint64_t chunkSize) const
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
HierarchicalAlltoAll::GetPhaseStartupDelayNs(uint64_t chunkSize) const
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

uint32_t HierarchicalAlltoAll::GetNodeId(uint16_t rank) const { return rank / m_gpusPerNode; }
uint16_t HierarchicalAlltoAll::GetLocalIdx(uint16_t globalRank) const { return globalRank % m_gpusPerNode; }
uint16_t HierarchicalAlltoAll::GetGlobalRank(uint32_t nodeId, uint16_t localIdx) const
{
    return static_cast<uint16_t>(nodeId * m_gpusPerNode + localIdx);
}

CollectiveType HierarchicalAlltoAll::GetCollectiveType() const { return CollectiveType::ALLTOALL; }
bool HierarchicalAlltoAll::IsCompleted() const { return m_state == HierAlltoAllState::COMPLETED; }

void
HierarchicalAlltoAll::Initialize(uint16_t numGpus, uint64_t dataSize,
                                   const std::vector<Ptr<FabricEndpoint>>& endpoints)
{
    NS_LOG_FUNCTION(this << numGpus << dataSize);
    m_numGpus = numGpus;
    m_dataSize = dataSize;
    m_endpoints = endpoints;
    m_chunkSize = dataSize / numGpus;

    m_gpuReceivedBytes.resize(numGpus, 0);
    m_gpuPhaseComplete.resize(numGpus, false);
    m_dummyData.resize(dataSize, 0xAB);
}

void HierarchicalAlltoAll::SetCompletionCallback(std::function<void(uint64_t durationNs)> cb)
{
    m_completionCallback = cb;
}

void HierarchicalAlltoAll::SetStartupDelay(Time delay)
{
    m_startupDelayNs = delay.GetNanoSeconds();
}

void HierarchicalAlltoAll::DoDispose()
{
    m_endpoints.clear();
    m_completionCallback = nullptr;
    Object::DoDispose();
}

void HierarchicalAlltoAll::SetupReceiveCallbacks()
{
    for (uint16_t i = 0; i < m_numGpus; ++i)
    {
        if (m_endpoints[i])
        {
            m_endpoints[i]->SetReceiveCallback(
                MakeCallback(&HierarchicalAlltoAll::OnPacketReceived, this));
        }
    }
}

void HierarchicalAlltoAll::Start()
{
    NS_LOG_FUNCTION(this);
    m_state = HierAlltoAllState::RUNNING;
    m_startTimeNs = Simulator::Now().GetNanoSeconds();

    SetupReceiveCallbacks();

    uint64_t startupNs = GetLocalStartupDelayNs(m_chunkSize);
    uint64_t fullmeshDelay = GetFullmeshStepDelayNs(m_chunkSize);
    Simulator::Schedule(NanoSeconds(startupNs + fullmeshDelay),
                        &HierarchicalAlltoAll::StartLocalAlltoAll, this);
}

// Phase 1: Local alltoall (NVLink/MetaXLink within each node)
// Each GPU sends chunkSize to all local peers
void
HierarchicalAlltoAll::StartLocalAlltoAll()
{
    NS_LOG_FUNCTION(this);
    m_currentPhase = HierAlltoAllPhase::LOCAL_ALLTOALL;

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
                m_endpoints[gpu]->SendBulkWireTransfer(peerRank, m_dummyData.data(), m_chunkSize,
                                                        m_protocolId, m_flowId, 0);
            }
        }
    }
}

void
HierarchicalAlltoAll::OnLocalAlltoAllChunkReceived(uint16_t destRank, uint32_t size)
{
    m_gpuReceivedBytes[destRank] += size;
    CheckLocalAlltoAllComplete(destRank);
}

void
HierarchicalAlltoAll::CheckLocalAlltoAllComplete(uint16_t gpu)
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
            NS_LOG_INFO("Phase1 (local alltoall) complete for all GPUs");
            uint64_t interStartup = GetInterNodeStartupDelayNs(m_chunkSize);
            Simulator::Schedule(NanoSeconds(interStartup),
                                &HierarchicalAlltoAll::StartInterNodeAlltoAll, this);
        }
    }
}

// Phase 2: Inter-node alltoall (RDMA across nodes)
// Each GPU sends chunkSize to ALL GPUs on other nodes (not just same-local-rank)
void
HierarchicalAlltoAll::StartInterNodeAlltoAll()
{
    NS_LOG_FUNCTION(this);
    m_currentPhase = HierAlltoAllPhase::INTER_ALLTOALL;

    std::fill(m_gpuReceivedBytes.begin(), m_gpuReceivedBytes.end(), 0);
    std::fill(m_gpuPhaseComplete.begin(), m_gpuPhaseComplete.end(), false);
    m_completedGpuCount = 0;

    for (uint16_t gpu = 0; gpu < m_numGpus; ++gpu)
    {
        uint32_t myNodeId = GetNodeId(gpu);

        for (uint16_t remoteRank = 0; remoteRank < m_numGpus; ++remoteRank)
        {
            if (GetNodeId(remoteRank) == myNodeId) continue;

            if (m_endpoints[gpu] && m_chunkSize > 0)
            {
                m_endpoints[gpu]->SendBulkWireTransfer(remoteRank, m_dummyData.data(), m_chunkSize,
                                                        m_protocolId, m_flowId + 1, 0);
            }
        }
    }
}

void
HierarchicalAlltoAll::OnInterNodeAlltoAllChunkReceived(uint16_t destRank, uint32_t size)
{
    m_gpuReceivedBytes[destRank] += size;
    CheckInterNodeAlltoAllComplete(destRank);
}

void
HierarchicalAlltoAll::CheckInterNodeAlltoAllComplete(uint16_t gpu)
{
    // Each GPU receives chunkSize from all gpusPerNode GPUs on each remote node
    uint64_t expectedBytes = m_chunkSize * m_gpusPerNode * (m_numNodes - 1);
    if (expectedBytes == 0) expectedBytes = 1;

    if (m_gpuReceivedBytes[gpu] >= expectedBytes && !m_gpuPhaseComplete[gpu])
    {
        m_gpuPhaseComplete[gpu] = true;
        m_completedGpuCount++;

        if (m_completedGpuCount >= m_numGpus)
        {
            NS_LOG_INFO("Phase2 (inter-node alltoall) complete — hierarchical AlltoAll done!");
            Complete();
        }
    }
}

void
HierarchicalAlltoAll::Complete()
{
    m_state = HierAlltoAllState::COMPLETED;
    m_currentPhase = HierAlltoAllPhase::COMPLETED;
    m_endTimeNs = Simulator::Now().GetNanoSeconds();
    uint64_t durationNs = m_endTimeNs - m_startTimeNs;

    NS_LOG_INFO("Hierarchical AlltoAll completed in " << durationNs / 1000.0 << "µs");

    if (m_completionCallback)
    {
        m_completionCallback(durationNs);
    }
}

void
HierarchicalAlltoAll::OnPacketReceived(uint16_t srcRank, Ptr<Packet> packet, FabricHeader header)
{
    uint16_t destRank = header.GetDestRank();
    uint32_t effectiveDataSize = header.GetEffectiveDataSize();
    uint32_t payloadSize = header.GetPayloadSize();
    uint32_t dataBytes = (effectiveDataSize > 0) ? effectiveDataSize : payloadSize;

    switch (m_currentPhase)
    {
    case HierAlltoAllPhase::LOCAL_ALLTOALL:
        OnLocalAlltoAllChunkReceived(destRank, dataBytes);
        break;
    case HierAlltoAllPhase::INTER_ALLTOALL:
        OnInterNodeAlltoAllChunkReceived(destRank, dataBytes);
        break;
    default:
        break;
    }
}

} // namespace ns3