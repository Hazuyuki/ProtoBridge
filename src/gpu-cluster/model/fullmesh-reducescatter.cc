/*
 * fullmesh-reducescatter.cc
 *
 * Full-Mesh ReduceScatter: each GPU sends its chunk to all N-1 others simultaneously.
 * Single phase (reduce only, no gather back).
 */

#include "fullmesh-reducescatter.h"
#include "ns3/log.h"
#include "ns3/simulator.h"
#include "nccl-protocol.h"
#include "protocol-model.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("FullMeshReduceScatter");

NS_OBJECT_ENSURE_REGISTERED(FullMeshReduceScatter);

TypeId
FullMeshReduceScatter::GetTypeId()
{
    static TypeId tid = TypeId("ns3::FullMeshReduceScatter")
                            .SetParent<CollectiveInjector>()
                            .SetGroupName("GpuCluster")
                            .AddConstructor<FullMeshReduceScatter>();
    return tid;
}

FullMeshReduceScatter::FullMeshReduceScatter()
    : m_numGpus(0), m_dataSize(0), m_chunkSize(0), m_flowId(1),
      m_state(FullMeshRsState::IDLE), m_startupDelayNs(0),
      m_doneCount(0), m_startTimeNs(0), m_endTimeNs(0)
{
}

FullMeshReduceScatter::~FullMeshReduceScatter() {}

void
FullMeshReduceScatter::DoDispose()
{
    m_endpoints.clear();
    m_completionCallback = nullptr;
    CollectiveInjector::DoDispose();
}

void
FullMeshReduceScatter::Initialize(uint16_t numGpus, uint64_t dataSize,
                                  const std::vector<Ptr<FabricEndpoint>>& endpoints)
{
    m_numGpus = numGpus;
    m_dataSize = dataSize;
    m_endpoints = endpoints;

    m_chunkSize = dataSize / numGpus;
    if (m_chunkSize == 0) m_chunkSize = dataSize;

    m_gpuReceivedBytes.resize(numGpus, 0);
    m_gpuSrcDoneCount.resize(numGpus, 0);
    m_doneCount = 0;

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
    NS_LOG_INFO("FullMeshReduceScatter initialized: numGpus=" << numGpus
                << " dataSize=" << dataSize << " chunkSize=" << m_chunkSize);
}

void
FullMeshReduceScatter::SetupReceiveCallbacks()
{
    for (auto& ep : m_endpoints)
    {
        if (ep)
        {
            ep->SetReceiveCallback(MakeCallback(&FullMeshReduceScatter::OnPacketReceived, this));
        }
    }
}

void
FullMeshReduceScatter::SetCompletionCallback(std::function<void(uint64_t durationNs)> cb)
{
    m_completionCallback = cb;
}

void
FullMeshReduceScatter::SetStartupDelay(Time delay)
{
    m_startupDelayNs = delay.GetNanoSeconds();
}

void
FullMeshReduceScatter::Start()
{
    m_state = FullMeshRsState::RUNNING;
    m_startTimeNs = Simulator::Now().GetNanoSeconds();
    Simulator::Schedule(NanoSeconds(m_startupDelayNs), &FullMeshReduceScatter::TriggerSends, this);
}

void
FullMeshReduceScatter::TriggerSends()
{
    for (uint16_t gpu = 0; gpu < m_numGpus; gpu++)
    {
        if (gpu >= m_endpoints.size() || !m_endpoints[gpu]) continue;

        for (uint16_t dest = 0; dest < m_numGpus; dest++)
        {
            if (dest == gpu) continue;

            m_endpoints[gpu]->SendBulkWireTransferSize(
                dest, m_chunkSize, m_protocolId, m_flowId, 0);
        }
    }
    NS_LOG_INFO("ReduceScatter: all GPUs sending chunks to all others");
}

void
FullMeshReduceScatter::OnPacketReceived(uint16_t srcRank, Ptr<Packet> packet, FabricHeader header)
{
    if (header.GetPacketType() != FabricPacketType::DATA) return;

    uint16_t destRank = header.GetDestRank();
    uint32_t effectiveSize = header.GetEffectiveDataSize();
    if (effectiveSize == 0) effectiveSize = packet->GetSize();

    m_gpuReceivedBytes[destRank] += effectiveSize;
    CheckCompletion();
}

void
FullMeshReduceScatter::CheckCompletion()
{
    uint64_t expectedTotal = (m_numGpus - 1) * m_chunkSize;

    for (uint16_t gpu = 0; gpu < m_numGpus; gpu++)
    {
        if (m_gpuSrcDoneCount[gpu] == 0 && m_gpuReceivedBytes[gpu] >= expectedTotal)
        {
            m_gpuSrcDoneCount[gpu] = m_numGpus - 1;
            m_doneCount++;
        }
    }

    if (m_doneCount >= m_numGpus)
    {
        Complete();
    }
}

CollectiveType
FullMeshReduceScatter::GetCollectiveType() const
{
    return CollectiveType::REDUCESCATTER;
}

bool
FullMeshReduceScatter::IsCompleted() const
{
    return m_state == FullMeshRsState::COMPLETED;
}

void
FullMeshReduceScatter::Complete()
{
    m_state = FullMeshRsState::COMPLETED;
    m_endTimeNs = Simulator::Now().GetNanoSeconds();
    uint64_t durationNs = m_endTimeNs - m_startTimeNs;
    NS_LOG_INFO("FullMesh ReduceScatter COMPLETE in " << durationNs / 1000.0 << " µs");
    if (m_completionCallback) m_completionCallback(durationNs);
}

} // namespace ns3
