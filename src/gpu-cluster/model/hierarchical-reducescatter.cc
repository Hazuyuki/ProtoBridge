/*
 * SPDX-License-Identifier: GPL-2.0-only
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

uint64_t HierarchicalReduceScatter::ChunkOf(uint16_t rank) const
{
    // Frozen-grid convention over the FULL rank space (codex round 20,
    // finding 3 — same rule as FullMeshReduceScatter): rank r's chunk is
    // dataSize/numGpus plus one extra byte when r < dataSize % numGpus, so
    // the grid sums to exactly dataSize. ReduceScatter has no phase-4
    // all-gather, so one flat per-rank grid (not the 2-level AR tiling)
    // sizes both the local ring segments and the inter-node chunks.
    return m_dataSize / m_numGpus + (rank < m_dataSize % m_numGpus ? 1 : 0);
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
    // Base (floor) sizes for the DELAY lookups; byte accounting uses the
    // remainder-aware ChunkOf grid (codex round 20, finding 3). The old flat
    // division truncated the remainder — 2 nodes x 2 GPUs at dataSize=15
    // sent 40 effective bytes instead of 45.
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
                MakeCallback(&HierarchicalReduceScatter::OnPacketReceived, this));
        }
    }
    m_endpoints.clear();
    m_completionCallback = nullptr;
    CollectiveInjector::DoDispose();
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

    // Zero-data collective: every chunk is zero, no send ever fires a
    // receive to advance the phases — complete immediately instead of
    // stalling to the 1-hour cap (same rule as HierarchicalAllGather;
    // codex round 20, findings 9 and 10).
    if (m_dataSize == 0)
    {
        Complete();
        return;
    }

    // An explicit SetStartupDelay OVERRIDES the protocol-derived delay
    // (codex round 20, finding 8 — same fix as HierarchicalAllReduce):
    // the override member was written but never consumed.
    uint64_t startupNs = (m_startupDelayNs > 0)
                             ? m_startupDelayNs
                             : GetLocalStartupDelayNs(m_segmentSize);
    uint64_t fullmeshDelay = GetFullmeshStepDelayNs(m_segmentSize);
    m_pendingEvents.push_back(
        Simulator::Schedule(NanoSeconds(startupNs + fullmeshDelay),
                            &HierarchicalReduceScatter::StartLocalReduceScatter, this));
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

    // One GPU per node: the local RS phase has no peers, so no receive
    // callback will ever fire to advance it — the phase (and the whole
    // collective) stalls to the 3600s cap (codex round 20, finding 9).
    // HierarchicalAllReduce's phase 1 carries the same shortcut;
    // ReduceScatter lacked it. Mark every GPU trivially complete and jump
    // straight to the inter-node phase.
    if (m_gpusPerNode <= 1)
    {
        NS_LOG_INFO("Phase1 (local reduce-scatter) trivially complete (one GPU per node)");
        uint64_t computeDelay = GetLocalComputeDelayNs(m_segmentSize);
        uint64_t interStartup = GetInterNodeStartupDelayNs(m_interNodeChunk);
        uint64_t rampDelay = GetInterNodeRampDelayNs(m_interNodeChunk);
        m_pendingEvents.push_back(
            Simulator::Schedule(NanoSeconds(computeDelay + interStartup + rampDelay),
                                &HierarchicalReduceScatter::StartInterNodeReduceScatter, this));
        return;
    }

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

                // Destination-sized remainder-aware chunk (codex round 20,
                // finding 3): the sender ships the peer's chunk of the grid.
                const uint64_t peerChunk = ChunkOf(peerRank);
                if (m_endpoints[gpu] && peerChunk > 0)
                {
                    m_endpoints[gpu]->SendBulkWireTransferSize(
                        peerRank, peerChunk, m_protocolId, m_flowId, 0);
                }
            }
        }
    }
    else
    {
        // Ring reduce-scatter (codex round 19, finding 2 — same defect as
        // HierarchicalAllReduce): g-1 ring steps of one m_segmentSize chunk
        // each, i.e. m_segmentSize*(gpusPerNode-1) bytes to the successor.
        // The old single undersized transfer moved segmentSize/g once,
        // 12x under the required local RS volume at g=4.
        for (uint16_t gpu = 0; gpu < m_numGpus; ++gpu)
        {
            uint32_t myNodeId = GetNodeId(gpu);
            uint16_t localIdx = GetLocalIdx(gpu);
            uint16_t nextLocalIdx = (localIdx + 1) % m_gpusPerNode;
            uint16_t nextRank = GetGlobalRank(myNodeId, nextLocalIdx);

            // Destination-sized: the successor's remainder-aware chunk,
            // shipped (g-1) times (codex round 20, finding 3).
            uint64_t ringVolume = ChunkOf(nextRank) * (m_gpusPerNode - 1);

            if (m_endpoints[gpu])
            {
                m_endpoints[gpu]->SendBulkWireTransferSize(
                    nextRank, ringVolume, m_protocolId, m_flowId, 0);
            }
        }
    }

    // Degenerate grid pre-credit (codex round 21, finding 2): a rank whose
    // own chunk is zero receives nothing from its local peers — the 0->1
    // threshold clamp left it waiting for a byte nobody sends (2x2 grids
    // at D=1/2/3 stalled to the 3600s cap). Pre-credit such ranks here.
    for (uint16_t gpu = 0; gpu < m_numGpus; ++gpu)
    {
        if (!m_gpuPhaseComplete[gpu] && ChunkOf(gpu) == 0)
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
                                &HierarchicalReduceScatter::StartInterNodeReduceScatter, this));
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
    // Each GPU receives its OWN remainder-aware chunk from each of its
    // (gpusPerNode-1) local peers (codex round 20, finding 3) — fullmesh
    // sends destination-sized chunks and the ring aggregate matches. A
    // zero-chunk rank is pre-credited at the phase start (codex round 21,
    // finding 2).
    uint64_t expectedBytes = ChunkOf(gpu) * (m_gpusPerNode - 1);

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
            m_pendingEvents.push_back(
                Simulator::Schedule(NanoSeconds(computeDelay + interStartup + rampDelay),
                                    &HierarchicalReduceScatter::StartInterNodeReduceScatter, this));
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

            // Destination-sized remainder-aware chunk (codex round 20,
            // finding 3): the sender ships the remote rank's chunk.
            const uint64_t remoteChunk = ChunkOf(remoteRank);
            if (m_endpoints[gpu] && remoteChunk > 0)
            {
                m_endpoints[gpu]->SendBulkWireTransferSize(
                    remoteRank, remoteChunk, m_protocolId, m_flowId + 1, 0);
            }
        }
    }

    // Degenerate grid pre-credit (codex round 21, finding 2): a rank whose
    // own chunk is zero receives nothing in phase 2 either — pre-credit it
    // instead of clamping the threshold 0 -> 1 with no sender.
    for (uint16_t gpu = 0; gpu < m_numGpus; ++gpu)
    {
        if (!m_gpuPhaseComplete[gpu] && ChunkOf(gpu) == 0)
        {
            m_gpuPhaseComplete[gpu] = true;
            m_completedGpuCount++;
        }
    }
    if (m_completedGpuCount >= m_numGpus)
    {
        NS_LOG_INFO("Phase2 (inter-node reduce-scatter) trivially complete (degenerate grid)");
        uint64_t computeDelay = GetInterNodeComputeDelayNs(m_segmentSize);
        m_pendingEvents.push_back(
            Simulator::Schedule(NanoSeconds(computeDelay),
                                &HierarchicalReduceScatter::Complete, this));
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
    // Phase 2 sends are destination-sized (start of this phase): every
    // remote same-local-rank sender ships THIS rank's ChunkOf, so the
    // expected volume is the receiver's own chunk times (numNodes-1)
    // (codex round 20, finding 3). Summing the REMOTE nodes' chunks here
    // mismatches under the remainder — at 2x2/D=15 rank 3 (chunk 3B)
    // received its 3B but waited for rank 2's 4B. A zero chunk is
    // pre-credited at the phase start (codex round 21, finding 2).
    uint64_t expectedBytes = ChunkOf(gpu) * (m_numNodes - 1);

    if (m_gpuReceivedBytes[gpu] >= expectedBytes && !m_gpuPhaseComplete[gpu])
    {
        m_gpuPhaseComplete[gpu] = true;
        m_completedGpuCount++;

        if (m_completedGpuCount >= m_numGpus)
        {
            NS_LOG_INFO("Phase2 (inter-node reduce-scatter) complete — hierarchical ReduceScatter done!");
            uint64_t computeDelay = GetInterNodeComputeDelayNs(m_segmentSize);
            m_pendingEvents.push_back(
                Simulator::Schedule(NanoSeconds(computeDelay),
                                    &HierarchicalReduceScatter::Complete, this));
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
