/*
 * hccs-payload-builder.h
 *
 * Huawei HCCS payload builder. Derives from ProtocolPayloadBuilder.
 * 5% header overhead per chunk — simple segmentation with HCCS header prefix.
 */

#ifndef HCCS_PAYLOAD_BUILDER_H
#define HCCS_PAYLOAD_BUILDER_H

#include "protocol-payload-builder.h"
#include "ns3/core-module.h"

namespace ns3 {

class HccsPayloadBuilder : public ProtocolPayloadBuilder
{
public:
    static TypeId GetTypeId();

    HccsPayloadBuilder();
    virtual ~HccsPayloadBuilder();

    std::vector<Ptr<Packet>> BuildChunks(const uint8_t* data, uint64_t dataSize,
                                          uint8_t protocolId, uint64_t chunkSize) const override;
    uint64_t ExtractData(Ptr<Packet> packet, uint8_t protocolId,
                         uint8_t* outData, uint64_t maxDataSize) const override;
    uint64_t GetWireSize(uint64_t dataSize, uint8_t protocolId) const override;
    uint64_t GetEffectiveDataSize(uint64_t wireSize, uint8_t protocolId) const override;
    std::string GetVendorName() const override;

private:
    static constexpr uint64_t HCCS_HEADER_SIZE = 8; // 5% overhead per 128B chunk
};

} // namespace ns3

#endif // HCCS_PAYLOAD_BUILDER_H