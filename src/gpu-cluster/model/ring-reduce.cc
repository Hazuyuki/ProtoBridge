/*
 * SPDX-License-Identifier: GPL-2.0-only
 * ring-reduce.cc
 *
 * Pipelined Ring Reduce with protocol-aware sending.
 * Reverse of Broadcast: N-1 steps counterclockwise.
 * Each GPU sends its segment toward root (CCW).
 * After N-1 steps root has fully reduced dataSize.
 */

#include "ring-reduce.h"
#include "ns3/log.h"
#include "ns3/simulator.h"
#include "nccl-protocol.h"
#include "protocol-model.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("RingReduce");

NS_OBJECT_ENSURE_REGISTERED(RingReduce);

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
      m_segmentSize(0),
      m_flowId(1),
      m_rootRank(0),
      m_state(ReduceState::IDLE),
      m_startupDelayNs(0),
      m_gpuDoneCount(0),
      m_totalSteps(0),
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

    m_segmentSize = dataSize / numGpus;
    if (m_segmentSize == 0)
    {
        m_segmentSize = dataSize;
    }

    m_totalSteps = numGpus - 1;

    m_gpuCompletedStep.resize(numGpus, 0);
    m_gpuStepReceivedBytes.resize(numGpus, 0);
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

    NS_LOG_INFO("RingReduce initialized: numGpus=" << numGpus
                << " dataSize=" << dataSize << " segmentSize=" << m_segmentSize
                << " rootRank=" << m_rootRank
                << " totalSteps=" << m_totalSteps
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

    Simulator::Schedule(NanoSeconds(m_startupDelayNs), &RingReduce::TriggerInitialSends, this);
}

void
RingReduce::TriggerInitialSends()
{
    // All GPUs send step 0 simultaneously (CCW toward root)
    for (uint16_t gpu = 0; gpu < m_numGpus; gpu++)
    {
        SendGpuStepData(gpu, 0);
    }
}

void
RingReduce::SendGpuStepData(uint16_t gpu, uint32_t step)
{
    if (gpu >= m_endpoints.size() || !m_endpoints[gpu])
    {
        return;
    }

    // Reduce: counterclockwise. Prefer topology-embedded prev neighbor,
    // fall back to rank arithmetic when no embedding is installed.
    uint16_t prev = m_endpoints[gpu]->GetRingPrev();
    uint16_t target = (prev != 0xFFFF) ? prev : (gpu - 1 + m_numGpus) % m_numGpus;
    std::vector<uint8_t> data(m_segmentSize, 0xAB);

    uint16_t stepFlowId = m_flowId + step;

    m_endpoints[gpu]->SendDataWithProtocol(target, data.data(), m_segmentSize, m_protocolId, stepFlowId, 0);

    NS_LOG_DEBUG("GPU " << gpu << " sending step " << step
                 << " to GPU " << target
                 << " flowId=" << stepFlowId);
}

void
RingReduce::OnPacketReceived(uint16_t srcRank, Ptr<Packet> packet, FabricHeader header)
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

    uint32_t currentStep = m_gpuCompletedStep[destRank];
    m_gpuStepReceivedBytes[destRank] += effectiveSize;

    NS_LOG_DEBUG("Packet: src=" << header.GetSourceRank() << " -> dest=" << destRank
                 << " effectiveSize=" << effectiveSize
                 << " totalReceived=" << m_gpuStepReceivedBytes[destRank]
                 << " step=" << currentStep);

    if (m_gpuStepReceivedBytes[destRank] >= m_segmentSize)
    {
        m_gpuStepReceivedBytes[destRank] = 0;
        m_gpuCompletedStep[destRank] = currentStep + 1;

        NS_LOG_INFO("GPU " << destRank << " completed step " << currentStep
                     << " (now at step " << m_gpuCompletedStep[destRank] << ")");

        OnGpuStepReceiveComplete(destRank, currentStep);
    }
}

void
RingReduce::OnGpuStepReceiveComplete(uint16_t gpu, uint32_t step)
{
    if (m_gpuCompletedStep[gpu] >= m_totalSteps)
    {
        m_gpuDoneCount++;
        NS_LOG_INFO("GPU " << gpu << " DONE (total done=" << m_gpuDoneCount << "/" << m_numGpus << ")");
        CheckGlobalCompletion();
        return;
    }

    Simulator::ScheduleNow(&RingReduce::SendGpuStepData, this, gpu, step + 1);
}

void
RingReduce::CheckGlobalCompletion()
{
    // Only root needs to have fully received all data
    // But we track all GPU completions — when root has completed N-1 steps, done
    if (m_gpuCompletedStep[m_rootRank] >= m_totalSteps)
    {
        Complete();
    }
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