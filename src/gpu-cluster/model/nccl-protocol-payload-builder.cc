/*
 * nccl-protocol-payload-builder.cc
 *
 * Builds protocol-aware payloads for NCCL protocols.
 */

#include "nccl-protocol-payload-builder.h"
#include "nccl-protocol-model.h"
#include "ns3/log.h"
#include <cstring>

namespace ns3 {

NS_LOG_COMPONENT_DEFINE("NcclProtocolPayloadBuilder");

NS_OBJECT_ENSURE_REGISTERED(NcclProtocolPayloadBuilder);

TypeId
NcclProtocolPayloadBuilder::GetTypeId()
{
    static TypeId tid = TypeId("ns3::NcclProtocolPayloadBuilder")
                            .SetParent<ProtocolPayloadBuilder>()
                            .SetGroupName("GpuCluster")
                            .AddConstructor<NcclProtocolPayloadBuilder>();
    return tid;
}

NcclProtocolPayloadBuilder::NcclProtocolPayloadBuilder()
{
}

NcclProtocolPayloadBuilder::~NcclProtocolPayloadBuilder()
{
}

std::vector<Ptr<Packet>>
NcclProtocolPayloadBuilder::BuildChunks(const uint8_t* data, uint64_t dataSize,
                                         NcclProtocol protocol)
{
    std::vector<Ptr<Packet>> chunks;

    if (dataSize == 0 || data == nullptr)
    {
        return chunks;
    }

    switch (protocol)
    {
        case NcclProtocol::LL:
            {
                // Build LL lines (64 data + 64 flags per line)
                uint64_t offset = 0;
                uint32_t chunkIndex = 0;

                while (offset < dataSize)
                {
                    uint64_t remaining = dataSize - offset;
                    uint64_t chunkSize = std::min(remaining, LL_DATA_PER_LINE);

                    Ptr<Packet> chunk = BuildLLLine(data + offset, chunkSize);
                    chunks.push_back(chunk);

                    offset += chunkSize;
                    chunkIndex++;
                }
            }
            break;

        case NcclProtocol::LL128:
            {
                // Build LL128 chunks (8 header + 120 data per chunk)
                uint64_t offset = 0;
                uint32_t chunkIndex = 0;

                while (offset < dataSize)
                {
                    uint64_t remaining = dataSize - offset;
                    uint64_t chunkSize = std::min(remaining, LL128_DATA_PER_LINE);

                    Ptr<Packet> chunk = BuildLL128Chunk(data + offset, chunkSize, chunkIndex);
                    chunks.push_back(chunk);

                    offset += chunkSize;
                    chunkIndex++;
                }
            }
            break;

        case NcclProtocol::SIMPLE:
        case NcclProtocol::NONE:
        default:
            {
                // For Simple protocol, use chunking based on NCCL's chunk size
                uint64_t chunkSize = NcclProtocolModel::GetChunkSize(protocol);
                uint64_t offset = 0;

                while (offset < dataSize)
                {
                    uint64_t remaining = dataSize - offset;
                    uint64_t size = std::min(remaining, chunkSize);

                    Ptr<Packet> chunk = BuildSimpleChunk(data + offset, size);
                    chunks.push_back(chunk);

                    offset += size;
                }
            }
            break;
    }

    return chunks;
}

uint64_t
NcclProtocolPayloadBuilder::ExtractData(Ptr<Packet> packet, NcclProtocol protocol,
                                         uint8_t* outData, uint64_t maxDataSize)
{
    if (packet == nullptr || outData == nullptr || maxDataSize == 0)
    {
        return 0;
    }

    switch (protocol)
    {
        case NcclProtocol::LL:
            return ExtractLLLine(packet, outData);

        case NcclProtocol::LL128:
            return ExtractLL128Chunk(packet, outData, nullptr);

        case NcclProtocol::SIMPLE:
        case NcclProtocol::NONE:
        default:
            {
                // Simple protocol: data is raw
                uint32_t packetSize = packet->GetSize();
                uint64_t copySize = std::min(static_cast<uint64_t>(packetSize), maxDataSize);
                packet->CopyData(outData, copySize);
                return copySize;
            }
    }
}

Ptr<Packet>
NcclProtocolPayloadBuilder::BuildLLLine(const uint8_t* data, uint64_t dataBytes)
{
    // LL line format:
    // [0:63]   - Data bytes (padded with 0 if less than 64)
    // [64:127] - Flag bytes (all 0x00 for simulation)

    if (dataBytes > LL_DATA_PER_LINE)
    {
        dataBytes = LL_DATA_PER_LINE;
    }

    uint8_t buffer[LL_LINE_SIZE];
    std::memset(buffer, 0, LL_LINE_SIZE);

    // Copy data bytes to first half
    if (data != nullptr && dataBytes > 0)
    {
        std::memcpy(buffer, data, dataBytes);
    }

    // Second half is flags (already zeroed)

    return Create<Packet>(buffer, LL_LINE_SIZE);
}

Ptr<Packet>
NcclProtocolPayloadBuilder::BuildLL128Chunk(const uint8_t* data, uint64_t dataBytes,
                                            uint32_t chunkIndex)
{
    // LL128 chunk format:
    // [0:7]    - Header (size:4 + chunkIndex:4)
    // [8:127]  - Data bytes (padded with 0 if less than 120)

    if (dataBytes > LL128_DATA_PER_LINE)
    {
        dataBytes = LL128_DATA_PER_LINE;
    }

    uint8_t buffer[LL128_LINE_SIZE];
    std::memset(buffer, 0, LL128_LINE_SIZE);

    // Write header
    LL128Header* header = reinterpret_cast<LL128Header*>(buffer);
    header->size = static_cast<uint32_t>(dataBytes);
    header->chunkIndex = chunkIndex;

    // Copy data bytes after header
    if (data != nullptr && dataBytes > 0)
    {
        std::memcpy(buffer + LL128_HEADER_SIZE, data, dataBytes);
    }

    return Create<Packet>(buffer, LL128_LINE_SIZE);
}

Ptr<Packet>
NcclProtocolPayloadBuilder::BuildSimpleChunk(const uint8_t* data, uint64_t dataBytes)
{
    // Simple: just raw data
    if (data == nullptr || dataBytes == 0)
    {
        return Create<Packet>(0);
    }

    return Create<Packet>(data, dataBytes);
}

uint64_t
NcclProtocolPayloadBuilder::ExtractLLLine(Ptr<Packet> packet, uint8_t* outData)
{
    if (packet == nullptr || outData == nullptr)
    {
        return 0;
    }

    uint32_t packetSize = packet->GetSize();
    if (packetSize < LL_DATA_PER_LINE)
    {
        // Packet too small, copy what we have
        packet->CopyData(outData, packetSize);
        return packetSize;
    }

    // Copy data portion (first 64 bytes)
    packet->CopyData(outData, LL_DATA_PER_LINE);
    return LL_DATA_PER_LINE;
}

uint64_t
NcclProtocolPayloadBuilder::ExtractLL128Chunk(Ptr<Packet> packet, uint8_t* outData,
                                               uint32_t* outChunkIndex)
{
    if (packet == nullptr || outData == nullptr)
    {
        return 0;
    }

    uint32_t packetSize = packet->GetSize();
    if (packetSize < LL128_HEADER_SIZE)
    {
        // Packet too small for header
        return 0;
    }

    // Read header
    uint8_t headerBuffer[LL128_HEADER_SIZE];
    packet->CopyData(headerBuffer, LL128_HEADER_SIZE);

    LL128Header* header = reinterpret_cast<LL128Header*>(headerBuffer);
    uint32_t dataSize = header->size;
    uint32_t chunkIndex = header->chunkIndex;

    if (outChunkIndex != nullptr)
    {
        *outChunkIndex = chunkIndex;
    }

    // Validate data size
    if (dataSize > LL128_DATA_PER_LINE)
    {
        dataSize = LL128_DATA_PER_LINE;
    }

    // Check if packet has enough data
    if (packetSize < LL128_HEADER_SIZE + dataSize)
    {
        dataSize = packetSize - LL128_HEADER_SIZE;
    }

    // Copy data portion
    if (dataSize > 0)
    {
        // We need to copy data starting after header
        uint8_t* packetBuffer = new uint8_t[packetSize];
        packet->CopyData(packetBuffer, packetSize);
        std::memcpy(outData, packetBuffer + LL128_HEADER_SIZE, dataSize);
        delete[] packetBuffer;
    }

    return dataSize;
}

// --- ProtocolPayloadBuilder virtual overrides ---
std::vector<Ptr<Packet>>
NcclProtocolPayloadBuilder::BuildChunks(const uint8_t* data, uint64_t dataSize,
                                        uint8_t protocolId, uint64_t chunkSize) const
{
    NcclProtocol protocol = static_cast<NcclProtocol>(protocolId);
    // Use the existing NCCL-specific BuildChunks (ignores chunkSize for LL/LL128)
    return const_cast<NcclProtocolPayloadBuilder*>(this)->BuildChunks(data, dataSize, protocol);
}

uint64_t
NcclProtocolPayloadBuilder::ExtractData(Ptr<Packet> packet, uint8_t protocolId,
                                        uint8_t* outData, uint64_t maxDataSize) const
{
    NcclProtocol protocol = static_cast<NcclProtocol>(protocolId);
    return const_cast<NcclProtocolPayloadBuilder*>(this)->ExtractData(packet, protocol, outData, maxDataSize);
}

uint64_t
NcclProtocolPayloadBuilder::GetWireSize(uint64_t dataSize, uint8_t protocolId) const
{
    NcclProtocol protocol = static_cast<NcclProtocol>(protocolId);
    return NcclProtocolModel::GetWireSize(dataSize, protocol);
}

uint64_t
NcclProtocolPayloadBuilder::GetEffectiveDataSize(uint64_t wireSize, uint8_t protocolId) const
{
    NcclProtocol protocol = static_cast<NcclProtocol>(protocolId);
    return NcclProtocolModel::GetDataSize(wireSize, protocol);
}

std::string
NcclProtocolPayloadBuilder::GetVendorName() const
{
    return "NVIDIA";
}

} // namespace ns3
