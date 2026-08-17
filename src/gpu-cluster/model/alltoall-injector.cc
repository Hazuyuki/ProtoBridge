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

    m_chunkSize = dataSize / numGpus;
    if (m_chunkSize == 0)
    {
        m_chunkSize = dataSize;
    }

    m_totalSteps = numGpus - 1;

    // Ring pipeline mode: per-GPU step tracking
    m_gpuCompletedStep.resize(numGpus, 0);
    m_gpuStepReceivedBytes.resize(numGpus, 0);

    // Concurrent mode: per-GPU received bytes from all sources
    m_gpuReceivedBytes.resize(numGpus, 0);
    m_gpuSourcesReceived.resize(numGpus, 0);

    m_totalCompletedGpus = 0;

    Ptr<ProtocolModel> protoModel = m_endpoints[0]->GetProtocolModel();
    NcclProtocol startupProto = static_cast<NcclProtocol>(protoModel->GetProtocolId(m_dataSize));
    if (m_startupDelayNs == 0)
    {
        m_startupDelayNs = protoModel->GetStartupDelayNs(static_cast<uint8_t>(startupProto), m_numGpus);
    }
    NcclProtocol wireProto = static_cast<NcclProtocol>(protoModel->GetProtocolId(m_chunkSize));
    m_protocolId = static_cast<uint8_t>(wireProto);

    SetupReceiveCallbacks();

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

    Simulator::Schedule(NanoSeconds(m_startupDelayNs),
                         m_mode != AlltoAllMode::RING_PIPELINE
                             ? &AlltoAllInjector::TriggerConcurrentSends
                             : &AlltoAllInjector::TriggerInitialSends,
                         this);
}

// --- Ring Pipeline Mode ---

void
AlltoAllInjector::TriggerInitialSends()
{
    for (uint16_t gpu = 0; gpu < m_numGpus; gpu++)
    {
        SendGpuStepData(gpu, 0);
    }
}

void
AlltoAllInjector::SendGpuStepData(uint16_t gpu, uint32_t step)
{
    if (gpu >= m_endpoints.size() || !m_endpoints[gpu])
    {
        return;
    }

    uint16_t dest = (gpu + step + 1) % m_numGpus;

    std::vector<uint8_t> data(m_chunkSize, 0xAB);
    uint16_t stepFlowId = 1 + step;

    m_endpoints[gpu]->SendBulkWireTransfer(dest, data.data(), m_chunkSize, m_protocolId, stepFlowId, 0);

    NS_LOG_DEBUG("GPU " << gpu << " step " << step << " sending to GPU " << dest
                 << " chunkSize=" << m_chunkSize << " flowId=" << stepFlowId);
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

    m_endpoints[gpu]->SendBulkWireTransferSize(nextHop,
                                                m_chunkSize,
                                                m_protocolId,
                                                flowId,
                                                0);

    NS_LOG_DEBUG("GPU " << gpu << " concurrent send toward GPU " << dest
                 << " via GPU " << nextHop
                 << " chunkSize=" << m_chunkSize << " flowId=" << flowId);
}

void
AlltoAllInjector::ForwardTwoDimensionalChunk(uint16_t intermediate,
                                             uint16_t finalDest,
                                             uint16_t flowId)
{
    m_endpoints[intermediate]->SendBulkWireTransferSize(finalDest,
                                                        m_chunkSize,
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
        auto key = std::make_pair(srcRank, finalDest);
        uint64_t& received = m_firstHopReceivedBytes[key];
        received += effectiveSize;
        if (received >= m_chunkSize)
        {
            m_firstHopReceivedBytes.erase(key);
            Simulator::ScheduleNow(&AlltoAllInjector::ForwardTwoDimensionalChunk,
                                   this,
                                   destRank,
                                   finalDest,
                                   static_cast<uint16_t>(finalDest + 1));
        }
        return;
    }

    if (m_mode != AlltoAllMode::RING_PIPELINE)
    {
        m_gpuReceivedBytes[destRank] += effectiveSize;

        NS_LOG_DEBUG("Concurrent: src=" << srcRank << " -> dest=" << destRank
                     << " effectiveSize=" << effectiveSize
                     << " totalReceived=" << m_gpuReceivedBytes[destRank]);

        // Check if this source's chunk is fully received at this destination
        if (m_gpuReceivedBytes[destRank] >= m_chunkSize * (m_gpuSourcesReceived[destRank] + 1))
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

        if (m_gpuStepReceivedBytes[destRank] >= m_chunkSize)
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

    Simulator::ScheduleNow(&AlltoAllInjector::SendGpuStepData, this, gpu, step + 1);
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
