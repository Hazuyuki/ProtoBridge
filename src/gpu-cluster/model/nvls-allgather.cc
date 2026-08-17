/*
 * SPDX-License-Identifier: GPL-2.0-only
 * nvls-allgather.cc
 *
 * NVLS AllGather: each GPU sends dataSize/N to NVSwitch,
 * switch concatenates and multicasts full dataSize to all GPUs.
 */

#include "nvls-allgather.h"
#include "ns3/log.h"
#include "ns3/simulator.h"
#include "protocol-model.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("NvlsAllGather");

NS_OBJECT_ENSURE_REGISTERED(NvlsAllGather);

TypeId
NvlsAllGather::GetTypeId()
{
    static TypeId tid = TypeId("ns3::NvlsAllGather")
                            .SetParent<CollectiveInjector>()
                            .SetGroupName("GpuCluster")
                            .AddConstructor<NvlsAllGather>();
    return tid;
}

NvlsAllGather::NvlsAllGather()
    : m_numGpus(0),
      m_dataSize(0),
      m_chunkSize(0),
      m_flowId(1),
      m_state(NvlsAllGatherState::IDLE),
      m_startupDelayNs(0),
      m_broadcastDoneCount(0),
      m_startTimeNs(0),
      m_endTimeNs(0)
{
}

NvlsAllGather::~NvlsAllGather()
{
}

void
NvlsAllGather::DoDispose()
{
    m_endpoints.clear();
    m_completionCallback = nullptr;
    CollectiveInjector::DoDispose();
}

void
NvlsAllGather::Initialize(uint16_t numGpus, uint64_t dataSize,
                            const std::vector<Ptr<FabricEndpoint>>& endpoints)
{
    m_numGpus = numGpus;
    m_dataSize = dataSize;
    m_endpoints = endpoints;

    m_chunkSize = dataSize / numGpus;
    if (m_chunkSize == 0)
    {
        m_chunkSize = 1; // minimum chunk size
    }

    m_gpuBroadcastReceivedBytes.resize(numGpus, 0);
    m_broadcastDoneCount = 0;

    m_flowId = 1;

    if (m_startupDelayNs == 0)
    {
        Ptr<ProtocolModel> protoModel = m_endpoints[0]->GetProtocolModel();
        uint8_t llProtoId = protoModel->GetProtocolId(1);
        m_startupDelayNs = protoModel->GetStartupDelayNs(llProtoId, m_numGpus);
    }

    SetupReceiveCallbacks();

    NS_LOG_INFO("NvlsAllGather initialized: numGpus=" << numGpus
                << " dataSize=" << dataSize
                << " chunkSize=" << m_chunkSize
                << " startupDelay=" << m_startupDelayNs << "ns");
}

void
NvlsAllGather::SetupReceiveCallbacks()
{
    for (auto& ep : m_endpoints)
    {
        if (ep)
        {
            ep->SetReceiveCallback(
                MakeCallback(&NvlsAllGather::OnPacketReceived, this));
        }
    }
}

void
NvlsAllGather::SetCompletionCallback(std::function<void(uint64_t durationNs)> cb)
{
    m_completionCallback = cb;
}

void
NvlsAllGather::SetStartupDelay(Time delay)
{
    m_startupDelayNs = delay.GetNanoSeconds();
}

void
NvlsAllGather::Start()
{
    m_state = NvlsAllGatherState::SEND_PHASE;
    m_startTimeNs = Simulator::Now().GetNanoSeconds();

    NS_LOG_INFO("Starting NVLS AllGather with startup delay " << m_startupDelayNs << " ns");

    Simulator::Schedule(NanoSeconds(m_startupDelayNs), &NvlsAllGather::TriggerSendPhase, this);
}

void
NvlsAllGather::TriggerSendPhase()
{
    m_state = NvlsAllGatherState::BROADCAST_PHASE;

    // Each GPU sends only its chunk (dataSize/N) to the switch
    for (uint16_t gpu = 0; gpu < m_numGpus; gpu++)
    {
        if (gpu >= m_endpoints.size() || !m_endpoints[gpu])
        {
            continue;
        }

        std::vector<uint8_t> data(m_chunkSize, 0xAB);
        m_endpoints[gpu]->SendCollective(FabricPacketType::ALLGATHER, 0,
                                          data.data(), m_chunkSize);

        NS_LOG_DEBUG("GPU " << gpu << " sent " << m_chunkSize
                     << " bytes to switch for NVLS AllGather");
    }
}

void
NvlsAllGather::OnPacketReceived(uint16_t srcRank, Ptr<Packet> packet, FabricHeader header)
{
    if (header.GetPacketType() != FabricPacketType::ALLGATHER &&
        header.GetPacketType() != FabricPacketType::DATA)
    {
        return;
    }

    uint16_t destRank = header.GetDestRank();
    if (destRank >= m_numGpus)
    {
        return;
    }

    if (m_state != NvlsAllGatherState::BROADCAST_PHASE)
    {
        return;
    }

    uint32_t effectiveSize = header.GetEffectiveDataSize();
    if (effectiveSize == 0)
    {
        effectiveSize = packet->GetSize();
    }

    m_gpuBroadcastReceivedBytes[destRank] += effectiveSize;

    NS_LOG_DEBUG("NVLS AllGather packet: src=" << srcRank << " -> dest=" << destRank
                 << " effectiveSize=" << effectiveSize
                 << " totalReceived=" << m_gpuBroadcastReceivedBytes[destRank]);

    // Each GPU must receive the full dataSize (concatenation of all chunks)
    if (m_gpuBroadcastReceivedBytes[destRank] >= m_dataSize)
    {
        OnGpuBroadcastReceiveComplete(destRank);
    }
}

void
NvlsAllGather::OnGpuBroadcastReceiveComplete(uint16_t gpu)
{
    LogStepComplete(gpu, 0, m_startTimeNs, Simulator::Now().GetNanoSeconds());

    m_broadcastDoneCount++;

    NS_LOG_INFO("GPU " << gpu << " received NVLS AllGather broadcast"
                 << " (done=" << m_broadcastDoneCount << "/" << m_numGpus << ")");

    if (m_broadcastDoneCount >= m_numGpus)
    {
        Complete();
    }
}

CollectiveType
NvlsAllGather::GetCollectiveType() const
{
    return CollectiveType::ALLGATHER;
}

bool
NvlsAllGather::IsCompleted() const
{
    return m_state == NvlsAllGatherState::COMPLETED;
}

void
NvlsAllGather::Complete()
{
    m_state = NvlsAllGatherState::COMPLETED;
    m_endTimeNs = Simulator::Now().GetNanoSeconds();

    uint64_t durationNs = m_endTimeNs - m_startTimeNs;

    NS_LOG_INFO("NVLS AllGather COMPLETE in " << durationNs / 1000.0 << " µs");

    if (m_completionCallback)
    {
        m_completionCallback(durationNs);
    }

}

} // namespace ns3