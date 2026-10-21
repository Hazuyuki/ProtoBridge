/*
 * SPDX-License-Identifier: GPL-2.0-only
 * ring-broadcast.cc
 *
 * Chunk-pipelined Ring Broadcast carrying the FULL data size D over every
 * ring edge (see ring-broadcast.h for the volume model).
 *
 * Mechanics:
 * - The root streams D toward its ring successor in m_pipeChunkSize quanta;
 *   each non-root GPU forwards received bytes to its own successor, except
 *   the GPU whose successor is the root (end of the chain).
 * - Forwarding is quantized to m_pipeChunkSize = clamp(D/(32*(N-1)), 64 KiB,
 *   8 MiB): pipeline-fill error <= ~(N-2)*chunk/D, bounded event count.
 *   Sub-chunk tails are held until the flow end.
 * - Progress is byte-counted per GPU; packet order never matters.
 * - Completion: every non-root GPU has received D.
 */

#include "ring-broadcast.h"
#include "ns3/log.h"
#include "ns3/simulator.h"
#include "nccl-protocol.h"
#include "protocol-model.h"

#include <algorithm>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("RingBroadcast");

NS_OBJECT_ENSURE_REGISTERED(RingBroadcast);

namespace
{
constexpr uint16_t NO_RANK = 0xFFFF;
constexpr uint64_t PIPE_CHUNK_MIN = 64ULL * 1024;
constexpr uint64_t PIPE_CHUNK_MAX = 8ULL * 1024 * 1024;
} // namespace

TypeId
RingBroadcast::GetTypeId()
{
    static TypeId tid = TypeId("ns3::RingBroadcast")
                            .SetParent<CollectiveInjector>()
                            .SetGroupName("GpuCluster")
                            .AddConstructor<RingBroadcast>();
    return tid;
}

RingBroadcast::RingBroadcast()
    : m_numGpus(0),
      m_dataSize(0),
      m_chunkSize(0),
      m_pipeChunkSize(0),
      m_flowId(1),
      m_rootRank(0),
      m_protocolId(0),
      m_state(BroadcastState::IDLE),
      m_startupDelayNs(0),
      m_gpuDoneCount(0),
      m_startTimeNs(0),
      m_endTimeNs(0)
{
}

RingBroadcast::~RingBroadcast()
{
}

void
RingBroadcast::DoDispose()
{
    // Detach the receive callbacks BEFORE dropping the endpoint references:
    // they hold raw `this`, so a packet still in flight after this injector
    // is disposed would invoke OnPacketReceived on freed memory (same class
    // of defect as codex round 16, finding 2). Requires a null Callback of
    // the FabricEndpoint::ReceiveCallback signature.
    for (auto& ep : m_endpoints)
    {
        if (ep)
        {
            ep->SetReceiveCallback(MakeNullCallback<void, uint16_t, Ptr<Packet>, FabricHeader>());
        }
    }
    m_endpoints.clear();
    m_completionCallback = nullptr;
    CollectiveInjector::DoDispose();
}

void
RingBroadcast::Initialize(uint16_t numGpus, uint64_t dataSize,
                          const std::vector<Ptr<FabricEndpoint>>& endpoints)
{
    m_numGpus = numGpus;
    m_dataSize = dataSize;
    m_endpoints = endpoints;

    // D/N, kept only for protocol-threshold selection (same per-step payload
    // granularity the ring collectives use to pick LL/LL128/SIMPLE).
    m_chunkSize = dataSize / numGpus;
    if (m_chunkSize == 0)
    {
        m_chunkSize = dataSize;
    }

    uint64_t hops = (numGpus > 1) ? static_cast<uint64_t>(numGpus - 1) : 1;
    m_pipeChunkSize = std::clamp<uint64_t>(dataSize / (32ULL * hops),
                                           PIPE_CHUNK_MIN, PIPE_CHUNK_MAX);

    m_recvBytes.assign(numGpus, 0);
    m_fwdBytes.assign(numGpus, 0);
    m_gpuDoneCount = 0;

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

    NS_LOG_INFO("RingBroadcast initialized: numGpus=" << numGpus
                << " dataSize=" << dataSize << " pipeChunk=" << m_pipeChunkSize
                << " rootRank=" << m_rootRank
                << " protocol=" << static_cast<int>(wireProto)
                << " startupDelay=" << m_startupDelayNs << "ns");
}

void
RingBroadcast::SetupReceiveCallbacks()
{
    for (auto& ep : m_endpoints)
    {
        if (ep)
        {
            ep->SetReceiveCallback(
                MakeCallback(&RingBroadcast::OnPacketReceived, this));
        }
    }
}

void
RingBroadcast::SetCompletionCallback(std::function<void(uint64_t durationNs)> cb)
{
    m_completionCallback = cb;
}

void
RingBroadcast::SetStartupDelay(Time delay)
{
    m_startupDelayNs = delay.GetNanoSeconds();
}

void
RingBroadcast::SetRootRank(uint16_t rootRank)
{
    m_rootRank = rootRank;
}

void
RingBroadcast::Start()
{
    m_state = BroadcastState::RUNNING;
    m_startTimeNs = Simulator::Now().GetNanoSeconds();

    NS_LOG_INFO("Starting Ring Broadcast (root=" << m_rootRank << ") with startup delay " << m_startupDelayNs << " ns");

    // Tracked in m_pendingEvents so DoDispose cancels it (raw-`this` target).
    m_pendingEvents.push_back(
        Simulator::Schedule(NanoSeconds(m_startupDelayNs),
                            &RingBroadcast::TriggerInitialSends, this));
}

void
RingBroadcast::TriggerInitialSends()
{
    if (m_numGpus <= 1)
    {
        Complete();
        return;
    }
    // The root has all of D locally: start its forward flow.
    PumpForward(m_rootRank);
}

uint16_t
RingBroadcast::RingNextOf(uint16_t gpu) const
{
    // Clockwise. Prefer topology-embedded next neighbor, fall back to rank
    // arithmetic when no embedding is installed.
    uint16_t next = m_endpoints[gpu]->GetRingNext();
    return (next != NO_RANK) ? next : (gpu + 1) % m_numGpus;
}

void
RingBroadcast::PumpForward(uint16_t gpu)
{
    if (gpu >= m_endpoints.size() || !m_endpoints[gpu])
    {
        return;
    }

    uint16_t target = RingNextOf(gpu);
    if (target == m_rootRank)
    {
        return;  // end of the chain: the root needs nothing back
    }

    uint64_t ready = (gpu == m_rootRank)
                         ? m_dataSize
                         : std::min(m_recvBytes[gpu], m_dataSize);
    while (m_fwdBytes[gpu] < ready)
    {
        uint64_t remaining = ready - m_fwdBytes[gpu];
        if (remaining < m_pipeChunkSize && ready < m_dataSize)
        {
            break;  // hold the sub-chunk tail until more bytes (or flow end)
        }
        uint64_t chunk = std::min(remaining, m_pipeChunkSize);
        NS_LOG_DEBUG("GPU " << gpu << " -> " << target << " " << chunk << " bytes");
        m_endpoints[gpu]->SendBulkWireTransferSize(target, chunk, m_protocolId, m_flowId, 0);
        m_fwdBytes[gpu] += chunk;
    }
}

void
RingBroadcast::OnPacketReceived(uint16_t srcRank, Ptr<Packet> packet, FabricHeader header)
{
    // NOTE: the callback's first argument is the RECEIVING endpoint's rank
    // (FabricEndpoint passes m_rank); the sender would be header.GetSourceRank().
    if (header.GetPacketType() != FabricPacketType::DATA)
    {
        return;
    }

    uint16_t destRank = header.GetDestRank();
    if (destRank >= m_numGpus || destRank == m_rootRank ||
        m_state != BroadcastState::RUNNING)
    {
        return;
    }

    uint64_t effectiveSize = header.GetEffectiveDataSize();
    if (effectiveSize == 0)
    {
        effectiveSize = packet->GetSize();
    }

    bool wasDone = m_recvBytes[destRank] >= m_dataSize;
    m_recvBytes[destRank] += effectiveSize;

    NS_LOG_DEBUG("GPU " << destRank << " received " << effectiveSize
                 << " (total " << m_recvBytes[destRank] << "/" << m_dataSize << ")");

    PumpForward(destRank);

    if (!wasDone && m_recvBytes[destRank] >= m_dataSize)
    {
        m_gpuDoneCount++;
        NS_LOG_INFO("GPU " << destRank << " received full broadcast (done="
                     << m_gpuDoneCount << "/" << (m_numGpus - 1) << ")");
        if (m_gpuDoneCount >= static_cast<uint32_t>(m_numGpus - 1))
        {
            Complete();
        }
    }
}

CollectiveType
RingBroadcast::GetCollectiveType() const
{
    return CollectiveType::BROADCAST;
}

bool
RingBroadcast::IsCompleted() const
{
    return m_state == BroadcastState::COMPLETED;
}

void
RingBroadcast::Complete()
{
    m_state = BroadcastState::COMPLETED;
    m_endTimeNs = Simulator::Now().GetNanoSeconds();

    uint64_t durationNs = m_endTimeNs - m_startTimeNs;

    NS_LOG_INFO("Ring Broadcast COMPLETE in " << durationNs / 1000.0 << " µs");

    if (m_completionCallback)
    {
        m_completionCallback(durationNs);
    }

}

} // namespace ns3
