/*
 * SPDX-License-Identifier: GPL-2.0-only
 * RoCE v2 RDMA payload segmentation for the scale-out fabric. Mirrors the
 * McclPayloadBuilder raw-segmentation template with an 8-byte RDMA transport
 * header prepended per segment (BTH+aETH framing) and stripped on extract.
 */

#include "roce-payload-builder.h"

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
        uint64_t pktSize = size + ROCE_RDMA_HEADER_SIZE;

        std::vector<uint8_t> buffer(pktSize);
        RoceRdmaHeader header;
        header.size = static_cast<uint32_t>(size);
        header.chunkIndex = chunkIndex++;
        std::memcpy(buffer.data(), &header, ROCE_RDMA_HEADER_SIZE);
        std::memcpy(buffer.data() + ROCE_RDMA_HEADER_SIZE, data + offset, size);
        chunks.push_back(Create<Packet>(buffer.data(), pktSize));
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

    uint32_t packetSize = packet->GetSize();
    if (packetSize < ROCE_RDMA_HEADER_SIZE)
    {
        return 0;
    }

    uint8_t headerBuffer[ROCE_RDMA_HEADER_SIZE];
    packet->CopyData(headerBuffer, ROCE_RDMA_HEADER_SIZE);
    RoceRdmaHeader header;
    std::memcpy(&header, headerBuffer, ROCE_RDMA_HEADER_SIZE);

    uint32_t dataSize = header.size;
    if (dataSize > packetSize - ROCE_RDMA_HEADER_SIZE)
    {
        dataSize = packetSize - ROCE_RDMA_HEADER_SIZE;
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
    std::memcpy(outData, packetBuffer.data() + ROCE_RDMA_HEADER_SIZE, dataSize);
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
