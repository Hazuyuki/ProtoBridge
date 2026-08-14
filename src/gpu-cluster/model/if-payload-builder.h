/*
 * if-payload-builder.h
 *
 * AMD Infinity Fabric payload builder. Derives from ProtocolPayloadBuilder.
 * 100% efficiency — no protocol framing overhead (same pattern as IciPayloadBuilder).
 */

#ifndef IF_PAYLOAD_BUILDER_H
#define IF_PAYLOAD_BUILDER_H

#include "protocol-payload-builder.h"
#include "ns3/core-module.h"

namespace ns3 {

class IfPayloadBuilder : public ProtocolPayloadBuilder
{
public:
    static TypeId GetTypeId();

    IfPayloadBuilder();
    virtual ~IfPayloadBuilder();

    std::vector<Ptr<Packet>> BuildChunks(const uint8_t* data, uint64_t dataSize,
                                          uint8_t protocolId, uint64_t chunkSize) const override;
    uint64_t ExtractData(Ptr<Packet> packet, uint8_t protocolId,
                         uint8_t* outData, uint64_t maxDataSize) const override;
    uint64_t GetWireSize(uint64_t dataSize, uint8_t protocolId) const override;
    uint64_t GetEffectiveDataSize(uint64_t wireSize, uint8_t protocolId) const override;
    std::string GetVendorName() const override;
};

} // namespace ns3

#endif // IF_PAYLOAD_BUILDER_H