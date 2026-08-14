/*
 * ici-payload-builder.h
 *
 * Google TPU ICI payload builder. Derives from ProtocolPayloadBuilder.
 * ICI has no protocol framing overhead — simple segmentation only.
 */

#ifndef ICI_PAYLOAD_BUILDER_H
#define ICI_PAYLOAD_BUILDER_H

#include "protocol-payload-builder.h"
#include "ns3/core-module.h"

namespace ns3 {

class IciPayloadBuilder : public ProtocolPayloadBuilder
{
public:
    static TypeId GetTypeId();

    IciPayloadBuilder();
    virtual ~IciPayloadBuilder();

    std::vector<Ptr<Packet>> BuildChunks(const uint8_t* data, uint64_t dataSize,
                                          uint8_t protocolId, uint64_t chunkSize) const override;
    uint64_t ExtractData(Ptr<Packet> packet, uint8_t protocolId,
                         uint8_t* outData, uint64_t maxDataSize) const override;
    uint64_t GetWireSize(uint64_t dataSize, uint8_t protocolId) const override;
    uint64_t GetEffectiveDataSize(uint64_t wireSize, uint8_t protocolId) const override;
    std::string GetVendorName() const override;
};

} // namespace ns3

#endif // ICI_PAYLOAD_BUILDER_H