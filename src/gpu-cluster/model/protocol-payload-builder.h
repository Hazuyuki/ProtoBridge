/*
 * protocol-payload-builder.h
 *
 * Abstract base class for vendor-specific payload builders.
 * Each vendor (NVIDIA NCCL, Google ICI, Huawei HCCS, AMD IF, Intel RoCE)
 * implements this interface to build protocol-aware payload chunks.
 */

#ifndef PROTOCOL_PAYLOAD_BUILDER_H
#define PROTOCOL_PAYLOAD_BUILDER_H

#include "fabric-header.h"
#include "ns3/core-module.h"
#include "ns3/packet.h"
#include <cstdint>
#include <vector>

namespace ns3 {

class ProtocolPayloadBuilder : public Object
{
public:
    static TypeId GetTypeId();

    ProtocolPayloadBuilder();
    virtual ~ProtocolPayloadBuilder();

    /**
     * Build protocol-aware payload chunks from raw data
     *
     * @param data Raw data buffer
     * @param dataSize Size of raw data in bytes
     * @param protocolId Vendor-specific protocol ID
     * @param chunkSize Maximum chunk size in bytes
     * @return Vector of packets, each containing one chunk with protocol framing
     */
    virtual std::vector<Ptr<Packet>> BuildChunks(const uint8_t* data, uint64_t dataSize,
                                                  uint8_t protocolId, uint64_t chunkSize) const = 0;

    /**
     * Extract pure data from a protocol-aware payload chunk
     *
     * @param packet Packet with protocol overhead
     * @param protocolId Vendor-specific protocol ID
     * @param outData Output buffer for extracted data
     * @param maxDataSize Maximum data bytes to extract
     * @return Number of data bytes extracted
     */
    virtual uint64_t ExtractData(Ptr<Packet> packet, uint8_t protocolId,
                                 uint8_t* outData, uint64_t maxDataSize) const = 0;

    /**
     * Calculate wire size for a given data size and protocol
     *
     * @param dataSize Pure data size in bytes
     * @param protocolId Vendor-specific protocol ID
     * @return Wire size (data + protocol overhead) in bytes
     */
    virtual uint64_t GetWireSize(uint64_t dataSize, uint8_t protocolId) const = 0;

    /**
     * Calculate effective data size from wire size and protocol
     *
     * @param wireSize Wire size in bytes
     * @param protocolId Vendor-specific protocol ID
     * @return Pure data size in bytes
     */
    virtual uint64_t GetEffectiveDataSize(uint64_t wireSize, uint8_t protocolId) const = 0;

    /**
     * Identify the vendor this builder represents
     */
    virtual std::string GetVendorName() const = 0;
};

} // namespace ns3

#endif // PROTOCOL_PAYLOAD_BUILDER_H