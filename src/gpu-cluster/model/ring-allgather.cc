/*
 * SPDX-License-Identifier: GPL-2.0-only
 * ring-allgather.cc
 *
 * Pipelined Ring AllGather with protocol-aware sending.
 * N-1 steps counterclockwise: GPU i sends to (i-1+N)%N.
 * After N-1 steps every GPU has all N segments (full dataSize).
 */

#include "ring-allgather.h"
#include "ns3/log.h"
#include "ns3/simulator.h"
#include "nccl-protocol.h"
#include "protocol-model.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("RingAllGather");

NS_OBJECT_ENSURE_REGISTERED(RingAllGather);

TypeId
RingAllGather::GetTypeId()
{
    static TypeId tid = TypeId("ns3::RingAllGather")
                            .SetParent<CollectiveInjector>()
                            .SetGroupName("GpuCluster")
                            .AddConstructor<RingAllGather>();
    return tid;
}

RingAllGather::RingAllGather()
    : m_numGpus(0),
      m_dataSize(0),
      m_segmentSize(0),
      m_flowId(1),
      m_state(AllGatherState::IDLE),
      m_startupDelayNs(0),
      m_perStepSwOverheadNs(0),
      m_gpuDoneCount(0),
      m_totalSteps(0),
      m_startTimeNs(0),
      m_endTimeNs(0)
{
}

RingAllGather::~RingAllGather()
{
}

void
RingAllGather::DoDispose()
{
    m_endpoints.clear();
    m_completionCallback = nullptr;
    CollectiveInjector::DoDispose();
}

void
RingAllGather::Initialize(uint16_t numGpus, uint64_t dataSize,
                          const std::vector<Ptr<FabricEndpoint>>& endpoints)
{
    m_numGpus = numGpus;
    m_dataSize = dataSize;
    m_endpoints = endpoints;

    m_segmentSize = dataSize / numGpus;
    if (m_segmentSize == 0)
    {
        m_segmentSize = dataSize;
    }

    m_totalSteps = numGpus - 1;

    m_gpuCompletedStep.resize(numGpus, 0);
    m_gpuStepReceivedBytes.resize(numGpus, 0);
    m_gpuReceivedBytesByStep.assign(numGpus,
                                    std::vector<uint64_t>(m_totalSteps, 0));
    m_gpuDoneCount = 0;

    m_flowId = 1;

    // Startup delay from total data size, wire protocol from per-step size
    Ptr<ProtocolModel> protoModel = m_endpoints[0]->GetProtocolModel();
    NcclProtocol startupProto = static_cast<NcclProtocol>(protoModel->GetProtocolId(m_dataSize));
    if (m_startupDelayNs == 0)
    {
        m_startupDelayNs = protoModel->GetStartupDelayNs(static_cast<uint8_t>(startupProto), m_numGpus);
    }
    NcclProtocol wireProto = static_cast<NcclProtocol>(protoModel->GetProtocolId(m_segmentSize));
    m_protocolId = static_cast<uint8_t>(wireProto);


    SetupReceiveCallbacks();

    NS_LOG_INFO("RingAllGather initialized: numGpus=" << numGpus
                << " dataSize=" << dataSize << " segmentSize=" << m_segmentSize
                << " totalSteps=" << m_totalSteps
                << " protocol=" << static_cast<int>(wireProto)
                << " startupDelay=" << m_startupDelayNs << "ns");
}

void
RingAllGather::SetupReceiveCallbacks()
{
    for (auto& ep : m_endpoints)
    {
        if (ep)
        {
            ep->SetReceiveCallback(
                MakeCallback(&RingAllGather::OnPacketReceived, this));
        }
    }
}

void
RingAllGather::SetCompletionCallback(std::function<void(uint64_t durationNs)> cb)
{
    m_completionCallback = cb;
}

void
RingAllGather::SetStartupDelay(Time delay)
{
    m_startupDelayNs = delay.GetNanoSeconds();
}

void
RingAllGather::SetPerStepSwOverhead(Time delay)
{
    m_perStepSwOverheadNs = delay.GetNanoSeconds();
}

void
RingAllGather::Start()
{
    m_state = AllGatherState::RUNNING;
    m_startTimeNs = Simulator::Now().GetNanoSeconds();

    NS_LOG_INFO("Starting Ring AllGather with startup delay " << m_startupDelayNs << " ns");

    Simulator::Schedule(NanoSeconds(m_startupDelayNs), &RingAllGather::TriggerInitialSends, this);
}

void
RingAllGather::TriggerInitialSends()
{
    for (uint16_t gpu = 0; gpu < m_numGpus; gpu++)
    {
        SendGpuStepData(gpu, 0);
    }
}

void
RingAllGather::SendGpuStepData(uint16_t gpu, uint32_t step)
{
    if (gpu >= m_endpoints.size() || !m_endpoints[gpu])
    {
        return;
    }

    // AllGather: counterclockwise. Prefer topology-embedded prev neighbor,
    // fall back to rank arithmetic when no embedding is installed.
    uint16_t prev = m_endpoints[gpu]->GetRingPrev();
    uint16_t target = (prev != 0xFFFF) ? prev : (gpu - 1 + m_numGpus) % m_numGpus;
    uint16_t stepFlowId = m_flowId + step;

    m_endpoints[gpu]->SendBulkWireTransferSize(
        target, m_segmentSize, m_protocolId, stepFlowId, 0);

    NS_LOG_DEBUG("GPU " << gpu << " sending step " << step
                 << " to GPU " << target
                 << " flowId=" << stepFlowId);
}

void
RingAllGather::OnPacketReceived(uint16_t srcRank, Ptr<Packet> packet, FabricHeader header)
{
    if (header.GetPacketType() != FabricPacketType::DATA)
    {
        return;
    }

    uint16_t destRank = header.GetDestRank();
    if (destRank >= m_numGpus || header.GetFlowId() < m_flowId)
    {
        return;
    }

    uint32_t packetStep = header.GetFlowId() - m_flowId;
    if (packetStep >= m_totalSteps)
    {
        return;
    }

    uint32_t effectiveSize = header.GetEffectiveDataSize();
    if (effectiveSize == 0)
    {
        effectiveSize = packet->GetSize();
    }

    m_gpuReceivedBytesByStep[destRank][packetStep] += effectiveSize;
    uint32_t currentStep = m_gpuCompletedStep[destRank];
    if (currentStep < m_totalSteps)
    {
        m_gpuStepReceivedBytes[destRank] =
            m_gpuReceivedBytesByStep[destRank][currentStep];
    }

    NS_LOG_DEBUG("Packet: src=" << header.GetSourceRank() << " -> dest=" << destRank
                 << " effectiveSize=" << effectiveSize
                 << " totalReceived=" << m_gpuReceivedBytesByStep[destRank][packetStep]
                 << " packetStep=" << packetStep
                 << " currentStep=" << currentStep);

    while (currentStep < m_totalSteps
           && m_gpuReceivedBytesByStep[destRank][currentStep] >= m_segmentSize)
    {
        m_gpuReceivedBytesByStep[destRank][currentStep] -= m_segmentSize;
        m_gpuStepReceivedBytes[destRank] = 0;
        m_gpuCompletedStep[destRank] = currentStep + 1;

        NS_LOG_INFO("GPU " << destRank << " completed step " << currentStep
                     << " (now at step " << m_gpuCompletedStep[destRank] << ")");

        OnGpuStepReceiveComplete(destRank, currentStep);
        currentStep = m_gpuCompletedStep[destRank];
        if (currentStep < m_totalSteps)
        {
            m_gpuStepReceivedBytes[destRank] =
                m_gpuReceivedBytesByStep[destRank][currentStep];
        }
    }
}

void
RingAllGather::OnGpuStepReceiveComplete(uint16_t gpu, uint32_t step)
{
    if (m_gpuCompletedStep[gpu] >= m_totalSteps)
    {
        m_gpuDoneCount++;
        NS_LOG_INFO("GPU " << gpu << " DONE (total done=" << m_gpuDoneCount << "/" << m_numGpus << ")");
        CheckGlobalCompletion();
        return;
    }

    if (m_perStepSwOverheadNs > 0)
    {
        Simulator::Schedule(NanoSeconds(m_perStepSwOverheadNs),
                            &RingAllGather::SendGpuStepData,
                            this,
                            gpu,
                            step + 1);
    }
    else
    {
        Simulator::ScheduleNow(&RingAllGather::SendGpuStepData, this, gpu, step + 1);
    }
}

void
RingAllGather::CheckGlobalCompletion()
{
    if (m_gpuDoneCount >= m_numGpus)
    {
        Complete();
    }
}

CollectiveType
RingAllGather::GetCollectiveType() const
{
    return CollectiveType::ALLGATHER;
}

bool
RingAllGather::IsCompleted() const
{
    return m_state == AllGatherState::COMPLETED;
}

void
RingAllGather::Complete()
{
    m_state = AllGatherState::COMPLETED;
    m_endTimeNs = Simulator::Now().GetNanoSeconds();

    uint64_t durationNs = m_endTimeNs - m_startTimeNs;

    NS_LOG_INFO("Ring AllGather COMPLETE in " << durationNs / 1000.0 << " µs");

    if (m_completionCallback)
    {
        m_completionCallback(durationNs);
    }

}

} // namespace ns3
