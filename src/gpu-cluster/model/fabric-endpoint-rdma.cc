/* SPDX-License-Identifier: GPL-2.0-only */
#include "fabric-endpoint.h"
#include "so-header.h"

#include "ns3/abort.h"

#include <algorithm>
#include <cmath>

namespace ns3
{
uint64_t
FabricEndpoint::SubmitRdmaOperation(const RdmaRequest& request,
                                    RdmaOperationEngine::Completion complete)
{
    NS_ABORT_MSG_IF(!m_rdmaOperations || IsLocalRank(request.peer) ||
                        GetFabricTypeForDest(request.peer) != FabricType::ROCE,
                    "Detailed RDMA operations require a remote RoCE peer");
    return m_rdmaOperations->Submit(request, std::move(complete));
}

uint64_t
FabricEndpoint::SubmitRdmaRead(const RdmaRequest& input, RdmaOperationEngine::Completion complete)
{
    auto request = input;
    request.opcode = RdmaOpcode::READ;
    return SubmitRdmaOperation(request, std::move(complete));
}

uint64_t
FabricEndpoint::SubmitRdmaSend(const RdmaRequest& input, RdmaOperationEngine::Completion complete)
{
    auto request = input;
    if (request.opcode != RdmaOpcode::SEND_IMM)
    {
        request.opcode = RdmaOpcode::SEND;
    }
    return SubmitRdmaOperation(request, std::move(complete));
}

uint64_t
FabricEndpoint::SubmitRdmaFetchAdd(const RdmaRequest& input,
                                   RdmaOperationEngine::Completion complete)
{
    auto request = input;
    request.opcode = RdmaOpcode::FETCH_ADD;
    return SubmitRdmaOperation(request, std::move(complete));
}

uint64_t
FabricEndpoint::SubmitRdmaCompareSwap(const RdmaRequest& input,
                                      RdmaOperationEngine::Completion complete)
{
    auto request = input;
    request.opcode = RdmaOpcode::COMPARE_SWAP;
    return SubmitRdmaOperation(request, std::move(complete));
}

uint64_t
FabricEndpoint::SubmitRdmaPutSignal(const RdmaRequest& input,
                                    RdmaOperationEngine::Completion complete)
{
    auto request = input;
    request.opcode = RdmaOpcode::PUT_SIGNAL;
    return SubmitRdmaOperation(request, std::move(complete));
}

void
FabricEndpoint::RegisterRdmaMemory(uint64_t address, uint64_t bytes, uint32_t key)
{
    NS_ABORT_MSG_IF(!m_rdmaOperations, "Configure RDMA before registering memory");
    m_rdmaOperations->RegisterMemory(address, bytes, key);
}

void
FabricEndpoint::PostRdmaReceive(uint64_t id,
                                uint64_t address,
                                uint64_t bytes,
                                RdmaOperationEngine::ReceiveCompletion complete,
                                uint16_t peer,
                                uint32_t qp,
                                uint32_t key)
{
    NS_ABORT_MSG_IF(!m_rdmaOperations, "Configure RDMA before posting receives");
    m_rdmaOperations->PostReceive(id, address, bytes, std::move(complete), peer, qp, key);
}

void
FabricEndpoint::QueueRdmaOperationPacket(uint16_t peer,
                                         Ptr<Packet> packet,
                                         const RdmaOperationTag& tag,
                                         uint32_t qp,
                                         uint16_t flow,
                                         uint8_t vc,
                                         uint32_t overhead)
{
    m_rdmaTxQueue.push_back({peer, packet, tag, qp, flow, vc, overhead});
    PumpRdmaOperationPackets();
}

void
FabricEndpoint::PumpRdmaOperationPackets()
{
    while (!m_rdmaTxQueue.empty() && (!m_soTxWindow || m_bulkTxInflight < m_soTxWindow))
    {
        auto pending = std::move(m_rdmaTxQueue.front());
        m_rdmaTxQueue.pop_front();
        uint32_t seq = m_nextSeqNum[MakeSeqKey(pending.peer, pending.vc, pending.flow)]++;
        auto packet = pending.packet->Copy();
        uint32_t dataBytes = packet->GetSize();
        if (pending.overhead)
        {
            packet->AddAtEnd(Create<Packet>(pending.overhead));
        }
        SoHeader so;
        so.SetProtocolId(1);
        so.SetDataSize(dataBytes);
        so.SetChunkIndex(seq);
        packet->AddHeader(so);
        double efficiency = GetProtocolModelForDest(pending.peer)->GetEfficiency(1);
        // Round 61: the uint32 cast below is UB when the /efficiency
        // expansion reaches 2^32. The built-in model checkers floor
        // efficiency at std::numeric_limits<double>::min() (2^-1022, the
        // smallest normal), where even a ~4-byte body already targets
        // 2^1024, and a custom model registered without a checker can go
        // lower still. Fail loudly on any expansion that cannot be
        // represented as a uint32 — the same hard-configuration-error
        // policy as the SendDataInternal wire-efficiency pad abort
        // (fabric-endpoint-send.cc).
        const double expanded =
            std::ceil(static_cast<double>(dataBytes + pending.overhead) / efficiency);
        NS_ABORT_MSG_IF(!(expanded < 4294967296.0),
                        "RDMA wire-efficiency padding target " << expanded << " B (efficiency "
                                                               << efficiency << ") cannot be "
                                                               << "represented as a uint32 packet");
        uint32_t target = static_cast<uint32_t>(std::max<double>(packet->GetSize(), expanded));
        if (target > packet->GetSize())
        {
            packet->AddAtEnd(Create<Packet>(target - packet->GetSize()));
        }
        FabricHeader header;
        header.SetPacketType(FabricPacketType::DATA);
        header.SetFabricType(GetFabricTypeForDest(pending.peer));
        header.SetProtocol(1);
        header.SetProtocolFramed(true);
        header.SetSourceRank(m_rank);
        header.SetDestRank(pending.peer);
        header.SetFlowId(pending.flow);
        header.SetVirtualChannel(pending.vc);
        header.SetSequenceNumber(seq);
        header.SetSourceMac(m_address);
        header.SetDestMac(ResolveDestMac(pending.peer));
        header.SetPayloadSize(packet->GetSize());
        header.SetEffectiveDataSize(dataBytes);
        RdmaPacketTag work;
        work.workId = pending.tag.workId;
        work.workBytes = pending.tag.bytes;
        work.qp = pending.qp;
        work.sourceReady = true;
        packet->AddPacketTag(work);
        packet->AddPacketTag(pending.tag);
        m_rdmaOperations->PacketSent(header, packet);
        if (m_soTxWindow)
        {
            ++m_bulkTxInflight;
            m_bulkTxWindowSeqs.insert({pending.peer, pending.vc, pending.flow, seq});
        }
        auto devices = GetPhysicalDevicesForDest(pending.peer);
        uint32_t device = devices.empty() ? ResolveDeviceIndex(pending.peer)
                                          : devices[m_globalSprayOffset++ % devices.size()];
        if (!FlowControlGate(pending.vc, seq, true))
        {
            SendQueueEntry entry;
            entry.packet = packet;
            entry.header = header;
            entry.vcId = pending.vc;
            entry.seqNum = seq;
            entry.deviceIndex = device;
            m_sendQueue.push_back(entry);
            continue;
        }
        packet->AddHeader(header);
        if (SendPacketOnDevice(packet, device, ResolveDestMac(pending.peer)))
        {
            m_txTrace(packet->GetSize(), seq);
            ++m_txPackets;
            m_txBytes += packet->GetSize();
        }
    }
}
} // namespace ns3
