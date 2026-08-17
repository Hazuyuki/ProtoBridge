/*
 * SPDX-License-Identifier: GPL-2.0-only
 * ring-broadcast.cc
 *
 * Pipelined Ring Broadcast with protocol-aware sending.
 * Root rank has dataSize data. N-1 steps clockwise:
 * Step 0: root sends to (root+1)%N
 * Step k: (root+k)%N forwards to (root+k+1)%N
 * After N-1 steps all GPUs have root's data.
 * The last receiver does NOT forward.
 */

#include "ring-broadcast.h"
#include "ns3/log.h"
#include "ns3/simulator.h"
#include "nccl-protocol.h"
#include "protocol-model.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("RingBroadcast");

NS_OBJECT_ENSURE_REGISTERED(RingBroadcast);

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
      m_segmentSize(0),
      m_flowId(1),
      m_rootRank(0),
      m_state(BroadcastState::IDLE),
      m_startupDelayNs(0),
      m_currentSenderRank(0),
      m_currentStep(0),
      m_senderReceivedBytes(0),
      m_gpuDoneCount(0),
      m_totalSteps(0),
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

    m_segmentSize = dataSize / numGpus;
    if (m_segmentSize == 0)
    {
        m_segmentSize = dataSize;
    }

    m_totalSteps = numGpus - 1;
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

    NS_LOG_INFO("RingBroadcast initialized: numGpus=" << numGpus
                << " dataSize=" << dataSize << " segmentSize=" << m_segmentSize
                << " rootRank=" << m_rootRank
                << " totalSteps=" << m_totalSteps
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
    m_currentSenderRank = m_rootRank;
    m_currentStep = 0;

    NS_LOG_INFO("Starting Ring Broadcast (root=" << m_rootRank << ") with startup delay " << m_startupDelayNs << " ns");

    Simulator::Schedule(NanoSeconds(m_startupDelayNs), &RingBroadcast::TriggerInitialSends, this);
}

void
RingBroadcast::TriggerInitialSends()
{
    // Root sends step 0 to (root+1)%N
    SendStepData(m_currentSenderRank, m_currentStep);
}

void
RingBroadcast::SendStepData(uint16_t sender, uint32_t step)
{
    if (sender >= m_endpoints.size() || !m_endpoints[sender])
    {
        return;
    }

    // Broadcast: clockwise. Prefer topology-embedded next neighbor, fall back
    // to rank arithmetic when no embedding is installed.
    uint16_t next = m_endpoints[sender]->GetRingNext();
    uint16_t target = (next != 0xFFFF) ? next : (sender + 1) % m_numGpus;
    std::vector<uint8_t> data(m_segmentSize, 0xAB);

    uint16_t stepFlowId = m_flowId + step;

    m_endpoints[sender]->SendDataWithProtocol(target, data.data(), m_segmentSize, m_protocolId, stepFlowId, 0);

    NS_LOG_INFO("GPU " << sender << " sending step " << step
                 << " to GPU " << target
                 << " flowId=" << stepFlowId);
}

void
RingBroadcast::OnPacketReceived(uint16_t srcRank, Ptr<Packet> packet, FabricHeader header)
{
    if (header.GetPacketType() != FabricPacketType::DATA)
    {
        return;
    }

    uint16_t actualSrc = header.GetSourceRank();
    uint16_t destRank = header.GetDestRank();

    // Only process packets destined for the current step's target
    uint16_t expectedTarget = (m_currentSenderRank + 1) % m_numGpus;
    if (destRank != expectedTarget)
    {
        NS_LOG_INFO("Ignoring packet: dest=" << destRank << " expected=" << expectedTarget);
        return;
    }

    uint32_t effectiveSize = header.GetEffectiveDataSize();
    if (effectiveSize == 0)
    {
        effectiveSize = packet->GetSize();
    }

    m_senderReceivedBytes += effectiveSize;

    NS_LOG_INFO("Packet: src=" << actualSrc << " -> dest=" << destRank
                 << " effectiveSize=" << effectiveSize
                 << " totalReceived=" << m_senderReceivedBytes
                 << " step=" << m_currentStep);

    if (m_senderReceivedBytes >= m_segmentSize)
    {
        m_senderReceivedBytes = 0;
        m_gpuDoneCount++;

        NS_LOG_INFO("GPU " << destRank << " received step " << m_currentStep
                     << " (done=" << m_gpuDoneCount << "/" << (m_numGpus - 1) << ")");

        // Advance: next step's sender is the GPU that just received
        m_currentSenderRank = destRank;
        m_currentStep++;

        if (m_currentStep >= m_totalSteps)
        {
            // All N-1 non-root GPUs have received the data
            Complete();
            return;
        }

        // This GPU now forwards to the next one
        Simulator::ScheduleNow(&RingBroadcast::SendStepData, this, destRank, m_currentStep);
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