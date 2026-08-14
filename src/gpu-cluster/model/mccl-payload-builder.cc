/*
 * Raw MCCL payload segmentation for MetaX C500 systems.
 */

#include "mccl-payload-builder.h"

#include "ns3/log.h"

#include <algorithm>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("McclPayloadBuilder");
NS_OBJECT_ENSURE_REGISTERED(McclPayloadBuilder);

TypeId
McclPayloadBuilder::GetTypeId()
{
    static TypeId tid = TypeId("ns3::McclPayloadBuilder")
                            .SetParent<ProtocolPayloadBuilder>()
                            .SetGroupName("GpuCluster")
                            .AddConstructor<McclPayloadBuilder>();
    return tid;
}

McclPayloadBuilder::McclPayloadBuilder() = default;
McclPayloadBuilder::~McclPayloadBuilder() = default;

std::vector<Ptr<Packet>>
McclPayloadBuilder::BuildChunks(const uint8_t* data,
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

    for (uint64_t offset = 0; offset < dataSize; offset += chunkSize)
    {
        uint64_t size = std::min(chunkSize, dataSize - offset);
        chunks.push_back(Create<Packet>(data + offset, size));
    }
    return chunks;
}

uint64_t
McclPayloadBuilder::ExtractData(Ptr<Packet> packet,
                                uint8_t protocolId,
                                uint8_t* outData,
                                uint64_t maxDataSize) const
{
    (void)protocolId;
    if (!packet || !outData || maxDataSize == 0)
    {
        return 0;
    }
    uint64_t size = std::min<uint64_t>(packet->GetSize(), maxDataSize);
    packet->CopyData(outData, size);
    return size;
}

uint64_t
McclPayloadBuilder::GetWireSize(uint64_t dataSize, uint8_t protocolId) const
{
    (void)protocolId;
    return dataSize;
}

uint64_t
McclPayloadBuilder::GetEffectiveDataSize(uint64_t wireSize, uint8_t protocolId) const
{
    (void)protocolId;
    return wireSize;
}

std::string
McclPayloadBuilder::GetVendorName() const
{
    return "MetaX";
}

} // namespace ns3
