/*
 * ub-payload-builder.cc
 *
 * Simplified UB Payload Builder implementation
 */

#include "ub-payload-builder.h"

#include "ns3/log.h"
#include "ns3/packet.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("UbPayloadBuilder");

NS_OBJECT_ENSURE_REGISTERED(UbPayloadBuilder);

TypeId
UbPayloadBuilder::GetTypeId()
{
    static TypeId tid = TypeId("ns3::UbPayloadBuilder")
        .SetParent<ProtocolPayloadBuilder>()
        .SetGroupName("GpuCluster")
        .AddConstructor<UbPayloadBuilder>();
    return tid;
}

UbPayloadBuilder::UbPayloadBuilder()
{
}

UbPayloadBuilder::~UbPayloadBuilder()
{
}

std::vector<Ptr<Packet>>
UbPayloadBuilder::BuildChunks(const uint8_t* data, uint64_t dataSize,
                               uint8_t protocolId, uint64_t chunkSize) const
{
    NS_LOG_FUNCTION(this << dataSize << static_cast<int>(protocolId) << chunkSize);

    std::vector<Ptr<Packet>> chunks;
    UbTransaction txn = static_cast<UbTransaction>(protocolId);

    if (txn == UbTransaction::MAINTENANCE)
    {
        // Small fixed-format packet
        std::vector<uint8_t> payload(UB_TRANSACTION_HEADER_SIZE + dataSize, 0);

        // Transaction header: type(1) + srcEID(2) + dstEID(2) + address(4) + size(4) + reserved(3)
        payload[0] = protocolId;
        // address and size fields
        uint32_t sz32 = static_cast<uint32_t>(dataSize);
        std::memcpy(payload.data() + 11, &sz32, 4);

        if (data != nullptr && dataSize > 0)
        {
            std::memcpy(payload.data() + UB_TRANSACTION_HEADER_SIZE, data, dataSize);
        }

        Ptr<Packet> pkt = Create<Packet>(payload.data(), payload.size());
        chunks.push_back(pkt);
        return chunks;
    }

    // MEM_SYNC, MEM_ASYNC, MESSAGE: split into chunks with transaction header per chunk
    uint64_t offset = 0;
    uint64_t remaining = dataSize;
    uint32_t chunkIndex = 0;

    while (remaining > 0)
    {
        uint64_t thisChunkDataSize = std::min(chunkSize, remaining);

        // Build payload: transaction header + data
        std::vector<uint8_t> payload(UB_TRANSACTION_HEADER_SIZE + thisChunkDataSize, 0);

        // Transaction header
        payload[0] = protocolId;                           // type (1 byte)
        payload[1] = static_cast<uint8_t>(chunkIndex);     // chunk index (1 byte)
        // srcEID, dstEID left as 0 (filled by endpoint)
        uint32_t dataSz = static_cast<uint32_t>(thisChunkDataSize);
        std::memcpy(payload.data() + 11, &dataSz, 4);

        if (data != nullptr && thisChunkDataSize > 0)
        {
            std::memcpy(payload.data() + UB_TRANSACTION_HEADER_SIZE, data + offset, thisChunkDataSize);
        }

        Ptr<Packet> pkt = Create<Packet>(payload.data(), payload.size());
        chunks.push_back(pkt);

        offset += thisChunkDataSize;
        remaining -= thisChunkDataSize;
        chunkIndex++;
    }

    return chunks;
}

uint64_t
UbPayloadBuilder::ExtractData(Ptr<Packet> packet, uint8_t protocolId,
                               uint8_t* outData, uint64_t maxDataSize) const
{
    NS_LOG_FUNCTION(this << packet->GetSize() << static_cast<int>(protocolId) << maxDataSize);

    uint32_t pktSize = packet->GetSize();

    if (pktSize <= UB_TRANSACTION_HEADER_SIZE)
    {
        NS_LOG_WARN("Packet too small for UB transaction header");
        return 0;
    }

    // Skip transaction header, extract data payload
    uint32_t dataSize = pktSize - UB_TRANSACTION_HEADER_SIZE;

    if (dataSize > maxDataSize)
    {
        dataSize = static_cast<uint32_t>(maxDataSize);
    }

    if (dataSize == 0)
    {
        return 0;
    }

    // Read entire packet, then copy data portion
    std::vector<uint8_t> buf(pktSize);
    packet->CopyData(buf.data(), pktSize);

    std::memcpy(outData, buf.data() + UB_TRANSACTION_HEADER_SIZE, dataSize);

    return dataSize;
}

uint64_t
UbPayloadBuilder::GetWireSize(uint64_t dataSize, uint8_t protocolId) const
{
    UbTransaction txn = static_cast<UbTransaction>(protocolId);
    (void)txn;

    // 2% Flit overhead + 16-byte transaction header
    double overhead = 1.0 / 0.98; // ~1.02
    uint64_t wireData = static_cast<uint64_t>(dataSize * overhead);

    return UB_TRANSACTION_HEADER_SIZE + wireData;
}

uint64_t
UbPayloadBuilder::GetEffectiveDataSize(uint64_t wireSize, uint8_t protocolId) const
{
    if (wireSize <= UB_TRANSACTION_HEADER_SIZE)
    {
        return 0;
    }

    uint64_t dataWire = wireSize - UB_TRANSACTION_HEADER_SIZE;
    return static_cast<uint64_t>(dataWire * 0.98);
}

std::string
UbPayloadBuilder::GetVendorName() const
{
    return "Huawei";
}

} // namespace ns3