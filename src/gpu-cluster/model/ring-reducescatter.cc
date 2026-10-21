/*
 * SPDX-License-Identifier: GPL-2.0-only
 * ring-reducescatter.cc
 *
 * Pipelined Ring ReduceScatter with protocol-aware sending.
 * N-1 steps clockwise: GPU i sends to (i+1)%N.
 * After N-1 steps each GPU holds one reduced segment of dataSize/N.
 * This is exactly RingAllReduce's ReduceScatter phase standalone.
 */

#include "ring-reducescatter.h"
#include "ns3/log.h"
#include "ns3/simulator.h"
#include "nccl-protocol.h"
#include "protocol-model.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("RingReduceScatter");

NS_OBJECT_ENSURE_REGISTERED(RingReduceScatter);

TypeId
RingReduceScatter::GetTypeId()
{
    static TypeId tid = TypeId("ns3::RingReduceScatter")
                            .SetParent<CollectiveInjector>()
                            .SetGroupName("GpuCluster")
                            .AddConstructor<RingReduceScatter>();
    return tid;
}

RingReduceScatter::RingReduceScatter()
    : m_numGpus(0),
      m_dataSize(0),
      m_segmentSize(0),
      m_segmentRemainder(0),
      m_flowId(1),
      m_state(ReduceScatterState::IDLE),
      m_startupDelayNs(0),
      m_perStepSwOverheadNs(0),
      m_gpuDoneCount(0),
      m_totalSteps(0),
      m_sentDataBytes(0),
      m_startTimeNs(0),
      m_endTimeNs(0)
{
}

RingReduceScatter::~RingReduceScatter()
{
}

void
RingReduceScatter::DoDispose()
{
    m_endpoints.clear();
    m_ringPos.clear();
    m_completionCallback = nullptr;
    CollectiveInjector::DoDispose();
}

void
RingReduceScatter::Initialize(uint16_t numGpus, uint64_t dataSize,
                              const std::vector<Ptr<FabricEndpoint>>& endpoints)
{
    m_numGpus = numGpus;
    m_dataSize = dataSize;
    m_endpoints = endpoints;

    // Segment grid: rank r's base segment is dataSize/numGpus bytes, with
    // the first dataSize % numGpus segments carrying one extra byte, so
    // the grid sums to exactly dataSize (codex round 15, finding 7: plain
    // integer division silently dropped dataSize % numGpus bytes — the
    // same truncation RingAllReduce fixed in round 13, finding 3).
    m_segmentSize = dataSize / numGpus;
    m_segmentRemainder = dataSize % numGpus;
    if (m_segmentSize == 0)
    {
        // Degenerate dataSize < numGpus: keep the legacy fallback (each
        // rank transmits the whole dataSize every step).
        m_segmentSize = dataSize;
        m_segmentRemainder = 0;
    }
    ComputeRingPositions();

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

    NS_LOG_INFO("RingReduceScatter initialized: numGpus=" << numGpus
                << " dataSize=" << dataSize << " segmentSize=" << m_segmentSize
                << " totalSteps=" << m_totalSteps
                << " protocol=" << static_cast<int>(wireProto)
                << " startupDelay=" << m_startupDelayNs << "ns");
}

void
RingReduceScatter::ComputeRingPositions()
{
    // ReduceScatter sends run clockwise (to ring next): the POSITION table
    // walks that same direction from rank 0 — the same convention
    // RingAllReduce uses. Identity fallback when no embedding is installed
    // or the embedding is not a single cycle covering every rank.
    m_ringPos.assign(m_numGpus, 0);
    bool identity = true;
    if (m_endpoints.size() >= m_numGpus)
    {
        std::vector<bool> seen(m_numGpus, false);
        uint16_t cur = 0;
        uint16_t pos = 0;
        identity = false;
        do
        {
            if (cur >= m_numGpus || seen[cur])
            {
                identity = true;
                break;
            }
            seen[cur] = true;
            m_ringPos[cur] = pos++;
            uint16_t next = 0xFFFF;
            if (m_endpoints[cur])
            {
                next = m_endpoints[cur]->GetRingNext();
            }
            if (next >= m_numGpus)
            {
                identity = true;
                break;
            }
            cur = next;
        } while (cur != 0 && pos < m_numGpus);
        if (!identity && pos != m_numGpus)
        {
            identity = true;
        }
    }
    if (identity)
    {
        for (uint16_t r = 0; r < m_numGpus; ++r)
        {
            m_ringPos[r] = r;
        }
    }
}

uint64_t
RingReduceScatter::SegmentSizeOf(uint16_t gpu, uint32_t step) const
{
    // Which segment the GPU transmits at this step, keyed on ring POSITION
    // (the GPU's slot along the topology-embedded ring), not numeric rank
    // — the same rule RingAllReduce applies (codex round 15, finding 4).
    // ReduceScatter sends run clockwise (to ring next), so ownership of
    // segment c sits at position (c + s) mod N: position p holds segment
    // (p - s) mod N at step s. The receiver (position p+1) at step s+1
    // evaluates (p+1 - (s+1)) = (p - s): the SAME segment — a rank
    // forwards exactly the chunk it received.
    const uint32_t n = m_numGpus;
    const uint32_t pos = (gpu < m_ringPos.size()) ? m_ringPos[gpu] : gpu;
    const uint32_t idx = (pos + n - (step % n)) % n;
    return m_segmentSize + (idx < m_segmentRemainder ? 1 : 0);
}

uint64_t
RingReduceScatter::StepExpectedBytes(uint16_t destRank, uint32_t step) const
{
    // Bytes destRank must receive at `step`: the segment the SENDER at
    // that step holds. ReduceScatter sends to ring next, so the sender is
    // the GPU whose ring prev (or rank-arithmetic predecessor) is destRank.
    for (uint16_t gpu = 0; gpu < m_numGpus; ++gpu)
    {
        uint16_t target;
        if (gpu < m_endpoints.size() && m_endpoints[gpu])
        {
            uint16_t next = m_endpoints[gpu]->GetRingNext();
            target = (next != 0xFFFF) ? next : (gpu + 1) % m_numGpus;
        }
        else
        {
            target = (gpu + 1) % m_numGpus;
        }
        if (target == destRank)
        {
            return SegmentSizeOf(gpu, step);
        }
    }
    return m_segmentSize;
}

void
RingReduceScatter::SetupReceiveCallbacks()
{
    for (auto& ep : m_endpoints)
    {
        if (ep)
        {
            ep->SetReceiveCallback(
                MakeCallback(&RingReduceScatter::OnPacketReceived, this));
        }
    }
}

void
RingReduceScatter::SetCompletionCallback(std::function<void(uint64_t durationNs)> cb)
{
    m_completionCallback = cb;
}

void
RingReduceScatter::SetStartupDelay(Time delay)
{
    m_startupDelayNs = delay.GetNanoSeconds();
}

void
RingReduceScatter::SetPerStepSwOverhead(Time delay)
{
    m_perStepSwOverheadNs = delay.GetNanoSeconds();
}

void
RingReduceScatter::Start()
{
    m_state = ReduceScatterState::RUNNING;
    m_startTimeNs = Simulator::Now().GetNanoSeconds();

    NS_LOG_INFO("Starting Ring ReduceScatter with startup delay " << m_startupDelayNs << " ns");

    Simulator::Schedule(NanoSeconds(m_startupDelayNs), &RingReduceScatter::TriggerInitialSends, this);
}

void
RingReduceScatter::TriggerInitialSends()
{
    for (uint16_t gpu = 0; gpu < m_numGpus; gpu++)
    {
        SendGpuStepData(gpu, 0);
    }
}

void
RingReduceScatter::SendGpuStepData(uint16_t gpu, uint32_t step)
{
    if (gpu >= m_endpoints.size() || !m_endpoints[gpu])
    {
        return;
    }

    // ReduceScatter: clockwise. Prefer topology-embedded next neighbor,
    // fall back to rank arithmetic when no embedding is installed.
    uint16_t next = m_endpoints[gpu]->GetRingNext();
    uint16_t target = (next != 0xFFFF) ? next : (gpu + 1) % m_numGpus;
    uint16_t stepFlowId = m_flowId + step;

    const uint64_t segmentBytes = SegmentSizeOf(gpu, step);
    m_sentDataBytes += segmentBytes;
    m_endpoints[gpu]->SendBulkWireTransferSize(
        target, segmentBytes, m_protocolId, stepFlowId, 0);

    NS_LOG_DEBUG("GPU " << gpu << " sending step " << step
                 << " to GPU " << target
                 << " flowId=" << stepFlowId);
}

void
RingReduceScatter::OnPacketReceived(uint16_t srcRank, Ptr<Packet> packet, FabricHeader header)
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
           && m_gpuReceivedBytesByStep[destRank][currentStep]
               >= StepExpectedBytes(destRank, currentStep))
    {
        m_gpuReceivedBytesByStep[destRank][currentStep] -=
            StepExpectedBytes(destRank, currentStep);
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
RingReduceScatter::OnGpuStepReceiveComplete(uint16_t gpu, uint32_t step)
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
                            &RingReduceScatter::SendGpuStepData,
                            this,
                            gpu,
                            step + 1);
    }
    else
    {
        Simulator::ScheduleNow(&RingReduceScatter::SendGpuStepData, this, gpu, step + 1);
    }
}

void
RingReduceScatter::CheckGlobalCompletion()
{
    if (m_gpuDoneCount >= m_numGpus)
    {
        Complete();
    }
}

CollectiveType
RingReduceScatter::GetCollectiveType() const
{
    return CollectiveType::REDUCESCATTER;
}

bool
RingReduceScatter::IsCompleted() const
{
    return m_state == ReduceScatterState::COMPLETED;
}

void
RingReduceScatter::Complete()
{
    m_state = ReduceScatterState::COMPLETED;
    m_endTimeNs = Simulator::Now().GetNanoSeconds();

    uint64_t durationNs = m_endTimeNs - m_startTimeNs;

    NS_LOG_INFO("Ring ReduceScatter COMPLETE in " << durationNs / 1000.0 << " µs");

    if (m_completionCallback)
    {
        m_completionCallback(durationNs);
    }

}

} // namespace ns3
