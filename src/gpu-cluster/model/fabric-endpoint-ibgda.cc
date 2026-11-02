/* SPDX-License-Identifier: GPL-2.0-only */
#include "fabric-endpoint.h"
#include "ns3/abort.h"
#include "ns3/ppp-header.h"
#include "ns3/simulator.h"

namespace ns3
{
void FabricEndpoint::SetIbgdaSubmissionModel(Ptr<IbgdaSubmissionModel> model)
{
    NS_ABORT_MSG_IF(m_ibgda && m_ibgda->GetOutstanding(),
                    "Cannot change IBGDA mode while work is outstanding");
    DisposeIbgda();
    m_ibgda=model;
    if (m_ibgda) for (auto device : m_devices) ConnectIbgdaDevice(device);
}
Ptr<IbgdaSubmissionModel> FabricEndpoint::GetIbgdaSubmissionModel() const { return m_ibgda; }
void FabricEndpoint::ConnectIbgdaDevice(Ptr<NetDevice> device)
{
    if (m_ibgda && device->TraceConnectWithoutContext("PhyTxBegin",
            MakeCallback(&FabricEndpoint::OnIbgdaPhyTxBegin,this)))
        m_ibgdaTraceDevices.push_back(device);
}
void FabricEndpoint::DisposeIbgda()
{
    for (auto device : m_ibgdaTraceDevices)
        device->TraceDisconnectWithoutContext("PhyTxBegin",
            MakeCallback(&FabricEndpoint::OnIbgdaPhyTxBegin,this));
    m_ibgdaTraceDevices.clear(); m_ibgdaPackets.clear();
    if (m_ibgda) m_ibgda->Dispose();
    m_ibgda=nullptr;
}
uint64_t FabricEndpoint::SubmitIbgdaWrite(uint16_t destRank, uint64_t bytes,
    uint8_t protocolId, uint16_t flowId, uint8_t vcId, uint32_t producer,
    IbgdaSubmissionModel::Completion complete)
{
    NS_ABORT_MSG_IF(!m_ibgda || IsLocalRank(destRank),
                    "SubmitIbgdaWrite requires IBGDA mode and a remote destination");
    return m_ibgda->Submit(destRank,bytes,
        [this,destRank,bytes,protocolId,flowId,vcId](uint64_t id) {
            uint64_t previous=m_activeIbgdaWork;
            m_activeIbgdaWork=id;
            SendBulkWireTransferSizeImpl(destRank,bytes,protocolId,flowId,vcId);
            m_activeIbgdaWork=previous;
        },producer,std::move(complete));
}
void FabricEndpoint::SendBulkWireTransferSize(uint16_t destRank, uint64_t bytes,
    uint8_t protocolId, uint16_t flowId, uint8_t vcId)
{
    if (m_ibgda && !IsLocalRank(destRank) && bytes > 0)
    {
        SubmitIbgdaWrite(destRank,bytes,protocolId,flowId,vcId);
        return;
    }
    SendBulkWireTransferSizeImpl(destRank,bytes,protocolId,flowId,vcId);
}
bool FabricEndpoint::PrepareIbgdaPacket(Ptr<Packet> packet, uint32_t deviceIndex,
                                        const Address& dest)
{
    if (!m_ibgda) return false;
    FabricHeader header;
    packet->PeekHeader(header);
    if (header.GetSourceRank() != m_rank || header.GetPacketType() != FabricPacketType::DATA)
        return false;
    IbgdaPacketTag tag;
    bool tagged=packet->PeekPacketTag(tag);
    if (!tagged && m_activeIbgdaWork != 0)
    {
        tag.workId=m_activeIbgdaWork;
        packet->AddPacketTag(tag);
        tagged=true;
    }
    if (!tagged || tag.sourceReady) return false;
    tag.qp=m_ibgda->GetQpForWork(tag.workId);
    m_ibgdaPackets.emplace(IbgdaPacketKey{header.GetDestRank(),header.GetVirtualChannel(),
        header.GetFlowId(),header.GetSequenceNumber()},
        std::make_pair(tag.workId,header.GetEffectiveDataSize()));
    tag.sourceReady=true;
    packet->ReplacePacketTag(tag);
    Time delay=m_ibgda->ReserveSourceRead(header.GetEffectiveDataSize());
    if (delay.IsZero()) return false;
    m_deferredEgressEvents.push_back(Simulator::Schedule(delay,
        [this,packet,deviceIndex,dest]{SendPacketOnDevice(packet,deviceIndex,dest);}));
    return true;
}
bool FabricEndpoint::DeferIbgdaReceive(Ptr<Packet> packet, const FabricHeader& header)
{
    IbgdaPacketTag tag;
    if (!packet->PeekPacketTag(tag) || tag.destinationReady) return false;
    NS_ABORT_MSG_IF(!m_ibgda, "IBGDA data requires a configured receiving endpoint");
    tag.destinationReady=true;
    packet->ReplacePacketTag(tag);
    Time delay=m_ibgda->ReserveDestinationWrite(header.GetEffectiveDataSize());
    if (delay.IsZero()) return false;
    m_deferredEgressEvents.push_back(Simulator::Schedule(delay,
        [this,packet,header]{OnPacketDelivered(packet,header.GetSequenceNumber(),header);}));
    return true;
}
void FabricEndpoint::OnIbgdaPhyTxBegin(Ptr<const Packet> packet)
{
    if (!m_ibgda) return;
    IbgdaPacketTag tag;
    if (!packet->PeekPacketTag(tag)) return;
    auto copy=packet->Copy();
    PppHeader ppp;
    FabricHeader header;
    if (copy->GetSize() < ppp.GetSerializedSize()+header.GetSerializedSize()) return;
    copy->RemoveHeader(ppp);
    copy->PeekHeader(header);
    if (header.GetSourceRank() == m_rank) m_ibgda->NotifyFirstWire(tag.workId);
}
uint16_t FabricEndpoint::GetIbgdaRateFlow(Ptr<const Packet> packet, const FabricHeader& header) const
{
    IbgdaPacketTag tag;
    return packet->PeekPacketTag(tag) ? static_cast<uint16_t>(tag.qp) : header.GetFlowId();
}
} // namespace ns3
