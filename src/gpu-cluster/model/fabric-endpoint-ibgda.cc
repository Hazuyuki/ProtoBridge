/* SPDX-License-Identifier: GPL-2.0-only */
#include "fabric-endpoint.h"
#include "ns3/time-bounds.h"

#include "ns3/abort.h"
#include "ns3/ppp-header.h"
#include "ns3/simulator.h"

namespace ns3
{
void
FabricEndpoint::SetIbgdaSubmissionModel(Ptr<IbgdaSubmissionModel> model)
{
    NS_ABORT_MSG_IF(m_ibgda && (m_ibgda->GetOutstanding() || !m_rdmaReceiveWork.empty() ||
                                !m_rdmaPendingAck.empty() || !m_rdmaTxQueue.empty() ||
                                (m_rdmaOperations && m_rdmaOperations->HasPending())),
                    "Cannot change RDMA mode while work is outstanding");
    DisposeIbgda();
    m_ibgda = model;
    if (m_ibgda)
    {
        m_rdmaOperations = CreateObject<RdmaOperationEngine>();
        m_rdmaOperations->Configure(
            m_rank,
            m_ibgda,
            [this](uint16_t peer,
                   Ptr<Packet> packet,
                   const RdmaOperationTag& tag,
                   uint32_t qp,
                   uint16_t flow,
                   uint8_t vc,
                   uint32_t overhead) {
                QueueRdmaOperationPacket(peer, packet, tag, qp, flow, vc, overhead);
            },
            [this](const FabricHeader& h) { SendLlrAckPacket(h, false); },
            [this](const FabricHeader& h, bool held) {
                IbgdaPacketKey key{h.GetSourceRank(),
                                   h.GetVirtualChannel(),
                                   h.GetFlowId(),
                                   h.GetSequenceNumber()};
                if (held)
                {
                    m_rdmaPendingAck.insert(key);
                }
                else
                {
                    m_rdmaPendingAck.erase(key);
                }
            });
        m_rdmaOperations->SetPayloadLimit([this](uint16_t peer) {
            double efficiency = GetProtocolModelForDest(peer)->GetEfficiency(1);
            uint32_t mtu = m_ibgda->GetConfig().mtuBytes;
            uint64_t cap = static_cast<uint64_t>(mtu * efficiency);
            return std::max(
                1u,
                std::min(mtu > 14 ? mtu - 14 : 1u, cap > 4 ? static_cast<uint32_t>(cap - 4) : 1u));
        });
        for (auto device : m_devices)
        {
            ConnectIbgdaDevice(device);
        }
    }
}

Ptr<IbgdaSubmissionModel>
FabricEndpoint::GetIbgdaSubmissionModel() const
{
    return m_ibgda;
}

void
FabricEndpoint::ConnectIbgdaDevice(Ptr<NetDevice> device)
{
    if (m_ibgda &&
        device->TraceConnectWithoutContext("PhyTxBegin",
                                           MakeCallback(&FabricEndpoint::OnIbgdaPhyTxBegin, this)))
    {
        m_ibgdaTraceDevices.push_back(device);
    }
}

void
FabricEndpoint::DisposeIbgda()
{
    if (m_rdmaOperations)
    {
        m_rdmaOperations->Dispose();
    }
    m_rdmaOperations = nullptr;
    m_rdmaTxQueue.clear();
    for (auto device : m_ibgdaTraceDevices)
    {
        device->TraceDisconnectWithoutContext(
            "PhyTxBegin",
            MakeCallback(&FabricEndpoint::OnIbgdaPhyTxBegin, this));
    }
    m_ibgdaTraceDevices.clear();
    m_ibgdaPackets.clear();
    m_rdmaReceiveWork.clear();
    m_rdmaPendingAck.clear();
    if (m_ibgda)
    {
        m_ibgda->Dispose();
    }
    m_ibgda = nullptr;
}

uint64_t
FabricEndpoint::SubmitRdmaWrite(uint16_t destRank,
                                uint64_t bytes,
                                uint8_t protocolId,
                                uint16_t flowId,
                                uint8_t vcId,
                                uint32_t producer,
                                RdmaSubmissionModel::Completion complete)
{
    NS_ABORT_MSG_IF(!m_ibgda || IsLocalRank(destRank),
                    "SubmitRdmaWrite requires detailed RDMA mode and a remote destination");
    return m_ibgda->Submit(
        destRank,
        bytes,
        [this, destRank, protocolId, flowId, vcId](uint64_t id) {
            uint64_t previous = m_activeIbgdaWork;
            m_activeIbgdaWork = id;
            SendBulkWireTransferSizeImpl(destRank,
                                         m_ibgda->GetBytesForWork(id),
                                         protocolId,
                                         flowId,
                                         vcId);
            m_activeIbgdaWork = previous;
        },
        producer,
        std::move(complete));
}

uint64_t
FabricEndpoint::SubmitIbgdaWrite(uint16_t destRank,
                                 uint64_t bytes,
                                 uint8_t protocolId,
                                 uint16_t flowId,
                                 uint8_t vcId,
                                 uint32_t producer,
                                 IbgdaSubmissionModel::Completion complete)
{
    return SubmitRdmaWrite(destRank,
                           bytes,
                           protocolId,
                           flowId,
                           vcId,
                           producer,
                           std::move(complete));
}

void
FabricEndpoint::SendBulkWireTransferSize(uint16_t destRank,
                                         uint64_t bytes,
                                         uint8_t protocolId,
                                         uint16_t flowId,
                                         uint8_t vcId)
{
    if (m_ibgda && !IsLocalRank(destRank) && bytes > 0)
    {
        SubmitRdmaWrite(destRank, bytes, protocolId, flowId, vcId);
        return;
    }
    SendBulkWireTransferSizeImpl(destRank, bytes, protocolId, flowId, vcId);
}

bool
FabricEndpoint::PrepareIbgdaPacket(Ptr<Packet> packet, uint32_t deviceIndex, const Address& dest)
{
    if (!m_ibgda)
    {
        return false;
    }
    FabricHeader header;
    packet->PeekHeader(header);
    if (header.GetSourceRank() != m_rank || header.GetPacketType() != FabricPacketType::DATA)
    {
        return false;
    }
    IbgdaPacketTag tag;
    bool tagged = packet->PeekPacketTag(tag);
    if (!tagged && m_activeIbgdaWork != 0)
    {
        tag.workId = m_activeIbgdaWork;
        packet->AddPacketTag(tag);
        tagged = true;
    }
    if (!tagged || tag.sourceReady)
    {
        return false;
    }
    tag.qp = m_ibgda->GetQpForWork(tag.workId);
    tag.workBytes = m_ibgda->GetBytesForWork(tag.workId);
    m_ibgdaPackets.emplace(IbgdaPacketKey{header.GetDestRank(),
                                          header.GetVirtualChannel(),
                                          header.GetFlowId(),
                                          header.GetSequenceNumber()},
                           std::make_pair(tag.workId, header.GetEffectiveDataSize()));
    tag.sourceReady = true;
    packet->ReplacePacketTag(tag);
    if (m_ibgda->GetConfig().pcieModel == "tlp")
    {
        tag.dmaOffset = m_ibgda->TakeSourceDmaOffset(tag.workId, header.GetEffectiveDataSize());
        packet->ReplacePacketTag(tag);
        m_ibgda->SourceRead(
            header.GetEffectiveDataSize(),
            [this, packet, deviceIndex, dest] { SendPacketOnDevice(packet, deviceIndex, dest); },
            tag.dmaOffset + (m_ibgda->GetConfig().dataPath == "staged"
                                 ? 0
                                 : m_ibgda->GetConfig().localAddress));
        return true;
    }
    Time delay = m_ibgda->ReserveSourceRead(header.GetEffectiveDataSize());
    if (delay.IsZero())
    {
        return false;
    }
    m_deferredEgressEvents.push_back(TimeBounds::Schedule(delay, [this, packet, deviceIndex, dest] {
        SendPacketOnDevice(packet, deviceIndex, dest);
    }));
    return true;
}

bool
FabricEndpoint::DeferIbgdaReceive(Ptr<Packet> packet, const FabricHeader& header)
{
    IbgdaPacketTag tag;
    if (!packet->PeekPacketTag(tag) || tag.destinationReady)
    {
        return false;
    }
    NS_ABORT_MSG_IF(!m_ibgda, "RDMA data requires a configured receiving endpoint");
    const IbgdaPacketKey ackKey{header.GetSourceRank(),
                                header.GetVirtualChannel(),
                                header.GetFlowId(),
                                header.GetSequenceNumber()};
    m_rdmaPendingAck.insert(ackKey);
    tag.destinationReady = true;
    if (m_ibgda->GetConfig().dataPath == "staged")
    {
        tag.ackIssued = true;
        packet->ReplacePacketTag(tag);
        RdmaReceiveKey key{header.GetSourceRank(), tag.workId};
        auto found = m_rdmaReceiveWork.find(key);
        if (found == m_rdmaReceiveWork.end())
        {
            auto work = std::make_shared<RdmaReceiveWork>();
            work->bytes = tag.workBytes;
            work->waiting.push_back({packet, header});
            m_rdmaReceiveWork[key] = work;
            m_ibgda->AcquireReceiveStaging(work->bytes, [this, key, work] {
                work->allocated = true;
                auto waiting = std::move(work->waiting);
                for (auto& piece : waiting)
                {
                    StartRdmaHostWrite(key, work, piece.packet, piece.header);
                }
            });
        }
        else if (found->second->allocated)
        {
            StartRdmaHostWrite(key, found->second, packet, header);
        }
        else
        {
            found->second->waiting.push_back({packet, header});
        }
        return true;
    }
    packet->ReplacePacketTag(tag);
    if (m_ibgda->GetConfig().pcieModel == "tlp")
    {
        m_ibgda->DestinationWrite(
            header.GetEffectiveDataSize(),
            [this, packet, header, ackKey] {
                m_rdmaPendingAck.erase(ackKey);
                m_ibgda->NotifyDataReady();
                OnPacketDelivered(packet, header.GetSequenceNumber(), header);
            },
            m_ibgda->GetConfig().remoteAddress + tag.dmaOffset);
        return true;
    }
    Time delay = m_ibgda->ReserveDestinationWrite(header.GetEffectiveDataSize());
    if (delay.IsZero())
    {
        m_rdmaPendingAck.erase(ackKey);
        m_ibgda->NotifyDataReady();
        return false;
    }
    m_deferredEgressEvents.push_back(TimeBounds::Schedule(delay, [this, packet, header, ackKey] {
        m_rdmaPendingAck.erase(ackKey);
        m_ibgda->NotifyDataReady();
        OnPacketDelivered(packet, header.GetSequenceNumber(), header);
    }));
    return true;
}

void
FabricEndpoint::StartRdmaHostWrite(const RdmaReceiveKey& key,
                                   std::shared_ptr<RdmaReceiveWork> work,
                                   Ptr<Packet> packet,
                                   FabricHeader header)
{
    IbgdaPacketTag tag;
    packet->PeekPacketTag(tag);
    m_ibgda->DestinationWrite(
        header.GetEffectiveDataSize(),
        [this, key, work, packet, header] {
            m_rdmaPendingAck.erase({header.GetSourceRank(),
                                    header.GetVirtualChannel(),
                                    header.GetFlowId(),
                                    header.GetSequenceNumber()});
            SendLlrAckPacket(header, false); // Transport completes at registered HOST memory.
            work->received += header.GetEffectiveDataSize();
            work->ready.push_back({packet, header});
            NS_ABORT_MSG_IF(work->received > work->bytes, "Receive staging byte ledger overflow");
            if (work->received != work->bytes)
            {
                return;
            }
            m_ibgda->DestinationH2d(work->bytes, [this, key, work] {
                m_ibgda->NotifyDataReady();
                m_ibgda->ReleaseReceiveStaging(work->bytes);
                m_rdmaReceiveWork.erase(key);
                for (auto& piece : work->ready)
                {
                    if (!m_ibgda)
                    {
                        return;
                    }
                    OnPacketDelivered(piece.packet, piece.header.GetSequenceNumber(), piece.header);
                }
            });
        },
        tag.dmaOffset);
}

void
FabricEndpoint::OnIbgdaPhyTxBegin(Ptr<const Packet> packet)
{
    if (!m_ibgda)
    {
        return;
    }
    IbgdaPacketTag tag;
    if (!packet->PeekPacketTag(tag))
    {
        return;
    }
    RdmaOperationTag operation;
    if (packet->PeekPacketTag(operation) && operation.owner != m_rank)
    {
        return;
    }
    auto copy = packet->Copy();
    PppHeader ppp;
    FabricHeader header;
    if (copy->GetSize() < ppp.GetSerializedSize() + header.GetSerializedSize())
    {
        return;
    }
    copy->RemoveHeader(ppp);
    copy->PeekHeader(header);
    if (header.GetSourceRank() == m_rank)
    {
        m_ibgda->NotifyFirstWire(tag.workId);
    }
}

uint16_t
FabricEndpoint::GetIbgdaRateFlow(Ptr<const Packet> packet, const FabricHeader& header) const
{
    IbgdaPacketTag tag;
    return packet->PeekPacketTag(tag) ? static_cast<uint16_t>(tag.qp) : header.GetFlowId();
}
} // namespace ns3
