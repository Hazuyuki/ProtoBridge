/*
 * SPDX-License-Identifier: GPL-2.0-only
 * ring-reduce.cc
 *
 * Chunk-pipelined Ring Reduce carrying the FULL data size D over every ring
 * edge of the chain toward the root (see ring-reduce.h for the volume model).
 *
 * Mechanics:
 * - The chain head (the root's ring predecessor) streams its D toward its
 *   own ring predecessor in m_pipeChunkSize quanta; each interior GPU can
 *   forward byte i of the partial sum only after receiving byte i from
 *   upstream (its own contribution is local), so its forward progress is
 *   gated on its received bytes.
 * - Forwarding is quantized to m_pipeChunkSize = clamp(D/(32*(N-1)), 64 KiB,
 *   8 MiB): pipeline-fill error <= ~(N-2)*chunk/D, bounded event count.
 *   Sub-chunk tails are held until the flow end.
 * - Progress is byte-counted per GPU; packet order never matters.
 * - Completion: the root has received D from its ring successor.
 */

#include "ring-reduce.h"
#include "ns3/log.h"
#include "ns3/simulator.h"
#include "nccl-protocol.h"
#include "protocol-model.h"

#include <algorithm>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("RingReduce");

NS_OBJECT_ENSURE_REGISTERED(RingReduce);

namespace
{
constexpr uint16_t NO_RANK = 0xFFFF;
constexpr uint64_t PIPE_CHUNK_MIN = 64ULL * 1024;
constexpr uint64_t PIPE_CHUNK_MAX = 8ULL * 1024 * 1024;
} // namespace

TypeId
RingReduce::GetTypeId()
{
    static TypeId tid = TypeId("ns3::RingReduce")
                            .SetParent<CollectiveInjector>()
                            .SetGroupName("GpuCluster")
                            .AddConstructor<RingReduce>();
    return tid;
}

RingReduce::RingReduce()
    : m_numGpus(0),
      m_dataSize(0),
      m_chunkSize(0),
      m_pipeChunkSize(0),
      m_flowId(1),
      m_rootRank(0),
      m_headRank(0),
      m_protocolId(0),
      m_state(ReduceState::IDLE),
      m_startupDelayNs(0),
      m_startTimeNs(0),
      m_endTimeNs(0)
{
}

RingReduce::~RingReduce()
{
}

void
RingReduce::DoDispose()
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
                MakeCallback(&RingReduce::OnPacketReceived, this));
        }
    }
    m_endpoints.clear();
    m_completionCallback = nullptr;
    CollectiveInjector::DoDispose();
}

void
RingReduce::Initialize(uint16_t numGpus, uint64_t dataSize,
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

    NS_LOG_INFO("RingReduce initialized: numGpus=" << numGpus
                << " dataSize=" << dataSize << " pipeChunk=" << m_pipeChunkSize
                << " rootRank=" << m_rootRank
                << " protocol=" << static_cast<int>(wireProto)
                << " startupDelay=" << m_startupDelayNs << "ns");
}

void
RingReduce::SetupReceiveCallbacks()
{
    for (auto& ep : m_endpoints)
    {
        if (ep)
        {
            ep->SetReceiveCallback(
                MakeCallback(&RingReduce::OnPacketReceived, this));
        }
    }
}

void
RingReduce::SetCompletionCallback(std::function<void(uint64_t durationNs)> cb)
{
    m_completionCallback = cb;
}

void
RingReduce::SetStartupDelay(Time delay)
{
    m_startupDelayNs = delay.GetNanoSeconds();
}

void
RingReduce::SetRootRank(uint16_t rootRank)
{
    m_rootRank = rootRank;
}

void
RingReduce::Start()
{
    m_state = ReduceState::RUNNING;
    m_startTimeNs = Simulator::Now().GetNanoSeconds();

    NS_LOG_INFO("Starting Ring Reduce (root=" << m_rootRank << ") with startup delay " << m_startupDelayNs << " ns");

    // Zero-data collective (codex round 20, finding 10): no send ever fires
    // a receive to advance the ring — complete immediately instead of
    // stalling to the 1-hour cap.
    if (m_dataSize == 0)
    {
        NS_LOG_INFO("Ring Reduce trivially complete (zero data)");
        Complete();
        return;
    }

    // Tracked in m_pendingEvents so DoDispose cancels it (raw-`this` target).
    m_pendingEvents.push_back(
        Simulator::Schedule(NanoSeconds(m_startupDelayNs),
                            &RingReduce::TriggerInitialSends, this));
}

void
RingReduce::TriggerInitialSends()
{
    if (m_numGpus <= 1)
    {
        Complete();
        return;
    }
    // The chain head is the root's ring predecessor: nothing flows into it,
    // so its full contribution is ready at start.
    m_headRank = RingPrevOf(m_rootRank);
    PumpForward(m_headRank);
}

uint16_t
RingReduce::RingPrevOf(uint16_t gpu) const
{
    // Counterclockwise (toward root). Prefer topology-embedded prev neighbor,
    // fall back to rank arithmetic when no embedding is installed.
    uint16_t prev = m_endpoints[gpu]->GetRingPrev();
    return (prev != NO_RANK) ? prev : (gpu - 1 + m_numGpus) % m_numGpus;
}

void
RingReduce::PumpForward(uint16_t gpu)
{
    if (gpu >= m_endpoints.size() || !m_endpoints[gpu] || gpu == m_rootRank)
    {
        return;
    }

    uint16_t target = RingPrevOf(gpu);
    uint64_t ready = (gpu == m_headRank)
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
RingReduce::OnPacketReceived(uint16_t srcRank, Ptr<Packet> packet, FabricHeader header)
{
    // NOTE: the callback's first argument is the RECEIVING endpoint's rank
    // (FabricEndpoint passes m_rank); the sender would be header.GetSourceRank().
    if (header.GetPacketType() != FabricPacketType::DATA)
    {
        return;
    }

    uint16_t destRank = header.GetDestRank();
    if (destRank >= m_numGpus || m_state != ReduceState::RUNNING)
    {
        return;
    }

    uint64_t effectiveSize = header.GetEffectiveDataSize();
    if (effectiveSize == 0)
    {
        effectiveSize = packet->GetSize();
    }

    m_recvBytes[destRank] += effectiveSize;

    NS_LOG_DEBUG("GPU " << destRank << " received " << effectiveSize
                 << " (total " << m_recvBytes[destRank] << "/" << m_dataSize << ")");

    if (destRank == m_rootRank)
    {
        if (m_recvBytes[destRank] >= m_dataSize)
        {
            NS_LOG_INFO("Root " << destRank << " received fully reduced data");
            Complete();
        }
        return;
    }

    PumpForward(destRank);
}

CollectiveType
RingReduce::GetCollectiveType() const
{
    return CollectiveType::REDUCE;
}

bool
RingReduce::IsCompleted() const
{
    return m_state == ReduceState::COMPLETED;
}

void
RingReduce::Complete()
{
    m_state = ReduceState::COMPLETED;
    m_endTimeNs = Simulator::Now().GetNanoSeconds();

    uint64_t durationNs = m_endTimeNs - m_startTimeNs;

    NS_LOG_INFO("Ring Reduce COMPLETE in " << durationNs / 1000.0 << " µs");

    if (m_completionCallback)
    {
        m_completionCallback(durationNs);
    }

}

} // namespace ns3
