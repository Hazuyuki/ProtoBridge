/*
 * SPDX-License-Identifier: GPL-2.0-only
 * sharp-allreduce.cc
 *
 * SHARP AllReduce: tree-based reduction through NVSwitch.
 *
 * Reduce phase: all GPUs send full data to the switch via ALLREDUCE packets.
 * The NVSwitch buffers contributions from all GPUs, then multicasts the
 * reduced result back to all GPUs.
 *
 * Compared to Ring AllReduce (2*(N-1) steps):
 * - SHARP has 2 logical phases (send-to-switch + receive-from-switch)
 * - For small N, ring may be faster due to pipelining overlap
 * - For large N, SHARP wins because ring scales linearly with N
 */

#include "sharp-allreduce.h"

#include "protocol-model.h"

#include "ns3/boolean.h"
#include "ns3/fatal-error.h"
#include "ns3/log.h"
#include "ns3/simulator.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("SharpAllReduce");

NS_OBJECT_ENSURE_REGISTERED(SharpAllReduce);

TypeId
SharpAllReduce::GetTypeId()
{
    static TypeId tid = TypeId("ns3::SharpAllReduce")
                            .SetParent<CollectiveInjector>()
                            .SetGroupName("GpuCluster")
                            .AddConstructor<SharpAllReduce>()
                            .AddAttribute("UseTransactionModel",
                                          "Use the event-driven protocol transaction model.",
                                          BooleanValue(true),
                                          MakeBooleanAccessor(&SharpAllReduce::m_useTransactionModel),
                                          MakeBooleanChecker());
    return tid;
}

SharpAllReduce::SharpAllReduce()
    : m_numGpus(0),
      m_dataSize(0),
      m_flowId(1),
      m_useTransactionModel(true),
      m_state(SharpState::IDLE),
      m_startupDelayNs(0),
      m_reduceDoneCount(0),
      m_broadcastDoneCount(0),
      m_startTimeNs(0),
      m_endTimeNs(0)
{
}

SharpAllReduce::~SharpAllReduce()
{
}

void
SharpAllReduce::DoDispose()
{
    if (m_transactionExecutor)
    {
        m_transactionExecutor->Reset();
        m_transactionExecutor->SetActionCallback({});
        m_transactionExecutor->SetNodeCompletionCallback({});
        m_transactionExecutor->SetCompletionCallback({});
        m_transactionExecutor = nullptr;
    }
    m_transactionGraph.Clear();
    m_endpoints.clear();
    m_completionCallback = nullptr;
    CollectiveInjector::DoDispose();
}

void
SharpAllReduce::Initialize(uint16_t numGpus, uint64_t dataSize,
                            const std::vector<Ptr<FabricEndpoint>>& endpoints)
{
    m_numGpus = numGpus;
    m_dataSize = dataSize;
    m_endpoints = endpoints;

    m_gpuBroadcastReceivedBytes.resize(numGpus, 0);
    m_reduceDoneCount = 0;
    m_broadcastDoneCount = 0;

    m_flowId = 1;

    // SHARP always uses persistent kernel (LL protocol) regardless of data size,
    // because GPUs continuously stream data to the NVSwitch.
    if (m_startupDelayNs == 0)
    {
        Ptr<ProtocolModel> protoModel = m_endpoints[0]->GetProtocolModel();
        // Use protocol ID for small data (1 byte) to get LL startup delay
        uint8_t llProtoId = protoModel->GetProtocolId(1);
        m_startupDelayNs = protoModel->GetStartupDelayNs(llProtoId, m_numGpus);
    }

    SetupReceiveCallbacks();

    NS_LOG_INFO("SharpAllReduce initialized: numGpus=" << numGpus
                << " dataSize=" << dataSize
                << " startupDelay=" << m_startupDelayNs << "ns");
}

void
SharpAllReduce::SetupReceiveCallbacks()
{
    for (auto& ep : m_endpoints)
    {
        if (ep)
        {
            ep->SetReceiveCallback(
                MakeCallback(&SharpAllReduce::OnPacketReceived, this));
        }
    }
}

void
SharpAllReduce::SetCompletionCallback(std::function<void(uint64_t durationNs)> cb)
{
    m_completionCallback = cb;
}

void
SharpAllReduce::SetStartupDelay(Time delay)
{
    m_startupDelayNs = delay.GetNanoSeconds();
}

void
SharpAllReduce::Start()
{
    m_state = SharpState::REDUCE_PHASE;
    m_startTimeNs = Simulator::Now().GetNanoSeconds();

    NS_LOG_INFO("Starting SHARP AllReduce with startup delay " << m_startupDelayNs << " ns");

    if (m_useTransactionModel)
    {
        BuildTransactionGraph();
        m_transactionExecutor->Start();
        return;
    }

    Simulator::Schedule(NanoSeconds(m_startupDelayNs), &SharpAllReduce::TriggerReducePhase, this);
}

void
SharpAllReduce::BuildTransactionGraph()
{
    m_transactionGraph.Clear();
    const auto startup = m_transactionGraph.AddDelay(NanoSeconds(m_startupDelayNs),
                                                     {},
                                                     "collective-startup");
    std::vector<ProtocolTransactionNodeId> resultArrivals;
    resultArrivals.reserve(m_numGpus);
    Ptr<ProtocolModel> protocolModel = m_endpoints[0]->GetProtocolModel();

    for (uint16_t gpu = 0; gpu < m_numGpus; ++gpu)
    {
        ProtocolTransactionRequest request;
        request.kind = ProtocolTransactionKind::COLLECTIVE_OFFLOAD;
        request.packetType = FabricPacketType::ALLREDUCE;
        request.sourceRank = gpu;
        request.destinationRank = 0;
        request.flowId = 0;
        request.virtualChannel = 0;
        request.effectiveBytes = m_dataSize;
        request.responseBytes = m_dataSize;
        request.label = "gpu-" + std::to_string(gpu) + "-offload";
        resultArrivals.push_back(
            protocolModel->AddTransaction(m_transactionGraph, request, {startup}));
    }
    m_transactionGraph.AddCompletion(resultArrivals);

    m_transactionExecutor = CreateObject<ProtocolTransactionExecutor>();
    m_transactionExecutor->SetActionCallback(
        [this](const ProtocolTransactionAction& action) { ExecuteTransactionAction(action); });
    m_transactionExecutor->SetCompletionCallback([this]() { OnTransactionComplete(); });

    std::string error;
    if (!m_transactionExecutor->SetGraph(m_transactionGraph, &error))
    {
        NS_FATAL_ERROR("Invalid SHARP AllReduce transaction graph: " << error);
    }
}

void
SharpAllReduce::ExecuteTransactionAction(const ProtocolTransactionAction& action)
{
    if (action.type != ProtocolTransactionActionType::SEND_COLLECTIVE ||
        action.sourceRank >= m_endpoints.size() || !m_endpoints[action.sourceRank])
    {
        NS_FATAL_ERROR("SHARP AllReduce emitted an invalid transaction action");
    }
    m_endpoints[action.sourceRank]->SendCollectiveBulk(
        action.packetType,
        action.destinationRank,
        action.effectiveBytes,
        action.wireBytes);
}

void
SharpAllReduce::OnTransactionComplete()
{
    m_broadcastDoneCount = m_numGpus;
    Complete();
}

void
SharpAllReduce::TriggerReducePhase()
{
    // In real NVLS AllReduce, each GPU sends its full dataSize buffer to the
    // switch via multimem loads. The switch SHARP engine reduces all N
    // contributions element-wise, producing a single dataSize result, then
    // multicasts the full result back to all GPUs.
    for (uint16_t gpu = 0; gpu < m_numGpus; gpu++)
    {
        if (gpu >= m_endpoints.size() || !m_endpoints[gpu])
        {
            continue;
        }

        // Each GPU sends dataSize — the switch reduces N copies into one result
        m_endpoints[gpu]->SendCollectiveBulk(FabricPacketType::ALLREDUCE, 0, m_dataSize);

        NS_LOG_DEBUG("GPU " << gpu << " sent " << m_dataSize
                     << " bytes to switch for SHARP reduce");
    }
    // the broadcast phase — when each GPU receives the full result back.
}

void
SharpAllReduce::OnPacketReceived(uint16_t srcRank, Ptr<Packet> packet, FabricHeader header)
{
    // We're interested in ALLREDUCE packets coming back from the switch
    // (broadcast phase) and DATA packets that carry the broadcast result
    if (header.GetPacketType() != FabricPacketType::ALLREDUCE &&
        header.GetPacketType() != FabricPacketType::DATA)
    {
        return;
    }

    uint16_t destRank = header.GetDestRank();
    if (destRank >= m_numGpus)
    {
        return;
    }

    // Only count during broadcast phase
    if (m_state != SharpState::BROADCAST_PHASE && m_state != SharpState::REDUCE_PHASE)
    {
        return;
    }

    uint32_t effectiveSize = header.GetEffectiveDataSize();
    if (effectiveSize == 0)
    {
        effectiveSize = packet->GetSize();
    }

    if (m_useTransactionModel)
    {
        ProtocolTransactionEvent event;
        event.type = ProtocolTransactionEventType::PACKET_DELIVERED;
        event.packetType = header.GetPacketType();
        event.sourceRank = header.GetSourceRank();
        event.destinationRank = destRank;
        event.flowId = header.GetFlowId();
        event.bytes = effectiveSize;
        m_transactionExecutor->NotifyEvent(event);
        return;
    }

    m_gpuBroadcastReceivedBytes[destRank] += effectiveSize;

    NS_LOG_DEBUG("SHARP packet: src=" << srcRank << " -> dest=" << destRank
                 << " effectiveSize=" << effectiveSize
                 << " totalReceived=" << m_gpuBroadcastReceivedBytes[destRank]);

    // Check if this GPU has received the full result
    // AllReduce: each GPU receives dataSize (the full reduced result)
    if (m_gpuBroadcastReceivedBytes[destRank] >= m_dataSize)
    {
        OnGpuBroadcastReceiveComplete(destRank);
    }
}

void
SharpAllReduce::OnGpuBroadcastReceiveComplete(uint16_t gpu)
{
    m_broadcastDoneCount++;

    NS_LOG_INFO("GPU " << gpu << " received SHARP broadcast result"
                 << " (done=" << m_broadcastDoneCount << "/" << m_numGpus << ")");

    if (m_broadcastDoneCount >= m_numGpus)
    {
        Complete();
    }
}

CollectiveType
SharpAllReduce::GetCollectiveType() const
{
    return CollectiveType::ALLREDUCE;
}

bool
SharpAllReduce::IsCompleted() const
{
    return m_state == SharpState::COMPLETED;
}

void
SharpAllReduce::Complete()
{
    m_state = SharpState::COMPLETED;
    m_endTimeNs = Simulator::Now().GetNanoSeconds();

    uint64_t durationNs = m_endTimeNs - m_startTimeNs;

    NS_LOG_INFO("SHARP AllReduce COMPLETE in " << durationNs / 1000.0 << " µs");

    if (m_completionCallback)
    {
        m_completionCallback(durationNs);
    }

}

} // namespace ns3
