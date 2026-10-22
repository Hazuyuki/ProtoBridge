/*
 * SPDX-License-Identifier: GPL-2.0-only
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

uint64_t HierarchicalAllReduce::LocalSegmentOf(uint16_t localIdx) const
{
    // Frozen-grid convention over the LOCAL dimension (codex round 20,
    // finding 3): local index l's phase-1/4 segment is dataSize/gpusPerNode
    // plus one extra byte when l < dataSize % gpusPerNode, repeated on every
    // node. The grid over one node sums to the per-node share, and the
    // 2-level grid (this + InterChunkOf) sums to exactly dataSize.
    return m_dataSize / m_gpusPerNode
           + (localIdx < m_dataSize % m_gpusPerNode ? 1 : 0);
}

uint64_t HierarchicalAllReduce::InterChunkOf(uint32_t nodeId, uint16_t localIdx) const
{
    // Frozen-grid convention over the NODE dimension: within local index
    // l's segment (LocalSegmentOf(l) bytes), node n's inter-node chunk
    // carries segment/N plus one extra byte when n < segment % N. Applied
    // per (node, localIdx) the two grids tile dataSize exactly —
    // sum over n,l of InterChunkOf(n,l) == dataSize.
    const uint64_t segment = LocalSegmentOf(localIdx);
    return segment / m_numNodes + (nodeId < segment % m_numNodes ? 1 : 0);
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
    // Base (floor) sizes for the per-phase DELAY lookups below; the byte
    // accounting itself uses the remainder-aware grids LocalSegmentOf /
    // InterChunkOf (codex round 20, finding 3). The old flat division
    // truncated both levels: 2 nodes x 2 GPUs at dataSize=15 gave
    // segment=7/inter=3 and the collective moved 80 effective bytes
    // instead of 90 — 5 bytes of real data never reached the wire, yet the
    // thresholds shared the same truncation so it still "completed".
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
                MakeCallback(&HierarchicalAllReduce::OnPacketReceived, this));
        }
    }
    m_endpoints.clear();
    m_completionCallback = nullptr;
    CollectiveInjector::DoDispose();
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

    // Zero-data collective: every chunk is zero, no send ever fires a
    // receive to advance the phases — complete immediately instead of
    // stalling to the 1-hour cap (same rule as HierarchicalAllGather;
    // codex round 20, finding 10).
    if (m_dataSize == 0)
    {
        Complete();
        return;
    }

    // Schedule first phase after startup delay based on initial data size.
    // An explicit SetStartupDelay OVERRIDES the protocol-derived delay
    // (codex round 20, finding 8): the override member was written but
    // never consumed, so --startupDelay=1000000000 left AllReduce timing
    // identical to the auto value.
    uint64_t startupNs = (m_startupDelayNs > 0)
                             ? m_startupDelayNs
                             : GetLocalStartupDelayNs(m_dataSize);
    uint64_t fullmeshDelay = GetFullmeshStepDelayNs(m_segmentSize);
    m_pendingEvents.push_back(
        Simulator::Schedule(NanoSeconds(startupNs + fullmeshDelay),
                            &HierarchicalAllReduce::StartLocalReduceScatter, this));
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

                // Destination-sized remainder-aware segment (codex round
                // 20, finding 3): the sender ships the PEER's segment, so
                // per-dest load is (g-1)*LocalSegmentOf(destLocalIdx) and
                // the phase moves every remainder byte.
                const uint64_t peerSegment = LocalSegmentOf(peerLocal);
                if (m_endpoints[gpu] && peerSegment > 0)
                {
                    m_endpoints[gpu]->SendBulkWireTransfer(peerRank, m_dummyData.data(), peerSegment,
                                                            m_protocolId, m_flowId, 0);
                }
            }
        }
    }
    else
    {
        // Ring reduce-scatter (codex round 19, finding 2): a real ring RS
        // runs g-1 steps, each moving one full ring chunk (the successor's
        // remainder-aware segment, codex round 20, finding 3) to the
        // successor, so each GPU ships (g-1)*LocalSegmentOf(next) bytes
        // locally — the same volume the fullmesh branch sends. The old code
        // divided a second time (segmentSize/g per step) AND sent only one
        // step, under-transferring 12x at g=4 (1 MiB moved locally instead
        // of 12 MiB) while still completing on a threshold that matched the
        // under-send. Modeled as one aggregate transfer (per-step pacing is
        // a latency detail the
        // surrogate already folds into per-chunk delays).
        for (uint16_t gpu = 0; gpu < m_numGpus; ++gpu)
        {
            uint32_t myNodeId = GetNodeId(gpu);
            uint16_t localIdx = GetLocalIdx(gpu);
            uint16_t nextLocalIdx = (localIdx + 1) % m_gpusPerNode;
            uint16_t nextRank = GetGlobalRank(myNodeId, nextLocalIdx);

            // Destination-sized (the successor's remainder-aware segment,
            // codex round 20, finding 3): the ring ships the successor's
            // chunk g-1 times, so the aggregate is (g-1)*LocalSegmentOf(next).
            uint64_t ringVolume = LocalSegmentOf(nextLocalIdx) * (m_gpusPerNode - 1);

            if (m_endpoints[gpu])
            {
                m_endpoints[gpu]->SendBulkWireTransfer(nextRank, m_dummyData.data(), ringVolume,
                                                        m_protocolId, m_flowId, 0);
            }
        }
    }

    // One GPU per node: the local phase has no peers (the ring's successor
    // is the GPU itself; fullmesh has nobody to send to), so no receive
    // callback will ever fire to advance the phase. Every GPU is trivially
    // complete — schedule the inter-node phase directly instead of stalling
    // until the 3600s ceiling.
    if (m_gpusPerNode <= 1)
    {
        NS_LOG_INFO("Phase1 (local reduce-scatter) trivially complete (one GPU per node)");
        uint64_t computeDelay = GetLocalComputeDelayNs(m_segmentSize);
        uint64_t interStartup = GetInterNodeStartupDelayNs(m_interNodeChunk);
        uint64_t rampDelay = GetInterNodeRampDelayNs(m_interNodeChunk);
        m_pendingEvents.push_back(
            Simulator::Schedule(NanoSeconds(computeDelay + interStartup + rampDelay),
                                &HierarchicalAllReduce::StartInterNodeReduceScatter, this));
        return;
    }

    // Degenerate grid pre-credit (codex round 21, finding 2): a rank whose
    // local segment is zero receives nothing from its local peers (every
    // peer's destination-sized send to it is zero bytes) — the 0->1 clamp
    // on the threshold left it waiting for a byte nobody sends. Mark such
    // ranks complete here instead of clamping in the check.
    for (uint16_t gpu = 0; gpu < m_numGpus; ++gpu)
    {
        if (!m_gpuPhaseComplete[gpu] && LocalSegmentOf(GetLocalIdx(gpu)) == 0)
        {
            m_gpuPhaseComplete[gpu] = true;
            m_completedGpuCount++;
        }
    }
    if (m_completedGpuCount >= m_numGpus)
    {
        NS_LOG_INFO("Phase1 (local reduce-scatter) trivially complete (degenerate grid)");
        uint64_t computeDelay = GetLocalComputeDelayNs(m_segmentSize);
        uint64_t interStartup = GetInterNodeStartupDelayNs(m_interNodeChunk);
        uint64_t rampDelay = GetInterNodeRampDelayNs(m_interNodeChunk);
        m_pendingEvents.push_back(
            Simulator::Schedule(NanoSeconds(computeDelay + interStartup + rampDelay),
                                &HierarchicalAllReduce::StartInterNodeReduceScatter, this));
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
    // Each GPU receives its OWN remainder-aware segment from each of its
    // (gpusPerNode-1) local peers — fullmesh sends destination-sized
    // segments and the ring's aggregate is the same volume (codex round 20,
    // finding 3). A zero-segment rank (degenerate dataSize < g local grid)
    // receives nothing and is PRE-CREDITED at the phase start (codex round
    // 21, finding 2) — a clamped 1-byte threshold with no sender stalled
    // the degenerate grids.
    uint64_t expectedBytes = LocalSegmentOf(GetLocalIdx(gpu)) * (m_gpusPerNode - 1);

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
            m_pendingEvents.push_back(
                Simulator::Schedule(NanoSeconds(computeDelay + interStartup + rampDelay),
                                    &HierarchicalAllReduce::StartInterNodeReduceScatter, this));
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

            // Destination-sized inter-node chunk (codex round 20, finding 3):
            // the sender ships the REMOTE node's share of this local index's
            // segment — InterChunkOf(remoteNode, localIdx) — so the phase-2
            // traffic tiles dataSize's node dimension exactly, remainder
            // bytes included.
            const uint64_t remoteChunk = InterChunkOf(remoteNode, localIdx);

            NS_LOG_DEBUG("Phase2: GPU " << gpu << " sending " << remoteChunk << "B to "
                        << remoteRank << " (node" << remoteNode << "/local" << localIdx << ")");

            if (m_endpoints[gpu] && remoteChunk > 0)
            {
                m_endpoints[gpu]->SendBulkWireTransfer(remoteRank, m_dummyData.data(), remoteChunk,
                                                        m_protocolId, m_flowId + 1, 0);
            }
        }
    }

    // Degenerate grid pre-credit (codex round 21, finding 2): a rank whose
    // own node share of its local index's segment is zero receives nothing
    // in phase 2 (every remote sender's destination-sized chunk to it is
    // zero bytes). The 0->1 threshold clamp stalled such ranks at D=1/2/3
    // on a 2x2 grid; pre-credit them here instead.
    for (uint16_t gpu = 0; gpu < m_numGpus; ++gpu)
    {
        if (!m_gpuPhaseComplete[gpu] && InterChunkOf(GetNodeId(gpu), GetLocalIdx(gpu)) == 0)
        {
            m_gpuPhaseComplete[gpu] = true;
            m_completedGpuCount++;
        }
    }
    if (m_completedGpuCount >= m_numGpus)
    {
        NS_LOG_INFO("Phase2 (inter-node reduce-scatter) trivially complete (degenerate grid)");
        uint64_t computeDelay = GetInterNodeComputeDelayNs(m_segmentSize);
        uint64_t interStartup = GetInterNodeStartupDelayNs(m_interNodeChunk);
        uint64_t rampDelay = GetInterNodeRampDelayNs(m_interNodeChunk);
        m_pendingEvents.push_back(
            Simulator::Schedule(NanoSeconds(computeDelay + interStartup + rampDelay),
                                &HierarchicalAllReduce::StartInterNodeAllGather, this));
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
    // Phase 2 sends are destination-sized (start of this phase): every
    // remote same-local-rank sender ships THIS rank's node share, so the
    // expected volume is the receiver's own share times (numNodes-1)
    // (codex round 20, finding 3). Summing the REMOTE nodes' shares here —
    // as the phase-3 threshold correctly does for its own-share sends —
    // mismatches under a non-uniform grid: at 2x2/D=15 rank 3 (node 1,
    // share 3B) received its 3B but waited for the 4B the node-0 peer's
    // grid entry promises. A zero obligation is pre-credited in
    // StartInterNodeReduceScatter (codex round 21, finding 2) — a fake
    // 1-byte threshold with no sender stalls exactly the degenerate grids
    // the pre-credit exists for.
    uint64_t expectedBytes = InterChunkOf(GetNodeId(gpu), GetLocalIdx(gpu)) * (m_numNodes - 1);

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
            m_pendingEvents.push_back(
                Simulator::Schedule(NanoSeconds(computeDelay + interStartup + rampDelay),
                                    &HierarchicalAllReduce::StartInterNodeAllGather, this));
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

            // Own-node chunk of this local index's segment (codex round 20,
            // finding 3): phase 3 all-gathers what each node reduced, so the
            // sender ships ITS node's share — InterChunkOf(myNodeId,
            // localIdx) — to every remote same-local-rank peer.
            const uint64_t myChunk = InterChunkOf(myNodeId, localIdx);

            NS_LOG_DEBUG("Phase3: GPU " << gpu << " sending " << myChunk << "B to "
                        << remoteRank);

            if (m_endpoints[gpu] && myChunk > 0)
            {
                m_endpoints[gpu]->SendBulkWireTransfer(remoteRank, m_dummyData.data(), myChunk,
                                                        m_protocolId, m_flowId + 2, 0);
            }
        }
    }

    // Degenerate grid pre-credit (codex round 21, finding 2): a rank whose
    // remote receive obligation (the sum of the REMOTE nodes' shares of
    // its local index's segment) is zero receives nothing in phase 3 —
    // every remote peer's own-share send is zero bytes. Pre-credit instead
    // of clamping the threshold 0 -> 1 with no sender.
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
            remoteObligation += InterChunkOf(node, GetLocalIdx(gpu));
        }
        if (remoteObligation == 0)
        {
            m_gpuPhaseComplete[gpu] = true;
            m_completedGpuCount++;
        }
    }
    if (m_completedGpuCount >= m_numGpus)
    {
        NS_LOG_INFO("Phase3 (inter-node all-gather) trivially complete (degenerate grid)");
        uint64_t localStartup = GetLocalStartupDelayNs(m_segmentSize);
        uint64_t fullmeshDelay = GetFullmeshStepDelayNs(m_segmentSize);
        m_pendingEvents.push_back(
            Simulator::Schedule(NanoSeconds(localStartup + fullmeshDelay),
                                &HierarchicalAllReduce::StartLocalAllGather, this));
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
    // Same remainder-aware threshold as phase 2 (codex round 20, finding 3):
    // the remote nodes' shares of this local index's segment. A zero
    // obligation is pre-credited in StartInterNodeAllGather (codex round
    // 21, finding 2).
    uint64_t expectedBytes = 0;
    for (uint32_t node = 0; node < m_numNodes; ++node)
    {
        if (node == GetNodeId(gpu)) continue;
        expectedBytes += InterChunkOf(node, GetLocalIdx(gpu));
    }

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
            m_pendingEvents.push_back(
                Simulator::Schedule(NanoSeconds(localStartup + fullmeshDelay),
                                    &HierarchicalAllReduce::StartLocalAllGather, this));
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

            // Destination-sized remainder-aware segment (codex round 20,
            // finding 3): phase 4 shares the fully-reduced result, so the
            // sender ships the PEER's segment.
            const uint64_t peerSegment = LocalSegmentOf(peerLocal);

            NS_LOG_DEBUG("Phase4: GPU " << gpu << " sending " << peerSegment << "B to local peer " << peerRank);

            if (m_endpoints[gpu] && peerSegment > 0)
            {
                m_endpoints[gpu]->SendBulkWireTransfer(peerRank, m_dummyData.data(), peerSegment,
                                                        m_protocolId, m_flowId + 3, 0);
            }
        }
    }

    // One GPU per node: Phase4 has no local peers, so no receive callback
    // will ever fire to complete the collective — every GPU is trivially
    // done. (Phase1's gpusPerNode<=1 note applies here too.)
    if (m_gpusPerNode <= 1)
    {
        NS_LOG_INFO("Phase4 (local all-gather) trivially complete (one GPU per node)");
        Complete();
        return;
    }

    // Degenerate grid pre-credit (codex round 21, finding 2): a rank whose
    // own segment is zero receives nothing from its local peers — same rule
    // as phase 1.
    for (uint16_t gpu = 0; gpu < m_numGpus; ++gpu)
    {
        if (!m_gpuPhaseComplete[gpu] && LocalSegmentOf(GetLocalIdx(gpu)) == 0)
        {
            m_gpuPhaseComplete[gpu] = true;
            m_completedGpuCount++;
        }
    }
    if (m_completedGpuCount >= m_numGpus)
    {
        NS_LOG_INFO("Phase4 (local all-gather) trivially complete (degenerate grid)");
        Complete();
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
    // Each GPU receives its OWN remainder-aware segment from each of its
    // (gpusPerNode-1) local peers (codex round 20, finding 3). A
    // zero-segment rank is pre-credited at the phase start (codex round
    // 21, finding 2).
    uint64_t expectedBytes = LocalSegmentOf(GetLocalIdx(gpu)) * (m_gpusPerNode - 1);

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

    // Only DATA counts toward progress. A PERMANENT_LOSS notification (retries
    // exhausted) carries the original packet's byte counts so the fabric layer
    // can propagate terminal progress, but the data is gone — counting it
    // would let the collective report completion with missing data (same rule
    // as RingAllReduce).
    if (header.GetPacketType() != FabricPacketType::DATA)
    {
        if (header.GetPacketType() == FabricPacketType::PERMANENT_LOSS)
        {
            m_permanentLossCount++;
            NS_LOG_INFO("PERMANENT_LOSS: src=" << srcRank << " -> dest="
                         << header.GetDestRank()
                         << " (data integrity violated, not counted as progress)");
        }
        return;
    }

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