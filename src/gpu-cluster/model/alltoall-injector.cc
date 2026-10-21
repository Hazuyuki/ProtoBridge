/*
 * SPDX-License-Identifier: GPL-2.0-only
 * alltoall-injector.cc
 *
 * AlltoAll collective injector supporting two modes:
 * - RING_PIPELINE: Sequential N-1 ring steps, each sends chunk to next ring neighbor.
 * - CONCURRENT: All N-1 chunks sent simultaneously to all destinations (switched topology).
 */

#include "alltoall-injector.h"
#include "ns3/log.h"
#include "ns3/simulator.h"
#include "nccl-protocol.h"
#include "protocol-model.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("AlltoAllInjector");

NS_OBJECT_ENSURE_REGISTERED(AlltoAllInjector);

TypeId
AlltoAllInjector::GetTypeId()
{
    static TypeId tid = TypeId("ns3::AlltoAllInjector")
                            .SetParent<CollectiveInjector>()
                            .SetGroupName("GpuCluster")
                            .AddConstructor<AlltoAllInjector>();
    return tid;
}

AlltoAllInjector::AlltoAllInjector()
    : m_numGpus(0),
      m_dataSize(0),
      m_chunkSize(0),
      m_chunkRemainder(0),
      m_sentDataBytes(0),
      m_protocolId(0),
      m_state(AlltoAllState::IDLE),
      m_mode(AlltoAllMode::RING_PIPELINE),
      m_startupDelayNs(0),
      m_totalSteps(0),
      m_rows(0),
      m_cols(0),
      m_startTimeNs(0),
      m_endTimeNs(0),
      m_totalCompletedGpus(0)
{
}

AlltoAllInjector::~AlltoAllInjector()
{
}

void
AlltoAllInjector::DoDispose()
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
AlltoAllInjector::Initialize(uint16_t numGpus, uint64_t dataSize,
                              const std::vector<Ptr<FabricEndpoint>>& endpoints)
{
    m_numGpus = numGpus;
    m_dataSize = dataSize;
    m_endpoints = endpoints;

    // Chunk grid (codex round 17, finding 7): rank r owns dataSize/numGpus
    // bytes plus one extra byte when r < dataSize % numGpus, so the grid
    // sums to exactly dataSize. The previous uniform `m_chunkSize` (with a
    // whole-dataSize fallback when the quotient was zero) silently dropped
    // the remainder — 15 B over 4 GPUs reported completion after 36
    // aggregate bytes instead of 45 — and, in the degenerate dataSize <
    // numGpus case, made every GPU send the WHOLE dataSize (3 B over 4 GPUs
    // sent 36 bytes instead of nine).
    m_chunkSize = dataSize / numGpus;
    m_chunkRemainder = dataSize % numGpus;
    if (m_chunkSize == 0 && dataSize > 0)
    {
        // Degenerate dataSize < numGpus: one byte on each of the first
        // dataSize ranks, zero on the rest (grid still sums to dataSize).
        m_chunkSize = 0;
        m_chunkRemainder = dataSize;
    }

    m_totalSteps = numGpus - 1;
    m_sentDataBytes = 0;

    // Ring pipeline mode: per-GPU step tracking
    m_gpuCompletedStep.resize(numGpus, 0);
    m_gpuStepReceivedBytes.resize(numGpus, 0);

    // Concurrent mode: per-GPU received bytes from all sources
    m_gpuReceivedBytes.resize(numGpus, 0);
    m_gpuSourcesReceived.resize(numGpus, 0);

    m_totalCompletedGpus = 0;

    // Degenerate grids give ranks past dataSize a zero-sized chunk: no
    // source ever sends them anything (the zero-byte bulk guard makes a
    // zero-chunk transfer a no-op), so their receive trackers would never
    // advance. Pre-credit those ranks as already complete instead of
    // stalling the collective forever.
    if (m_chunkSize == 0 && m_chunkRemainder < numGpus)
    {
        for (uint16_t rank = 0; rank < numGpus; rank++)
        {
            if (ChunkSizeOf(rank) == 0)
            {
                m_gpuSourcesReceived[rank] = numGpus - 1;
                m_gpuCompletedStep[rank] = m_totalSteps;
                m_totalCompletedGpus++;
            }
        }
    }

    Ptr<ProtocolModel> protoModel = m_endpoints[0]->GetProtocolModel();
    NcclProtocol startupProto = static_cast<NcclProtocol>(protoModel->GetProtocolId(m_dataSize));
    if (m_startupDelayNs == 0)
    {
        m_startupDelayNs = protoModel->GetStartupDelayNs(static_cast<uint8_t>(startupProto), m_numGpus);
    }
    NcclProtocol wireProto = static_cast<NcclProtocol>(protoModel->GetProtocolId(m_chunkSize));
    m_protocolId = static_cast<uint8_t>(wireProto);

    SetupReceiveCallbacks();

    if (m_totalCompletedGpus >= m_numGpus && m_numGpus > 0)
    {
        // Every rank holds a zero-sized chunk (dataSize == 0): the
        // pre-crediting above already finished the collective.
        Complete();
    }

    NS_LOG_INFO("AlltoAll initialized: numGpus=" << numGpus
                << " dataSize=" << dataSize << " chunkSize=" << m_chunkSize
                << " totalSteps=" << m_totalSteps
                << " mode=" << (m_mode == AlltoAllMode::CONCURRENT ? "CONCURRENT" : "RING_PIPELINE")
                << " protocol=" << static_cast<int>(wireProto)
                << " startupDelay=" << m_startupDelayNs << "ns");
}

void
AlltoAllInjector::SetupReceiveCallbacks()
{
    for (auto& ep : m_endpoints)
    {
        if (ep)
        {
            ep->SetReceiveCallback(
                MakeCallback(&AlltoAllInjector::OnPacketReceived, this));
        }
    }
}

void
AlltoAllInjector::SetCompletionCallback(std::function<void(uint64_t durationNs)> cb)
{
    m_completionCallback = cb;
}

void
AlltoAllInjector::SetStartupDelay(Time delay)
{
    m_startupDelayNs = delay.GetNanoSeconds();
}

void
AlltoAllInjector::SetSegmentSize(uint64_t bytes)
{
    // Segment size not used in current implementation — kept for API compatibility
}

void
AlltoAllInjector::SetConcurrentMode(bool concurrent)
{
    m_mode = concurrent ? AlltoAllMode::CONCURRENT : AlltoAllMode::RING_PIPELINE;
}

void
AlltoAllInjector::SetTwoDimensionalRouting(uint32_t rows, uint32_t cols)
{
    NS_ABORT_MSG_IF(rows * cols != m_numGpus,
                    "2D AllToAll dimensions must match the number of devices");
    NS_ABORT_MSG_IF(m_numGpus >= 0x8000,
                    "2D AllToAll flow encoding supports fewer than 32768 devices");
    m_rows = rows;
    m_cols = cols;
    m_mode = AlltoAllMode::TWO_DIMENSIONAL;
}

void
AlltoAllInjector::Start()
{
    m_state = AlltoAllState::RUNNING;
    m_startTimeNs = Simulator::Now().GetNanoSeconds();

    NS_LOG_INFO("Starting AlltoAll with startup delay " << m_startupDelayNs << " ns"
                << " mode=" << static_cast<int>(m_mode));

    // Tracked in m_pendingEvents so DoDispose cancels it (raw-`this` target).
    m_pendingEvents.push_back(
        Simulator::Schedule(NanoSeconds(m_startupDelayNs),
                            m_mode != AlltoAllMode::RING_PIPELINE
                                ? &AlltoAllInjector::TriggerConcurrentSends
                                : &AlltoAllInjector::TriggerInitialSends,
                            this));
}

// --- Ring Pipeline Mode ---

void
AlltoAllInjector::TriggerInitialSends()
{
    for (uint16_t gpu = 0; gpu < m_numGpus; gpu++)
    {
        SendGpuStepData(gpu, 0);
    }
    // Degenerate-grid zero-chunk ranks: pre-credited at Initialize (they
    // receive nothing), so the receive-complete chain never schedules their
    // sends. Drive their whole chain here instead — their partners are
    // waiting for those chunks (a zero-chunk rank still OWES the chunks of
    // every other rank).
    for (uint16_t gpu = 0; gpu < m_numGpus; gpu++)
    {
        if (m_gpuCompletedStep[gpu] >= m_totalSteps)
        {
            for (uint32_t step = 1; step < m_totalSteps; step++)
            {
                SendGpuStepData(gpu, step);
            }
        }
    }
}

uint64_t
AlltoAllInjector::ChunkSizeOf(uint16_t gpu) const
{
    // Chunk `gpu` of the grid: base size plus one byte for the first
    // dataSize % numGpus chunks (see the Initialize comment).
    return m_chunkSize + (gpu < m_chunkRemainder ? 1 : 0);
}

void
AlltoAllInjector::SendGpuStepData(uint16_t gpu, uint32_t step)
{
    if (gpu >= m_endpoints.size() || !m_endpoints[gpu])
    {
        return;
    }

    uint16_t dest = (gpu + step + 1) % m_numGpus;

    // The transfer moves the DESTINATION's chunk of the grid (codex
    // round 17, finding 7): the ring still delivers one chunk per step
    // per destination, but each chunk is sized by its owner, so the
    // aggregate over all N-1 steps is (N-1)*dataSize and per-destination
    // load is the sum of the other ranks' chunks.
    const uint64_t chunkBytes = ChunkSizeOf(dest);
    std::vector<uint8_t> data(chunkBytes, 0xAB);
    uint16_t stepFlowId = 1 + step;

    m_sentDataBytes += chunkBytes;
    m_endpoints[gpu]->SendBulkWireTransfer(dest, data.data(), chunkBytes, m_protocolId, stepFlowId, 0);

    NS_LOG_DEBUG("GPU " << gpu << " step " << step << " sending to GPU " << dest
                 << " chunkSize=" << chunkBytes << " flowId=" << stepFlowId);
}

// --- Concurrent Mode ---

void
AlltoAllInjector::TriggerConcurrentSends()
{
    for (uint16_t gpu = 0; gpu < m_numGpus; gpu++)
    {
        for (uint16_t dest = 0; dest < m_numGpus; dest++)
        {
            if (dest == gpu)
            {
                continue;
            }
            SendGpuConcurrentChunk(gpu, dest);
        }
    }
}

void
AlltoAllInjector::SendGpuConcurrentChunk(uint16_t gpu, uint16_t dest)
{
    if (gpu >= m_endpoints.size() || !m_endpoints[gpu])
    {
        return;
    }

    uint16_t flowId = dest + 1;
    uint16_t nextHop = dest;
    if (m_mode == AlltoAllMode::TWO_DIMENSIONAL)
    {
        uint32_t srcRow = gpu / m_cols;
        uint32_t srcCol = gpu % m_cols;
        uint32_t dstRow = dest / m_cols;
        uint32_t dstCol = dest % m_cols;
        if (srcRow != dstRow && srcCol != dstCol)
        {
            nextHop = static_cast<uint16_t>(srcRow * m_cols + dstCol);
            flowId = static_cast<uint16_t>(0x8000 | dest);
        }
    }

    // Destination-owned chunk sizing (codex round 17, finding 7): each
    // source sends its copy of chunk `dest`, so per-destination load is
    // (N-1)*ChunkSizeOf(dest) and zero-chunk ranks in a degenerate grid
    // send nothing (the zero-byte bulk guard makes that a safe no-op).
    const uint64_t chunkBytes = ChunkSizeOf(dest);
    m_sentDataBytes += chunkBytes;
    m_endpoints[gpu]->SendBulkWireTransferSize(nextHop,
                                                chunkBytes,
                                                m_protocolId,
                                                flowId,
                                                0);

    NS_LOG_DEBUG("GPU " << gpu << " concurrent send toward GPU " << dest
                 << " via GPU " << nextHop
                 << " chunkSize=" << chunkBytes << " flowId=" << flowId);
}

void
AlltoAllInjector::ForwardTwoDimensionalChunk(uint16_t intermediate,
                                             uint16_t finalDest,
                                             uint16_t flowId)
{
    // Forwarded byte count matches the first hop's destination-owned chunk.
    const uint64_t chunkBytes = ChunkSizeOf(finalDest);
    m_sentDataBytes += chunkBytes;
    m_endpoints[intermediate]->SendBulkWireTransferSize(finalDest,
                                                        chunkBytes,
                                                        m_protocolId,
                                                        flowId,
                                                        0);
}

// --- Receive Handler (shared by both modes) ---

void
AlltoAllInjector::OnPacketReceived(uint16_t srcRank, Ptr<Packet> packet, FabricHeader header)
{
    if (header.GetPacketType() != FabricPacketType::DATA)
    {
        return;
    }

    uint16_t destRank = header.GetDestRank();
    uint32_t effectiveSize = header.GetEffectiveDataSize();
    if (effectiveSize == 0)
    {
        effectiveSize = packet->GetSize();
    }

    if (m_mode == AlltoAllMode::TWO_DIMENSIONAL &&
        (header.GetFlowId() & 0x8000) != 0)
    {
        uint16_t finalDest = header.GetFlowId() & 0x7fff;
        // NOTE: the callback's first argument is the RECEIVING endpoint's rank
        // (FabricEndpoint passes m_rank), so the true sender must come from
        // the header; keying on srcRank pooled every same-row sender's bytes
        // into one counter and the erase() below discarded the overshoot.
        auto key = std::make_pair(header.GetSourceRank(), finalDest);
        uint64_t& received = m_firstHopReceivedBytes[key];
        received += effectiveSize;
        // First-hop threshold is the FINAL destination's chunk (the sender
        // moved ChunkSizeOf(finalDest) bytes; codex round 17, finding 7).
        if (received >= ChunkSizeOf(finalDest))
        {
            m_firstHopReceivedBytes.erase(key);
            m_pendingEvents.push_back(
                Simulator::ScheduleNow(&AlltoAllInjector::ForwardTwoDimensionalChunk,
                                       this,
                                       destRank,
                                       finalDest,
                                       static_cast<uint16_t>(finalDest + 1)));
        }
        return;
    }

    if (m_mode != AlltoAllMode::RING_PIPELINE)
    {
        m_gpuReceivedBytes[destRank] += effectiveSize;

        NS_LOG_DEBUG("Concurrent: src=" << srcRank << " -> dest=" << destRank
                     << " effectiveSize=" << effectiveSize
                     << " totalReceived=" << m_gpuReceivedBytes[destRank]);

        // Check if this source's chunk is fully received at this destination.
        // Every source sends the DESTINATION's chunk (ChunkSizeOf(destRank)),
        // so the k-th completion happens at k * ChunkSizeOf(destRank) bytes
        // (codex round 17, finding 7).
        if (m_gpuReceivedBytes[destRank] >=
            ChunkSizeOf(destRank) * (m_gpuSourcesReceived[destRank] + 1))
        {
            m_gpuSourcesReceived[destRank]++;
            NS_LOG_INFO("GPU " << destRank << " received chunk from source "
                         << m_gpuSourcesReceived[destRank] << "/" << (m_numGpus - 1));

            if (m_gpuSourcesReceived[destRank] >= static_cast<uint32_t>(m_numGpus - 1))
            {
                m_totalCompletedGpus++;
                NS_LOG_INFO("GPU " << destRank << " ALLTOALL DONE (total done="
                             << m_totalCompletedGpus << "/" << m_numGpus << ")");
                CheckGlobalCompletion();
            }
        }
    }
    else // RING_PIPELINE
    {
        uint32_t currentStep = m_gpuCompletedStep[destRank];
        m_gpuStepReceivedBytes[destRank] += effectiveSize;

        NS_LOG_DEBUG("Ring: src=" << srcRank << " -> dest=" << destRank
                     << " effectiveSize=" << effectiveSize
                     << " totalReceived=" << m_gpuStepReceivedBytes[destRank]
                     << " step=" << currentStep);

        // Ring threshold: the chunk destined for this rank this step is
        // ChunkSizeOf(destRank) — the ring's owner-sized chunk (codex
        // round 17, finding 7).
        if (m_gpuStepReceivedBytes[destRank] >= ChunkSizeOf(destRank))
        {
            m_gpuStepReceivedBytes[destRank] = 0;
            m_gpuCompletedStep[destRank] = currentStep + 1;

            NS_LOG_INFO("GPU " << destRank << " completed step " << currentStep
                         << " (now at step " << m_gpuCompletedStep[destRank] << ")");

            OnGpuStepReceiveComplete(destRank, currentStep);
        }
    }
}

void
AlltoAllInjector::OnGpuStepReceiveComplete(uint16_t gpu, uint32_t step)
{
    if (m_gpuCompletedStep[gpu] >= m_totalSteps)
    {
        m_totalCompletedGpus++;
        NS_LOG_INFO("GPU " << gpu << " ALLTOALL DONE (total done="
                     << m_totalCompletedGpus << "/" << m_numGpus << ")");
        CheckGlobalCompletion();
        return;
    }

    m_pendingEvents.push_back(
        Simulator::ScheduleNow(&AlltoAllInjector::SendGpuStepData, this, gpu, step + 1));
}

void
AlltoAllInjector::CheckGlobalCompletion()
{
    if (m_totalCompletedGpus >= m_numGpus)
    {
        Complete();
    }
}

CollectiveType
AlltoAllInjector::GetCollectiveType() const
{
    return CollectiveType::ALLTOALL;
}

bool
AlltoAllInjector::IsCompleted() const
{
    return m_state == AlltoAllState::COMPLETED;
}

void
AlltoAllInjector::Complete()
{
    m_state = AlltoAllState::COMPLETED;
    m_endTimeNs = Simulator::Now().GetNanoSeconds();

    uint64_t durationNs = m_endTimeNs - m_startTimeNs;

    NS_LOG_INFO("AlltoAll COMPLETE in " << durationNs / 1000.0 << " µs");

    if (m_completionCallback)
    {
        m_completionCallback(durationNs);
    }

}

} // namespace ns3
