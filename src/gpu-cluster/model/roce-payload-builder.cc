/*
 * roce-payload-builder.cc
 */

#include "roce-payload-builder.h"
#include "ns3/log.h"
#include <cstring>

namespace ns3 {

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

RocePayloadBuilder::RocePayloadBuilder()
{
}

RocePayloadBuilder::~RocePayloadBuilder()
{
}

std::vector<Ptr<Packet>>
RocePayloadBuilder::BuildChunks(const uint8_t* data, uint64_t dataSize,
                                uint8_t protocolId, uint64_t chunkSize) const
{
    std::vector<Ptr<Packet>> chunks;

    if (dataSize == 0 || data == nullptr)
    {
        return chunks;
    }

    // RoCE: add 8-byte RDMA header per chunk (simulates 5% overhead)
    uint64_t offset = 0;
    while (offset < dataSize)
    {
        uint64_t remaining = dataSize - offset;
        uint64_t dataBytes = std::min(remaining, chunkSize);

        uint64_t wireBytes = ROCE_HEADER_SIZE + dataBytes;
        uint8_t* buffer = new uint8_t[wireBytes];
        std::memset(buffer, 0, wireBytes);

        // Write header: size (4 bytes) + reserved (4 bytes)
        uint32_t sizeField = static_cast<uint32_t>(dataBytes);
        std::memcpy(buffer, &sizeField, 4);

        if (dataBytes > 0)
        {
            std::memcpy(buffer + ROCE_HEADER_SIZE, data + offset, dataBytes);
        }

        Ptr<Packet> chunk = Create<Packet>(buffer, wireBytes);
        chunks.push_back(chunk);

        delete[] buffer;
        offset += dataBytes;
    }

    return chunks;
}

uint64_t
RocePayloadBuilder::ExtractData(Ptr<Packet> packet, uint8_t protocolId,
                                uint8_t* outData, uint64_t maxDataSize) const
{
    if (packet == nullptr || outData == nullptr || maxDataSize == 0)
    {
        return 0;
    }

    uint32_t packetSize = packet->GetSize();
    if (packetSize < ROCE_HEADER_SIZE)
    {
        return 0;
    }

    uint8_t headerBuffer[ROCE_HEADER_SIZE];
    packet->CopyData(headerBuffer, ROCE_HEADER_SIZE);

    uint32_t dataBytes = 0;
    std::memcpy(&dataBytes, headerBuffer, 4);

    if (dataBytes > maxDataSize)
    {
        dataBytes = static_cast<uint32_t>(maxDataSize);
    }

    uint8_t* packetBuffer = new uint8_t[packetSize];
    packet->CopyData(packetBuffer, packetSize);

    uint64_t copySize = std::min(static_cast<uint64_t>(dataBytes),
                                 static_cast<uint64_t>(packetSize - ROCE_HEADER_SIZE));
    std::memcpy(outData, packetBuffer + ROCE_HEADER_SIZE, copySize);

    delete[] packetBuffer;
    return copySize;
}

uint64_t
RocePayloadBuilder::GetWireSize(uint64_t dataSize, uint8_t protocolId) const
{
    return static_cast<uint64_t>(dataSize / 0.95);
}

uint64_t
RocePayloadBuilder::GetEffectiveDataSize(uint64_t wireSize, uint8_t protocolId) const
{
    return static_cast<uint64_t>(wireSize * 0.95);
}

std::string
RocePayloadBuilder::GetVendorName() const
{
    return "Intel";
}

} // namespace ns3