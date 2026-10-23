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
      m_chunkRemainder(0),
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

uint64_t
HierarchicalAllGather::ChunkSizeOf(uint16_t rank) const
{
    return m_chunkSize + (rank < m_chunkRemainder ? 1 : 0);
}

void
HierarchicalAllGather::Initialize(uint16_t numGpus, uint64_t dataSize,
                                   const std::vector<Ptr<FabricEndpoint>>& endpoints)
{
    NS_LOG_FUNCTION(this << numGpus << dataSize);
    m_numGpus = numGpus;
    m_dataSize = dataSize;
    m_endpoints = endpoints;
    // Frozen chunk-grid convention (codex round 19, finding 5 — same defect
    // class as HierarchicalAlltoAll round 18, finding 4): rank r owns
    // dataSize/numGpus plus one extra byte when r < dataSize%numGpus, so
    // the grid sums to exactly dataSize. The old uniform m_chunkSize
    // truncated the remainder — 2 nodes x 2 GPUs at dataSize=15 moved 36
    // aggregate bytes instead of 45 — and in the degenerate dataSize <
    // numGpus case made m_chunkSize zero (nothing sent, stall to the
    // 1-hour cap: dataSize=3 never moved a byte).
    m_chunkSize = dataSize / numGpus;
    m_chunkRemainder = dataSize % numGpus;
    if (m_chunkSize == 0 && dataSize > 0)
    {
        // Degenerate dataSize < numGpus: one byte on each of the first
        // dataSize ranks, zero on the rest (grid still sums to dataSize).
        m_chunkRemainder = dataSize;
    }
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
    // Detach the receive callbacks BEFORE dropping the endpoint references:
    // they hold raw `this`, so a packet still in flight after this injector
    // is disposed would invoke OnPacketReceived on freed memory (same class
    // of defect as codex round 16, finding 2). Ownership-checked (codex
    // round 20, finding 6): only the callback THIS injector installed is
    // cleared — a later injector on the same endpoints keeps its binding.
    for (auto& ep : m_endpoints)
    {
        if (ep)
        {
            ep->DetachReceiveCallbackIfOwner(
                MakeCallback(&HierarchicalAllGather::OnPacketReceived, this));
        }
    }
    m_endpoints.clear();
    m_completionCallback = nullptr;
    CollectiveInjector::DoDispose();
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

    // Zero-data collective: every chunk is zero (no peer sends anything,
    // no receive ever fires) — complete immediately instead of stalling to
    // the 1-hour cap (same rule as HierarchicalAlltoAll; codex round 19,
    // finding 5).
    if (m_dataSize == 0)
    {
        Complete();
        return;
    }

    // An explicit SetStartupDelay OVERRIDES the protocol-derived delay
    // (codex round 20, finding 8): the override member was written but
    // never consumed, so --startupDelay=1000000000 left AllGather timing
    // identical to the auto value.
    uint64_t startupNs = (m_startupDelayNs > 0)
                             ? m_startupDelayNs
                             : GetLocalStartupDelayNs(m_chunkSize);
    uint64_t fullmeshDelay = GetFullmeshStepDelayNs(m_chunkSize);
    m_pendingEvents.push_back(
        Simulator::Schedule(NanoSeconds(startupNs + fullmeshDelay),
                            &HierarchicalAllGather::StartLocalAllGather, this));
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

            // DESTINATION-sized chunk (frozen grid convention): the sender
            // ships the peer's chunk, so per-dest load is
            // (gpusPerNode-1)*ChunkSizeOf(dest).
            uint64_t peerChunk = ChunkSizeOf(peerRank);
            if (m_endpoints[gpu] && peerChunk > 0)
            {
                m_endpoints[gpu]->SendBulkWireTransferSize(
                    peerRank, peerChunk, m_protocolId, m_flowId, 0);
            }
        }
    }

    // One GPU per node: the local phase has no peers to gather from, so no
    // receive callback will ever fire to advance the phase. Every GPU is
    // trivially complete — mark them and schedule the inter-node phase
    // directly instead of stalling until the 3600s ceiling.
    if (m_gpusPerNode <= 1)
    {
        NS_LOG_INFO("Phase1 (local all-gather) trivially complete (one GPU per node)");
        uint64_t interStartup = GetInterNodeStartupDelayNs(m_nodeChunkSize);
        uint64_t rampDelay = GetInterNodeRampDelayNs(m_nodeChunkSize);
        m_pendingEvents.push_back(
            Simulator::Schedule(NanoSeconds(interStartup + rampDelay),
                                &HierarchicalAllGather::StartInterNodeAllGather, this));
        return;
    }

    // Degenerate grid: ranks past dataSize hold a zero-sized chunk — no
    // local peer sends them anything, so no receive will advance them.
    // Pre-credit them as phase-complete now (same rule as
    // HierarchicalAlltoAll; codex round 19, finding 5).
    for (uint16_t gpu = 0; gpu < m_numGpus; ++gpu)
    {
        if (ChunkSizeOf(gpu) == 0 && !m_gpuPhaseComplete[gpu])
        {
            m_gpuPhaseComplete[gpu] = true;
            m_completedGpuCount++;
        }
    }
    if (m_completedGpuCount >= m_numGpus)
    {
        NS_LOG_INFO("Phase1 (local all-gather) trivially complete (degenerate grid)");
        uint64_t interStartup = GetInterNodeStartupDelayNs(m_nodeChunkSize);
        uint64_t rampDelay = GetInterNodeRampDelayNs(m_nodeChunkSize);
        m_pendingEvents.push_back(
            Simulator::Schedule(NanoSeconds(interStartup + rampDelay),
                                &HierarchicalAllGather::StartInterNodeAllGather, this));
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
    // Each GPU receives ChunkSizeOf(gpu) from each of its (gpusPerNode-1)
    // local peers (frozen grid — destination-sized chunks; codex round 19,
    // finding 5). A zero-chunk rank receives nothing and was pre-credited.
    uint64_t expectedBytes = ChunkSizeOf(gpu) * (m_gpusPerNode - 1);

    if (m_gpuReceivedBytes[gpu] >= expectedBytes && !m_gpuPhaseComplete[gpu])
    {
        m_gpuPhaseComplete[gpu] = true;
        m_completedGpuCount++;

        if (m_completedGpuCount >= m_numGpus)
        {
            NS_LOG_INFO("Phase1 (local all-gather) complete for all GPUs");
            uint64_t interStartup = GetInterNodeStartupDelayNs(m_nodeChunkSize);
            uint64_t rampDelay = GetInterNodeRampDelayNs(m_nodeChunkSize);
            m_pendingEvents.push_back(
                Simulator::Schedule(NanoSeconds(interStartup + rampDelay),
                                    &HierarchicalAllGather::StartInterNodeAllGather, this));
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

        // Per-node combined chunk on the frozen grid: the sender ships the
        // destination rank's node chunk — sum of ChunkSizeOf over the
        // destination node's local ranks (codex round 19, finding 5).
        uint64_t myNodeChunk = 0;
        for (uint16_t l = 0; l < m_gpusPerNode; ++l)
        {
            myNodeChunk += ChunkSizeOf(GetGlobalRank(myNodeId, l));
        }

        for (uint32_t remoteNode = 0; remoteNode < m_numNodes; ++remoteNode)
        {
            if (remoteNode == myNodeId) continue;
            uint16_t remoteRank = GetGlobalRank(remoteNode, localIdx);

            if (m_endpoints[gpu] && myNodeChunk > 0)
            {
                m_endpoints[gpu]->SendBulkWireTransferSize(
                    remoteRank, myNodeChunk, m_protocolId, m_flowId + 1, 0);
            }
        }
    }

    // Degenerate grid: pre-credit every rank whose REMOTE receive
    // obligation is zero — i.e. every OTHER node holds only zero-chunk
    // ranks, so no remote peer will ever send this rank anything (codex
    // round 20, finding 2). The old pre-credit keyed on the rank's OWN
    // node chunk being zero, which is the SEND side: at 2 nodes x 2 GPUs
    // with dataSize=1 or 2 the grid is [1,0]/[0,0]-style, node 0's ranks
    // have a zero REMOTE obligation (node 1 holds nothing to send) but a
    // nonzero own chunk — the receive threshold clamped 0 -> 1 with no
    // sender, and node 0 stalled to the 1-hour cap. Keying on the remote
    // obligation covers both directions: a rank on an all-zero node also
    // has zero remote obligation.
    for (uint16_t gpu = 0; gpu < m_numGpus; ++gpu)
    {
        if (m_gpuPhaseComplete[gpu])
        {
            continue;
        }
        uint64_t remoteObligation = 0;
        for (uint32_t node = 0; node < m_numNodes; ++node)
        {
            if (node == GetNodeId(gpu)) continue;
            for (uint16_t l = 0; l < m_gpusPerNode; ++l)
            {
                remoteObligation += ChunkSizeOf(GetGlobalRank(node, l));
            }
        }
        if (remoteObligation == 0)
        {
            m_gpuPhaseComplete[gpu] = true;
            m_completedGpuCount++;
        }
    }
    if (m_numNodes > 1 && m_completedGpuCount >= m_numGpus)
    {
        NS_LOG_INFO("Phase2 (inter-node all-gather) trivially complete (degenerate grid)");
        Complete();
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
    // Each GPU receives every REMOTE node's combined chunk (sum of
    // ChunkSizeOf over that node's local ranks) from its same-local-rank
    // peer there — frozen grid (codex round 19, finding 5). Node chunks
    // differ under the grid (node 0 may hold the remainder bytes), so the
    // threshold is the SUM over remote nodes, not ownNodeChunk * (N-1).
    uint64_t expectedBytes = 0;
    for (uint32_t node = 0; node < m_numNodes; ++node)
    {
        if (node == GetNodeId(gpu)) continue;
        for (uint16_t l = 0; l < m_gpusPerNode; ++l)
        {
            expectedBytes += ChunkSizeOf(GetGlobalRank(node, l));
        }
    }
    // A zero obligation (every remote node all-zero-chunk) is pre-credited
    // in StartInterNodeAllGather rather than clamped here — a fake 1-byte
    // threshold with no sender stalls exactly the degenerate grids the
    // pre-credit exists for (codex round 20, finding 2).
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
    // Only DATA counts toward progress (same rule as HierarchicalAllReduce/
    // RingAllReduce): PERMANENT_LOSS notifications carry the lost packet's
    // byte counts but the data is gone, and unrelated P2P traffic must not
    // advance the collective's phase counters.
    if (header.GetPacketType() != FabricPacketType::DATA)
    {
        return;
    }

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
