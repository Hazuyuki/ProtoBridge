/*
 * ici-payload-builder.cc
 */

#include "ici-payload-builder.h"
#include "ns3/log.h"
#include <cstring>

namespace ns3 {

NS_LOG_COMPONENT_DEFINE("IciPayloadBuilder");

NS_OBJECT_ENSURE_REGISTERED(IciPayloadBuilder);

TypeId
IciPayloadBuilder::GetTypeId()
{
    static TypeId tid = TypeId("ns3::IciPayloadBuilder")
                            .SetParent<ProtocolPayloadBuilder>()
                            .SetGroupName("GpuCluster")
                            .AddConstructor<IciPayloadBuilder>();
    return tid;
}

IciPayloadBuilder::IciPayloadBuilder()
{
}

IciPayloadBuilder::~IciPayloadBuilder()
{
}

std::vector<Ptr<Packet>>
IciPayloadBuilder::BuildChunks(const uint8_t* data, uint64_t dataSize,
                               uint8_t protocolId, uint64_t chunkSize) const
{
    std::vector<Ptr<Packet>> chunks;

    if (dataSize == 0 || data == nullptr)
    {
        return chunks;
    }

    // ICI: simple segmentation with no protocol framing overhead
    uint64_t offset = 0;
    while (offset < dataSize)
    {
        uint64_t remaining = dataSize - offset;
        uint64_t size = std::min(remaining, chunkSize);

        Ptr<Packet> chunk = Create<Packet>(data + offset, size);
        chunks.push_back(chunk);

        offset += size;
    }

    return chunks;
}

uint64_t
IciPayloadBuilder::ExtractData(Ptr<Packet> packet, uint8_t protocolId,
                               uint8_t* outData, uint64_t maxDataSize) const
{
    if (packet == nullptr || outData == nullptr || maxDataSize == 0)
    {
        return 0;
    }

    // ICI: data is raw, no protocol overhead
    uint32_t packetSize = packet->GetSize();
    uint64_t copySize = std::min(static_cast<uint64_t>(packetSize), maxDataSize);
    packet->CopyData(outData, copySize);
    return copySize;
}

uint64_t
IciPayloadBuilder::GetWireSize(uint64_t dataSize, uint8_t protocolId) const
{
    return dataSize;
}

uint64_t
IciPayloadBuilder::GetEffectiveDataSize(uint64_t wireSize, uint8_t protocolId) const
{
    return wireSize;
}

std::string
IciPayloadBuilder::GetVendorName() const
{
    return "Google";
}

} // namespace ns3