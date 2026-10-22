/*
 * SPDX-License-Identifier: GPL-2.0-only
 * fullmesh-reducescatter.cc
 *
 * Full-Mesh ReduceScatter: each GPU sends its chunk to all N-1 others simultaneously.
 * Single phase (reduce only, no gather back).
 */

#include "fullmesh-reducescatter.h"
#include "ns3/log.h"
#include "ns3/simulator.h"
#include "nccl-protocol.h"
#include "protocol-model.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("FullMeshReduceScatter");

NS_OBJECT_ENSURE_REGISTERED(FullMeshReduceScatter);

TypeId
FullMeshReduceScatter::GetTypeId()
{
    static TypeId tid = TypeId("ns3::FullMeshReduceScatter")
                            .SetParent<CollectiveInjector>()
                            .SetGroupName("GpuCluster")
                            .AddConstructor<FullMeshReduceScatter>();
    return tid;
}

FullMeshReduceScatter::FullMeshReduceScatter()
    : m_numGpus(0), m_dataSize(0), m_chunkSize(0), m_chunkRemainder(0),
      m_flowId(1),
      m_state(FullMeshRsState::IDLE), m_startupDelayNs(0),
      m_doneCount(0), m_startTimeNs(0), m_endTimeNs(0)
{
}

FullMeshReduceScatter::~FullMeshReduceScatter() {}

void
FullMeshReduceScatter::DoDispose()
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
                MakeCallback(&FullMeshReduceScatter::OnPacketReceived, this));
        }
    }
    m_endpoints.clear();
    m_completionCallback = nullptr;
    CollectiveInjector::DoDispose();
}

void
FullMeshReduceScatter::Initialize(uint16_t numGpus, uint64_t dataSize,
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
    NS_LOG_INFO("FullMeshReduceScatter initialized: numGpus=" << numGpus
                << " dataSize=" << dataSize << " chunkSize=" << m_chunkSize);
}

void
FullMeshReduceScatter::SetupReceiveCallbacks()
{
    for (auto& ep : m_endpoints)
    {
        if (ep)
        {
            ep->SetReceiveCallback(MakeCallback(&FullMeshReduceScatter::OnPacketReceived, this));
        }
    }
}

void
FullMeshReduceScatter::SetCompletionCallback(std::function<void(uint64_t durationNs)> cb)
{
    m_completionCallback = cb;
}

void
FullMeshReduceScatter::SetStartupDelay(Time delay)
{
    m_startupDelayNs = delay.GetNanoSeconds();
}

void
FullMeshReduceScatter::Start()
{
    m_state = FullMeshRsState::RUNNING;
    m_startTimeNs = Simulator::Now().GetNanoSeconds();
    // Tracked in m_pendingEvents so DoDispose cancels it (raw-`this` target).
    m_pendingEvents.push_back(
        Simulator::Schedule(NanoSeconds(m_startupDelayNs),
                            &FullMeshReduceScatter::TriggerSends, this));
}

void
FullMeshReduceScatter::TriggerSends()
{
    // Zero-data collective (codex round 20, finding 10): every chunk is
    // zero, no send ever fires a receive to advance the phase — complete
    // immediately instead of stalling to the 1-hour cap (same convention as
    // FullMeshAllGather, round 19).
    if (m_dataSize == 0)
    {
        NS_LOG_INFO("FullMesh ReduceScatter trivially complete (zero data)");
        Complete();
        return;
    }

    for (uint16_t gpu = 0; gpu < m_numGpus; gpu++)
    {
        if (gpu >= m_endpoints.size() || !m_endpoints[gpu]) continue;

        for (uint16_t dest = 0; dest < m_numGpus; dest++)
        {
            if (dest == gpu) continue;

            // Reduce-scatter: destination dest reduces its OWN chunk, so
            // every source sends ITS copy of chunk dest — per-destination
            // load is (N-1)*ChunkSizeOf(dest). Sizing by the sender's
            // chunk instead skewed per-rank load: the 15-B [4,4,4,3] grid
            // delivered [11,11,11,12] bytes instead of [12,12,12,9] (the
            // same defect codex round 16, finding 6 fixed in
            // FullMeshAllReduce's RS phase; this standalone injector is
            // codex round 17, finding 5).
            const uint64_t chunkBytes = ChunkSizeOf(dest);
            m_sentDataBytes += chunkBytes;
            m_endpoints[gpu]->SendBulkWireTransferSize(
                dest, chunkBytes, m_protocolId, m_flowId, 0);
        }
    }
    NS_LOG_INFO("ReduceScatter: all GPUs sending chunks to all others");
}

void
FullMeshReduceScatter::OnPacketReceived(uint16_t srcRank, Ptr<Packet> packet, FabricHeader header)
{
    if (header.GetPacketType() != FabricPacketType::DATA) return;

    uint16_t destRank = header.GetDestRank();
    uint32_t effectiveSize = header.GetEffectiveDataSize();
    if (effectiveSize == 0) effectiveSize = packet->GetSize();

    m_gpuReceivedBytes[destRank] += effectiveSize;
    CheckCompletion();
}

void
FullMeshReduceScatter::CheckCompletion()
{
    for (uint16_t gpu = 0; gpu < m_numGpus; gpu++)
    {
        // Each destination receives the other N-1 sources' copies of its
        // OWN chunk — (N-1)*ChunkSizeOf(gpu). The whole-grid-minus-own-
        // chunk formula is the AllGather threshold; conflating the two is
        // what allowed the sender-chunk sizing above to complete at all
        // (both sum to the same total across the collective, so the
        // coverage test passed while per-rank load was wrong).
        const uint64_t expectedTotal =
            static_cast<uint64_t>(m_numGpus - 1) * ChunkSizeOf(gpu);
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
FullMeshReduceScatter::ChunkSizeOf(uint16_t gpu) const
{
    // Chunk `gpu` of the grid: base size plus one byte for the first
    // dataSize % numGpus chunks.
    return m_chunkSize + (gpu < m_chunkRemainder ? 1 : 0);
}

CollectiveType
FullMeshReduceScatter::GetCollectiveType() const
{
    return CollectiveType::REDUCESCATTER;
}

bool
FullMeshReduceScatter::IsCompleted() const
{
    return m_state == FullMeshRsState::COMPLETED;
}

void
FullMeshReduceScatter::Complete()
{
    m_state = FullMeshRsState::COMPLETED;
    m_endTimeNs = Simulator::Now().GetNanoSeconds();
    uint64_t durationNs = m_endTimeNs - m_startTimeNs;
    NS_LOG_INFO("FullMesh ReduceScatter COMPLETE in " << durationNs / 1000.0 << " µs");
    if (m_completionCallback) m_completionCallback(durationNs);
}

} // namespace ns3
