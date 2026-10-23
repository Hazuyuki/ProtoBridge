/*
 * SPDX-License-Identifier: GPL-2.0-only
 * fullmesh-allgather.cc
 *
 * Full-Mesh AllGather: each GPU sends its chunk to all N-1 others simultaneously.
 */

#include "fullmesh-allgather.h"
#include "ns3/log.h"
#include "ns3/simulator.h"
#include "nccl-protocol.h"
#include "protocol-model.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("FullMeshAllGather");

NS_OBJECT_ENSURE_REGISTERED(FullMeshAllGather);

TypeId
FullMeshAllGather::GetTypeId()
{
    static TypeId tid = TypeId("ns3::FullMeshAllGather")
                            .SetParent<CollectiveInjector>()
                            .SetGroupName("GpuCluster")
                            .AddConstructor<FullMeshAllGather>();
    return tid;
}

FullMeshAllGather::FullMeshAllGather()
    : m_numGpus(0), m_dataSize(0), m_chunkSize(0), m_chunkRemainder(0),
      m_flowId(1),
      m_state(FullMeshAgState::IDLE), m_startupDelayNs(0),
      m_doneCount(0), m_startTimeNs(0), m_endTimeNs(0)
{
}

FullMeshAllGather::~FullMeshAllGather() {}

void
FullMeshAllGather::DoDispose()
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
                MakeCallback(&FullMeshAllGather::OnPacketReceived, this));
        }
    }
    m_endpoints.clear();
    m_completionCallback = nullptr;
    CollectiveInjector::DoDispose();
}

void
FullMeshAllGather::Initialize(uint16_t numGpus, uint64_t dataSize,
                              const std::vector<Ptr<FabricEndpoint>>& endpoints)
{
    m_numGpus = numGpus;
    m_dataSize = dataSize;
    m_endpoints = endpoints;

    // Chunk grid: rank r's base chunk is dataSize/numGpus bytes, with the
    // first dataSize % numGpus chunks carrying one extra byte, so the grid
    // sums to exactly dataSize (codex round 16, finding 5: plain integer
    // division silently dropped dataSize % numGpus bytes — 15 B over 4 GPUs
    // completed after 36 effective bytes instead of the required 45).
    m_chunkSize = dataSize / numGpus;
    m_chunkRemainder = dataSize % numGpus;
    if (m_chunkSize == 0 && dataSize > 0)
    {
        // Degenerate dataSize < numGpus: one byte on each of the first
        // dataSize ranks, zero on the rest (grid still sums to dataSize).
        m_chunkSize = 0;
        m_chunkRemainder = dataSize;
    }

    m_sentDataBytes = 0;

    m_gpuReceivedBytes.resize(numGpus, 0);
    m_gpuSrcDoneCount.resize(numGpus, 0);
    m_doneCount = 0;

    m_flowId = 1;

    // Startup delay from total data size, wire protocol from per-step size
    Ptr<ProtocolModel> protoModel = m_endpoints[0]->GetProtocolModel();
    NcclProtocol startupProto = static_cast<NcclProtocol>(protoModel->GetProtocolId(m_dataSize));
    if (m_startupDelayNs == 0)
    {
        m_startupDelayNs = protoModel->GetStartupDelayNs(static_cast<uint8_t>(startupProto), m_numGpus);
    }
    NcclProtocol wireProto = static_cast<NcclProtocol>(protoModel->GetProtocolId(m_chunkSize));
    m_protocolId = static_cast<uint8_t>(wireProto);


    SetupReceiveCallbacks();
    NS_LOG_INFO("FullMeshAllGather initialized: numGpus=" << numGpus
                << " dataSize=" << dataSize << " chunkSize=" << m_chunkSize);
}

void
FullMeshAllGather::SetupReceiveCallbacks()
{
    for (auto& ep : m_endpoints)
    {
        if (ep)
        {
            ep->SetReceiveCallback(MakeCallback(&FullMeshAllGather::OnPacketReceived, this));
        }
    }
}

void
FullMeshAllGather::SetCompletionCallback(std::function<void(uint64_t durationNs)> cb)
{
    m_completionCallback = cb;
}

void
FullMeshAllGather::SetStartupDelay(Time delay)
{
    m_startupDelayNs = delay.GetNanoSeconds();
}

void
FullMeshAllGather::Start()
{
    m_state = FullMeshAgState::RUNNING;
    m_startTimeNs = Simulator::Now().GetNanoSeconds();
    // Tracked in m_pendingEvents so DoDispose cancels it (raw-`this` target).
    m_pendingEvents.push_back(
        Simulator::Schedule(NanoSeconds(m_startupDelayNs),
                            &FullMeshAllGather::TriggerSends, this));
}

void
FullMeshAllGather::TriggerSends()
{
    // Zero-data collective: every chunk is zero, so no bulk transfer fires a
    // receive and CheckCompletion never runs — the collective would stall to
    // the simulation cap. Complete now instead (codex round 19, finding 8;
    // same convention as NvlsAllGather::TriggerSendPhase).
    if (m_dataSize == 0)
    {
        NS_LOG_INFO("FullMesh AllGather trivially complete (zero data)");
        Complete();
        return;
    }

    // Single-rank collective (codex round 21, finding 8): N=1 has no peers
    // to broadcast to and no receives will ever fire — the rank already
    // holds the entire gathered buffer. Complete now instead of stalling
    // to the cap.
    if (m_numGpus <= 1)
    {
        NS_LOG_INFO("FullMesh AllGather trivially complete (single rank)");
        Complete();
        return;
    }

    for (uint16_t gpu = 0; gpu < m_numGpus; gpu++)
    {
        if (gpu >= m_endpoints.size() || !m_endpoints[gpu]) continue;

        for (uint16_t dest = 0; dest < m_numGpus; dest++)
        {
            if (dest == gpu) continue;

            const uint64_t chunkBytes = ChunkSizeOf(gpu);
            m_sentDataBytes += chunkBytes;
            m_endpoints[gpu]->SendBulkWireTransferSize(
                dest, chunkBytes, m_protocolId, m_flowId, 0);
        }
    }
    NS_LOG_INFO("AllGather: all GPUs sending chunks to all others");
}

void
FullMeshAllGather::OnPacketReceived(uint16_t srcRank, Ptr<Packet> packet, FabricHeader header)
{
    if (header.GetPacketType() != FabricPacketType::DATA) return;

    uint16_t destRank = header.GetDestRank();
    uint32_t effectiveSize = header.GetEffectiveDataSize();
    if (effectiveSize == 0) effectiveSize = packet->GetSize();

    m_gpuReceivedBytes[destRank] += effectiveSize;
    CheckCompletion();
}

void
FullMeshAllGather::CheckCompletion()
{
    for (uint16_t gpu = 0; gpu < m_numGpus; gpu++)
    {
        // Each destination receives the chunks of all N-1 sources: the
        // whole grid minus its OWN chunk (which it already holds).
        const uint64_t expectedTotal = m_dataSize - ChunkSizeOf(gpu);
        if (m_gpuSrcDoneCount[gpu] == 0 && m_gpuReceivedBytes[gpu] >= expectedTotal)
        {
            m_gpuSrcDoneCount[gpu] = m_numGpus - 1;
            m_doneCount++;
        }
    }

    if (m_doneCount >= m_numGpus)
    {
        Complete();
    }
}

uint64_t
FullMeshAllGather::ChunkSizeOf(uint16_t gpu) const
{
    // Chunk `gpu` of the grid: base size plus one byte for the first
    // dataSize % numGpus chunks.
    return m_chunkSize + (gpu < m_chunkRemainder ? 1 : 0);
}

CollectiveType
FullMeshAllGather::GetCollectiveType() const
{
    return CollectiveType::ALLGATHER;
}

bool
FullMeshAllGather::IsCompleted() const
{
    return m_state == FullMeshAgState::COMPLETED;
}

void
FullMeshAllGather::Complete()
{
    m_state = FullMeshAgState::COMPLETED;
    m_endTimeNs = Simulator::Now().GetNanoSeconds();
    uint64_t durationNs = m_endTimeNs - m_startTimeNs;
    NS_LOG_INFO("FullMesh AllGather COMPLETE in " << durationNs / 1000.0 << " µs");
    if (m_completionCallback) m_completionCallback(durationNs);
}

} // namespace ns3
