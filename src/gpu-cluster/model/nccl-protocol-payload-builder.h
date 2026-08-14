/*
 * nccl-protocol-payload-builder.h
 *
 * Builds protocol-aware payloads for NCCL protocols.
 * Handles the actual packet structure with headers and flags.
 */

#ifndef NCCL_PROTOCOL_PAYLOAD_BUILDER_H
#define NCCL_PROTOCOL_PAYLOAD_BUILDER_H

#include "protocol-payload-builder.h"
#include "fabric-header.h"
#include "ns3/core-module.h"
#include "ns3/packet.h"
#include <cstdint>
#include <vector>

namespace ns3 {

/**
 * NCCL protocol payload builder
 *
 * Constructs protocol-aware payloads according to NCCL specifications:
 * - LL: 64 bytes data + 64 bytes flags per 128-byte line (50% efficiency)
 * - LL128: 8 bytes header + 120 bytes data per 128-byte chunk (93.75% efficiency)
 * - SIMPLE: Pure data with no per-chunk overhead (100% efficiency)
 */
class NcclProtocolPayloadBuilder : public ProtocolPayloadBuilder
{
public:
    static TypeId GetTypeId();

    NcclProtocolPayloadBuilder();
    virtual ~NcclProtocolPayloadBuilder();

    // --- ProtocolPayloadBuilder virtual overrides ---
    std::vector<Ptr<Packet>> BuildChunks(const uint8_t* data, uint64_t dataSize,
                                          uint8_t protocolId, uint64_t chunkSize) const override;
    uint64_t ExtractData(Ptr<Packet> packet, uint8_t protocolId,
                         uint8_t* outData, uint64_t maxDataSize) const override;
    uint64_t GetWireSize(uint64_t dataSize, uint8_t protocolId) const override;
    uint64_t GetEffectiveDataSize(uint64_t wireSize, uint8_t protocolId) const override;
    std::string GetVendorName() const override;

    /**
     * Build a protocol-aware payload from raw data (NCCL-specific interface)
     *
     * @param data Raw data buffer
     * @param dataSize Size of raw data in bytes
     * @param protocol NCCL protocol to use
     * @return Vector of packets, each containing one chunk
     */
    std::vector<Ptr<Packet>> BuildChunks(const uint8_t* data, uint64_t dataSize,
                                          NcclProtocol protocol);

    /**
     * Extract pure data from protocol-aware payload
     *
     * @param packet Packet with protocol overhead
     * @param protocol NCCL protocol used
     * @param outData Output buffer for extracted data
     * @param maxDataSize Maximum data bytes to extract
     * @return Number of data bytes extracted
     */
    uint64_t ExtractData(Ptr<Packet> packet, NcclProtocol protocol,
                         uint8_t* outData, uint64_t maxDataSize);

    /**
     * Build a single LL line (64 bytes data + 64 bytes flags)
     *
     * @param data Data buffer (up to 64 bytes)
     * @param dataBytes Number of data bytes (1-64)
     * @return Packet containing the LL line
     */
    static Ptr<Packet> BuildLLLine(const uint8_t* data, uint64_t dataBytes);

    /**
     * Build a single LL128 chunk (8 bytes header + 120 bytes data)
     *
     * @param data Data buffer (up to 120 bytes)
     * @param dataBytes Number of data bytes (1-120)
     * @param chunkIndex Index of this chunk in the sequence
     * @return Packet containing the LL128 chunk
     */
    static Ptr<Packet> BuildLL128Chunk(const uint8_t* data, uint64_t dataBytes,
                                       uint32_t chunkIndex);

    /**
     * Build a Simple chunk (pure data)
     *
     * @param data Data buffer
     * @param dataBytes Number of data bytes
     * @return Packet containing the data
     */
    static Ptr<Packet> BuildSimpleChunk(const uint8_t* data, uint64_t dataBytes);

    /**
     * Extract data from an LL line
     *
     * @param packet Packet containing LL line
     * @param outData Output buffer for data (must be at least 64 bytes)
     * @return Number of data bytes extracted (0-64)
     */
    static uint64_t ExtractLLLine(Ptr<Packet> packet, uint8_t* outData);

    /**
     * Extract data from an LL128 chunk
     *
     * @param packet Packet containing LL128 chunk
     * @param outData Output buffer for data (must be at least 120 bytes)
     * @param outChunkIndex Optional output for chunk index
     * @return Number of data bytes extracted (0-120)
     */
    static uint64_t ExtractLL128Chunk(Ptr<Packet> packet, uint8_t* outData,
                                      uint32_t* outChunkIndex = nullptr);

private:
    // Constants for protocol structure
    static constexpr uint64_t LL_LINE_SIZE = 128;
    static constexpr uint64_t LL_DATA_PER_LINE = 64;
    static constexpr uint64_t LL_FLAGS_PER_LINE = 64;

    static constexpr uint64_t LL128_LINE_SIZE = 128;
    static constexpr uint64_t LL128_HEADER_SIZE = 8;
    static constexpr uint64_t LL128_DATA_PER_LINE = 120;

    // LL128 header structure: uint32_t size + uint32_t chunkIndex
    struct LL128Header
    {
        uint32_t size;        // Data bytes in this chunk (1-120)
        uint32_t chunkIndex;  // Index of this chunk in sequence
    } NS3_PACKED;
};

} // namespace ns3

#endif // NCCL_PROTOCOL_PAYLOAD_BUILDER_H
