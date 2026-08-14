/*
 * hccs-payload-builder.cc
 */

#include "hccs-payload-builder.h"
#include "ns3/log.h"
#include <cstring>

namespace ns3 {

NS_LOG_COMPONENT_DEFINE("HccsPayloadBuilder");

NS_OBJECT_ENSURE_REGISTERED(HccsPayloadBuilder);

TypeId
HccsPayloadBuilder::GetTypeId()
{
    static TypeId tid = TypeId("ns3::HccsPayloadBuilder")
                            .SetParent<ProtocolPayloadBuilder>()
                            .SetGroupName("GpuCluster")
                            .AddConstructor<HccsPayloadBuilder>();
    return tid;
}

HccsPayloadBuilder::HccsPayloadBuilder()
{
}

HccsPayloadBuilder::~HccsPayloadBuilder()
{
}

std::vector<Ptr<Packet>>
HccsPayloadBuilder::BuildChunks(const uint8_t* data, uint64_t dataSize,
                                uint8_t protocolId, uint64_t chunkSize) const
{
    std::vector<Ptr<Packet>> chunks;

    if (dataSize == 0 || data == nullptr)
    {
        return chunks;
    }

    // HCCS: add 8-byte header per chunk (simulates 5% overhead)
    uint64_t offset = 0;
    while (offset < dataSize)
    {
        uint64_t remaining = dataSize - offset;
        uint64_t dataBytes = std::min(remaining, chunkSize);

        // Wire size = header + data
        uint64_t wireBytes = HCCS_HEADER_SIZE + dataBytes;
        uint8_t* buffer = new uint8_t[wireBytes];
        std::memset(buffer, 0, wireBytes);

        // Write header: size (4 bytes) + reserved (4 bytes)
        uint32_t sizeField = static_cast<uint32_t>(dataBytes);
        std::memcpy(buffer, &sizeField, 4);

        // Write data after header
        if (dataBytes > 0)
        {
            std::memcpy(buffer + HCCS_HEADER_SIZE, data + offset, dataBytes);
        }

        Ptr<Packet> chunk = Create<Packet>(buffer, wireBytes);
        chunks.push_back(chunk);

        delete[] buffer;
        offset += dataBytes;
    }

    return chunks;
}

uint64_t
HccsPayloadBuilder::ExtractData(Ptr<Packet> packet, uint8_t protocolId,
                                uint8_t* outData, uint64_t maxDataSize) const
{
    if (packet == nullptr || outData == nullptr || maxDataSize == 0)
    {
        return 0;
    }

    uint32_t packetSize = packet->GetSize();

    // Read HCCS header to get data size
    if (packetSize < HCCS_HEADER_SIZE)
    {
        return 0;
    }

    uint8_t headerBuffer[HCCS_HEADER_SIZE];
    packet->CopyData(headerBuffer, HCCS_HEADER_SIZE);

    uint32_t dataBytes = 0;
    std::memcpy(&dataBytes, headerBuffer, 4);

    if (dataBytes > maxDataSize)
    {
        dataBytes = static_cast<uint32_t>(maxDataSize);
    }

    // Extract data after header
    uint8_t* packetBuffer = new uint8_t[packetSize];
    packet->CopyData(packetBuffer, packetSize);

    uint64_t copySize = std::min(static_cast<uint64_t>(dataBytes),
                                 static_cast<uint64_t>(packetSize - HCCS_HEADER_SIZE));
    std::memcpy(outData, packetBuffer + HCCS_HEADER_SIZE, copySize);

    delete[] packetBuffer;
    return copySize;
}

uint64_t
HccsPayloadBuilder::GetWireSize(uint64_t dataSize, uint8_t protocolId) const
{
    // 5% overhead: wire size ≈ dataSize / 0.95
    // More precisely: number of chunks * (HCCS_HEADER_SIZE + chunkDataSize)
    // For simplicity, use the ratio
    return static_cast<uint64_t>(dataSize / 0.95);
}

uint64_t
HccsPayloadBuilder::GetEffectiveDataSize(uint64_t wireSize, uint8_t protocolId) const
{
    return static_cast<uint64_t>(wireSize * 0.95);
}

std::string
HccsPayloadBuilder::GetVendorName() const
{
    return "Huawei";
}

} // namespace ns3