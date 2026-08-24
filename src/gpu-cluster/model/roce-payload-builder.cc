/*
 * SPDX-License-Identifier: GPL-2.0-only
 * RoCE v2 RDMA payload segmentation for the scale-out fabric. Each segment
 * carries a SoHeader (the scale-out protocol header: ECN congestion bit +
 * data size + chunk index, matching RoCEv2 BTH+aETH framing) prepended per
 * segment and stripped on extract.
 */

#include "roce-payload-builder.h"

#include "roce-protocol-model.h" // RoceProtocol::SIMPLE
#include "ns3/log.h"
#include "ns3/packet.h"

#include <algorithm>
#include <cstring>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("RocePayloadBuilder");
NS_OBJECT_ENSURE_REGISTERED(RocePayloadBuilder);

TypeId
RocePayloadBuilder::GetTypeId()
{
    static TypeId tid = TypeId("ns3::RocePayloadBuilder")
                            .SetParent<ProtocolPayloadBuilder>()
                            .SetGroupName("GpuCluster")
                            .AddConstructor<RocePayloadBuilder>();
    return tid;
}

RocePayloadBuilder::RocePayloadBuilder() = default;
RocePayloadBuilder::~RocePayloadBuilder() = default;

std::vector<Ptr<Packet>>
RocePayloadBuilder::BuildChunks(const uint8_t* data,
                                uint64_t dataSize,
                                uint8_t protocolId,
                                uint64_t chunkSize) const
{
    (void)protocolId;
    std::vector<Ptr<Packet>> chunks;
    if (!data || dataSize == 0 || chunkSize == 0)
    {
        return chunks;
    }

    uint32_t chunkIndex = 0;
    for (uint64_t offset = 0; offset < dataSize; offset += chunkSize)
    {
        uint64_t size = std::min(chunkSize, dataSize - offset);
        Ptr<Packet> pkt = Create<Packet>(data + offset, static_cast<uint32_t>(size));
        SoHeader header;
        header.SetProtocolId(static_cast<uint8_t>(RoceProtocol::SIMPLE));
        header.SetEcn(false); // set in-flight by the So switch on congestion
        header.SetDataSize(static_cast<uint32_t>(size));
        header.SetChunkIndex(chunkIndex++);
        pkt->AddHeader(header);
        chunks.push_back(pkt);
    }
    return chunks;
}

uint64_t
RocePayloadBuilder::ExtractData(Ptr<Packet> packet,
                                uint8_t protocolId,
                                uint8_t* outData,
                                uint64_t maxDataSize) const
{
    (void)protocolId;
    if (!packet || !outData || maxDataSize == 0)
    {
        return 0;
    }

    // SoHeader is outermost here (the FabricHeader was peeled upstream on the
    // receive path), so PeekHeader deserializes it without mutating the packet.
    SoHeader header;
    packet->PeekHeader(header);
    uint32_t headerSize = header.GetSerializedSize();
    uint32_t packetSize = packet->GetSize();
    if (packetSize <= headerSize)
    {
        return 0;
    }

    uint32_t dataSize = header.GetDataSize();
    uint32_t avail = packetSize - headerSize;
    if (dataSize > avail)
    {
        dataSize = avail;
    }
    if (dataSize > maxDataSize)
    {
        dataSize = static_cast<uint32_t>(maxDataSize);
    }
    if (dataSize == 0)
    {
        return 0;
    }

    std::vector<uint8_t> packetBuffer(packetSize);
    packet->CopyData(packetBuffer.data(), packetSize);
    std::memcpy(outData, packetBuffer.data() + headerSize, dataSize);
    return dataSize;
}

uint64_t
RocePayloadBuilder::GetWireSize(uint64_t dataSize, uint8_t protocolId) const
{
    (void)protocolId;
    return dataSize;
}

uint64_t
RocePayloadBuilder::GetEffectiveDataSize(uint64_t wireSize, uint8_t protocolId) const
{
    (void)protocolId;
    return wireSize;
}

std::string
RocePayloadBuilder::GetVendorName() const
{
    return "Ethernet-RoCEv2";
}

} // namespace ns3
