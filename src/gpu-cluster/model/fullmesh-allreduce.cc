/*
 * SPDX-License-Identifier: GPL-2.0-only
 * fullmesh-allreduce.cc
 *
 * Full-Mesh AllReduce: each GPU sends to all N-1 others simultaneously.
 * Reduce-Scatter phase: each GPU splits data into N chunks, sends each
 *   chunk to a different GPU (all sends simultaneous).
 * AllGather phase: each GPU sends its reduced chunk to all others.
 */

#include "fullmesh-allreduce.h"
#include "ns3/log.h"
#include "ns3/simulator.h"
#include "nccl-protocol.h"
#include "protocol-model.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("FullMeshAllReduce");

NS_OBJECT_ENSURE_REGISTERED(FullMeshAllReduce);

TypeId
FullMeshAllReduce::GetTypeId()
{
    static TypeId tid = TypeId("ns3::FullMeshAllReduce")
                            .SetParent<CollectiveInjector>()
                            .SetGroupName("GpuCluster")
                            .AddConstructor<FullMeshAllReduce>();
    return tid;
}

FullMeshAllReduce::FullMeshAllReduce()
    : m_numGpus(0),
      m_dataSize(0),
      m_chunkSize(0),
      m_flowId(1),
      m_state(FullMeshArState::IDLE),
      m_startupDelayNs(0),
      m_reduceScatterDoneCount(0),
      m_allGatherDoneCount(0),
      m_startTimeNs(0),
      m_endTimeNs(0)
{
}

FullMeshAllReduce::~FullMeshAllReduce()
{
}

void
FullMeshAllReduce::DoDispose()
{
    m_endpoints.clear();
    m_completionCallback = nullptr;
    CollectiveInjector::DoDispose();
}

void
FullMeshAllReduce::Initialize(uint16_t numGpus, uint64_t dataSize,
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

    m_gpuReceivedBytes.resize(numGpus, 0);
    m_gpuSrcDoneCount.resize(numGpus, 0);
    m_reduceScatterDoneCount = 0;
    m_allGatherDoneCount = 0;

    m_flowId = 1;

    // Protocol-aware startup delay
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

    NS_LOG_INFO("FullMeshAllReduce initialized: numGpus=" << numGpus
                << " dataSize=" << dataSize << " chunkSize=" << m_chunkSize
                << " startupDelay=" << m_startupDelayNs << "ns");
}

void
FullMeshAllReduce::SetupReceiveCallbacks()
{
    for (auto& ep : m_endpoints)
    {
        if (ep)
        {
            ep->SetReceiveCallback(
                MakeCallback(&FullMeshAllReduce::OnPacketReceived, this));
        }
    }
}

void
FullMeshAllReduce::SetCompletionCallback(std::function<void(uint64_t durationNs)> cb)
{
    m_completionCallback = cb;
}

void
FullMeshAllReduce::SetStartupDelay(Time delay)
{
    m_startupDelayNs = delay.GetNanoSeconds();
}

void
FullMeshAllReduce::Start()
{
    m_state = FullMeshArState::REDUCE_SCATTER;
    m_startTimeNs = Simulator::Now().GetNanoSeconds();

    NS_LOG_INFO("Starting FullMesh AllReduce with startup delay " << m_startupDelayNs << " ns");

    Simulator::Schedule(NanoSeconds(m_startupDelayNs), &FullMeshAllReduce::TriggerReduceScatterPhase, this);
}

void
FullMeshAllReduce::TriggerReduceScatterPhase()
{
    // Each GPU sends its chunk to all N-1 other GPUs simultaneously
    for (uint16_t gpu = 0; gpu < m_numGpus; gpu++)
    {
        if (gpu >= m_endpoints.size() || !m_endpoints[gpu])
        {
            continue;
        }

        for (uint16_t dest = 0; dest < m_numGpus; dest++)
        {
            if (dest == gpu)
            {
                continue;  // Skip self
            }

            // GPU gpu sends chunk gpu to dest GPU (each GPU gets chunk from each source)
            m_endpoints[gpu]->SendBulkWireTransferSize(
                dest, m_chunkSize, m_protocolId, m_flowId, 0);

            NS_LOG_DEBUG("RS: GPU " << gpu << " sending chunk " << gpu
                         << " to GPU " << dest << " size=" << m_chunkSize);
        }
    }

    NS_LOG_INFO("Reduce-Scatter phase: all GPUs sending chunks to all others");
}

void
FullMeshAllReduce::TriggerAllGatherPhase()
{
    m_state = FullMeshArState::ALL_GATHER;
    // Reset tracking for all-gather phase
    for (uint16_t i = 0; i < m_numGpus; i++)
    {
        m_gpuReceivedBytes[i] = 0;
        m_gpuSrcDoneCount[i] = 0;
    }
    m_allGatherDoneCount = 0;

    // Each GPU sends its reduced chunk to all N-1 others simultaneously
    for (uint16_t gpu = 0; gpu < m_numGpus; gpu++)
    {
        if (gpu >= m_endpoints.size() || !m_endpoints[gpu])
        {
            continue;
        }

        for (uint16_t dest = 0; dest < m_numGpus; dest++)
        {
            if (dest == gpu)
            {
                continue;
            }

            m_endpoints[gpu]->SendBulkWireTransferSize(
                dest, m_chunkSize, m_protocolId, m_flowId + 100, 0);

            NS_LOG_DEBUG("AG: GPU " << gpu << " sending reduced chunk to GPU " << dest);
        }
    }

    NS_LOG_INFO("AllGather phase: all GPUs broadcasting reduced chunks");
}

void
FullMeshAllReduce::OnPacketReceived(uint16_t srcRank, Ptr<Packet> packet, FabricHeader header)
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

    m_gpuReceivedBytes[destRank] += effectiveSize;

    NS_LOG_DEBUG("Packet: src=" << srcRank << " -> dest=" << destRank
                 << " effectiveSize=" << effectiveSize
                 << " totalReceived=" << m_gpuReceivedBytes[destRank]
                 << " state=" << (m_state == FullMeshArState::REDUCE_SCATTER ? "RS" : "AG"));

    if (m_state == FullMeshArState::REDUCE_SCATTER)
    {
        CheckReduceScatterComplete();
    }
    else if (m_state == FullMeshArState::ALL_GATHER)
    {
        CheckAllGatherComplete();
    }
}

void
FullMeshAllReduce::CheckReduceScatterComplete()
{
    // Each GPU should receive chunks from N-1 sources, total = (N-1) * chunkSize
    uint64_t expectedTotal = (m_numGpus - 1) * m_chunkSize;

    for (uint16_t gpu = 0; gpu < m_numGpus; gpu++)
    {
        if (m_gpuSrcDoneCount[gpu] == 0 && m_gpuReceivedBytes[gpu] >= expectedTotal)
        {
            m_gpuSrcDoneCount[gpu] = m_numGpus - 1;
            m_reduceScatterDoneCount++;
            NS_LOG_INFO("RS: GPU " << gpu << " received all chunks (done=" << m_reduceScatterDoneCount << "/" << m_numGpus << ")");
        }
    }

    if (m_reduceScatterDoneCount >= m_numGpus)
    {
        NS_LOG_INFO("Reduce-Scatter phase complete, starting AllGather");
        TriggerAllGatherPhase();
    }
}

void
FullMeshAllReduce::CheckAllGatherComplete()
{
    // Each GPU should receive chunks from N-1 sources, total = (N-1) * chunkSize
    uint64_t expectedTotal = (m_numGpus - 1) * m_chunkSize;

    for (uint16_t gpu = 0; gpu < m_numGpus; gpu++)
    {
        if (m_gpuSrcDoneCount[gpu] == 0 && m_gpuReceivedBytes[gpu] >= expectedTotal)
        {
            m_gpuSrcDoneCount[gpu] = m_numGpus - 1;
            m_allGatherDoneCount++;
            NS_LOG_INFO("AG: GPU " << gpu << " received all chunks (done=" << m_allGatherDoneCount << "/" << m_numGpus << ")");
        }
    }

    if (m_allGatherDoneCount >= m_numGpus)
    {
        Complete();
    }
}

CollectiveType
FullMeshAllReduce::GetCollectiveType() const
{
    return CollectiveType::ALLREDUCE;
}

bool
FullMeshAllReduce::IsCompleted() const
{
    return m_state == FullMeshArState::COMPLETED;
}

void
FullMeshAllReduce::Complete()
{
    m_state = FullMeshArState::COMPLETED;
    m_endTimeNs = Simulator::Now().GetNanoSeconds();

    uint64_t durationNs = m_endTimeNs - m_startTimeNs;

    NS_LOG_INFO("FullMesh AllReduce COMPLETE in " << durationNs / 1000.0 << " µs");

    if (m_completionCallback)
    {
        m_completionCallback(durationNs);
    }

}

} // namespace ns3
