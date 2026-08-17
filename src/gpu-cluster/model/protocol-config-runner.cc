/*
 * SPDX-License-Identifier: GPL-2.0-only
 * protocol-config-runner.cc
 *
 * Generic graph runner. Binds a config-compiled ProtocolTransactionGraph to
 * FabricEndpoints + a ProtocolTransactionExecutor, and drives the OTP<->PEX
 * handoff: ACTION -> SendBulkWireTransferSize, delivered packet -> NotifyEvent.
 */

#include "protocol-config-runner.h"
#include "protocol-model.h"
#include "fabric-header.h"
#include "ns3/log.h"
#include "ns3/nstime.h"
#include "ns3/simulator.h"
#include "ns3/callback.h"

#include <limits>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("ProtocolConfigRunner");

ProtocolConfigRunner::ProtocolConfigRunner() = default;
ProtocolConfigRunner::~ProtocolConfigRunner() = default;

void
ProtocolConfigRunner::ApplyBundle(const std::vector<Ptr<FabricEndpoint>>& endpoints,
                                  const ProtocolBundle& bundle,
                                  uint32_t bulkChunkSize)
{
    const uint8_t nvc = bundle.credits ? bundle.credits->GetNumVcs() : 1;
    for (const auto& ep : endpoints)
    {
        if (!ep) continue;
        if (bundle.protocol) ep->SetProtocolModel(bundle.protocol);
        if (bundle.payloadBuilder) ep->SetProtocolPayloadBuilder(bundle.payloadBuilder);
        if (bundle.fec) ep->SetFecModel(bundle.fec);
        ep->SetNumVirtualChannels(nvc ? nvc : 1);
        if (bundle.credits)
        {
            for (uint8_t v = 0; v < nvc; ++v)
            {
                ep->SetVcCredits(v, bundle.credits->GetAvailableCredits(v));
            }
        }
        else
        {
            ep->SetVcCredits(0, 64);
        }
        ep->SetFlowControlPolicy(bundle.flowControl);
        ep->SetLlrEnabled(bundle.llrEnabled);
        ep->SetBulkChunkSize(bulkChunkSize);
    }
}

bool
ProtocolConfigRunner::Initialize(const std::vector<Ptr<FabricEndpoint>>& endpoints,
                                 const ProtocolBundle& bundle,
                                 const ProtocolConfig& config,
                                 uint16_t numGpus,
                                 uint64_t dataSize,
                                 std::string* error)
{
    m_endpoints = endpoints;
    m_executor = CreateObject<ProtocolTransactionExecutor>();

    ProtocolTransactionGraph graph;
    ProtocolTransactionNodeId completeNode =
        config.Compile(graph, bundle.protocol, numGpus, dataSize, m_baseFlowId, error);
    if (completeNode == PROTOCOL_TRANSACTION_INVALID_NODE)
    {
        return false;
    }

    m_executor->SetActionCallback(
        [this](const ProtocolTransactionAction& a) { OnAction(a); });
    m_executor->SetCompletionCallback([this]() { OnComplete(); });

    if (!m_executor->SetGraph(graph, error))
    {
        return false;
    }

    for (const auto& ep : m_endpoints)
    {
        if (ep)
        {
            ep->SetReceiveCallback(
                MakeCallback(&ProtocolConfigRunner::OnPacketReceived, this));
        }
    }
    NS_LOG_INFO("ProtocolConfigRunner initialized: "
                << graph.GetNodeCount() << " graph nodes, "
                << m_endpoints.size() << " endpoints");
    return true;
}

void
ProtocolConfigRunner::SetCompletionCallback(std::function<void(uint64_t durationNs)> cb)
{
    m_completionCallback = std::move(cb);
}

void
ProtocolConfigRunner::Start()
{
    m_startNs = Simulator::Now().GetNanoSeconds();
    m_executor->Start();
}

bool
ProtocolConfigRunner::IsComplete() const
{
    return m_complete;
}

void
ProtocolConfigRunner::OnAction(const ProtocolTransactionAction& action)
{
    // The config compiler only emits SEND_DATA / SEND_P2P actions (DATA packets
    // via SendBulkWireTransferSize). Memory/collective offload actions would
    // need a different send path and are not supported by the generic runner.
    if (action.type != ProtocolTransactionActionType::SEND_DATA &&
        action.type != ProtocolTransactionActionType::SEND_P2P)
    {
        NS_FATAL_ERROR("ProtocolConfigRunner: unsupported action type "
                       << static_cast<int>(action.type));
    }
    if (action.sourceRank >= m_endpoints.size() || !m_endpoints[action.sourceRank])
    {
        NS_FATAL_ERROR("ProtocolConfigRunner: action has invalid source rank "
                       << action.sourceRank);
    }
    m_endpoints[action.sourceRank]->SendBulkWireTransferSize(
        action.destinationRank,
        action.effectiveBytes,
        action.protocolId,
        action.flowId,
        action.virtualChannel);
}

void
ProtocolConfigRunner::OnPacketReceived(uint16_t srcRank, Ptr<Packet> packet, FabricHeader header)
{
    (void)srcRank;
    (void)packet;
    if (!m_executor) return;

    // Permanently lost data cannot complete a delivery -- the collective stalls
    // rather than silently corrupting (mirrors the collective injectors).
    if (header.GetPacketType() == FabricPacketType::PERMANENT_LOSS)
    {
        return;
    }

    uint32_t effectiveSize = header.GetEffectiveDataSize();
    if (effectiveSize == 0)
    {
        effectiveSize = packet->GetSize();
    }

    ProtocolTransactionEvent event;
    event.type = ProtocolTransactionEventType::PACKET_DELIVERED;
    event.packetType = header.GetPacketType();
    event.sourceRank = header.GetSourceRank();
    event.destinationRank = header.GetDestRank();
    event.flowId = header.GetFlowId();
    event.bytes = effectiveSize;
    // stageId is derived from the flow id (flowId = baseFlowId + stageId at
    // compile time), matching the RingAllReduce receive path.
    event.stageId = (event.flowId >= m_baseFlowId)
                        ? static_cast<uint32_t>(event.flowId - m_baseFlowId)
                        : std::numeric_limits<uint32_t>::max();
    m_executor->NotifyEvent(event);
}

void
ProtocolConfigRunner::OnComplete()
{
    m_complete = true;
    if (m_completionCallback)
    {
        uint64_t durationNs = Simulator::Now().GetNanoSeconds() - m_startNs;
        m_completionCallback(durationNs);
    }
}

} // namespace ns3
