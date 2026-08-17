/*
 * SPDX-License-Identifier: GPL-2.0-only
 * protocol-model.cc
 */

#include "protocol-model.h"
#include "ns3/log.h"

#include <limits>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("ProtocolModel");

NS_OBJECT_ENSURE_REGISTERED(ProtocolModel);

TypeId
ProtocolModel::GetTypeId()
{
    static TypeId tid = TypeId("ns3::ProtocolModel")
                            .SetParent<Object>()
                            .SetGroupName("GpuCluster");
    return tid;
}

ProtocolModel::ProtocolModel()
{
}

ProtocolModel::~ProtocolModel()
{
}

ProtocolTransactionNodeId
ProtocolModel::AddTransaction(
    ProtocolTransactionGraph& graph,
    const ProtocolTransactionRequest& request,
    const std::vector<ProtocolTransactionNodeId>& dependencies) const
{
    ProtocolTransactionAction action;
    action.packetType = request.packetType;
    action.sourceRank = request.sourceRank;
    action.destinationRank = request.destinationRank;
    action.flowId = request.flowId;
    action.virtualChannel = request.virtualChannel;
    action.effectiveBytes = request.effectiveBytes;
    action.address = request.address;
    action.stageId = request.stageId;
    action.protocolId = request.hasProtocolId ? request.protocolId
                                             : GetProtocolId(request.effectiveBytes);
    action.wireBytes = GetWireSize(request.effectiveBytes, action.protocolId);
    action.chunkBytes = GetChunkSize(action.protocolId);

    ProtocolTransactionEventMatcher matcher;
    matcher.sourceRank = request.sourceRank;
    matcher.flowId = request.flowId;
    matcher.stageId = request.stageId;
    matcher.matchPacketType = true;
    matcher.packetType = request.packetType;

    uint64_t completionBytes = request.effectiveBytes;
    switch (request.kind)
    {
    case ProtocolTransactionKind::DATA_TRANSFER:
        action.type = ProtocolTransactionActionType::SEND_DATA;
        matcher.type = ProtocolTransactionEventType::PACKET_DELIVERED;
        matcher.destinationRank = request.destinationRank;
        break;
    case ProtocolTransactionKind::P2P_TRANSFER:
        action.type = ProtocolTransactionActionType::SEND_P2P;
        matcher.type = ProtocolTransactionEventType::PACKET_DELIVERED;
        matcher.destinationRank = request.destinationRank;
        break;
    case ProtocolTransactionKind::MEMORY_READ:
        action.type = ProtocolTransactionActionType::SEND_MEMORY_READ;
        action.packetType = FabricPacketType::MEMORY_READ;
        matcher.type = ProtocolTransactionEventType::RESPONSE_RECEIVED;
        matcher.sourceRank = request.destinationRank;
        matcher.destinationRank = request.sourceRank;
        matcher.packetType = FabricPacketType::MEMORY_RESP;
        completionBytes = request.responseBytes > 0 ? request.responseBytes
                                                   : request.effectiveBytes;
        break;
    case ProtocolTransactionKind::MEMORY_WRITE:
        action.type = ProtocolTransactionActionType::SEND_MEMORY_WRITE;
        action.packetType = FabricPacketType::MEMORY_WRITE;
        matcher.type = ProtocolTransactionEventType::PACKET_DELIVERED;
        matcher.destinationRank = request.destinationRank;
        matcher.packetType = FabricPacketType::MEMORY_WRITE;
        break;
    case ProtocolTransactionKind::COLLECTIVE_OFFLOAD:
        action.type = ProtocolTransactionActionType::SEND_COLLECTIVE;
        matcher.type = ProtocolTransactionEventType::PACKET_DELIVERED;
        matcher.sourceRank = PROTOCOL_TRANSACTION_ANY_RANK;
        matcher.destinationRank = request.sourceRank;
        matcher.stageId = std::numeric_limits<uint32_t>::max();
        matcher.matchPacketType = false;
        completionBytes = request.responseBytes > 0 ? request.responseBytes
                                                   : request.effectiveBytes;
        break;
    }

    std::string prefix = request.label.empty() ? "transfer" : request.label;
    ProtocolTransactionNodeId actionNode =
        graph.AddAction(action, dependencies, prefix + ".send");
    return graph.AddWait(matcher,
                         completionBytes,
                         0,
                         {actionNode},
                         prefix + ".delivery");
}

void
ProtocolModel::SetForceProtocolId(uint8_t protocolId)
{
    m_forceProtocolId = protocolId;
}

uint8_t
ProtocolModel::GetForceProtocolId() const
{
    return m_forceProtocolId;
}

} // namespace ns3
