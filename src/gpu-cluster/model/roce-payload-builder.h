/*
 * roce-payload-builder.h
 *
 * Intel Gaudi RoCE payload builder. Derives from ProtocolPayloadBuilder.
 * 5% RDMA header overhead per chunk — same pattern as HccsPayloadBuilder.
 */

#ifndef ROCE_PAYLOAD_BUILDER_H
#define ROCE_PAYLOAD_BUILDER_H

#include "protocol-payload-builder.h"
#include "ns3/core-module.h"

namespace ns3 {

class RocePayloadBuilder : public ProtocolPayloadBuilder
{
public:
    static TypeId GetTypeId();

    RocePayloadBuilder();
    virtual ~RocePayloadBuilder();

    std::vector<Ptr<Packet>> BuildChunks(const uint8_t* data, uint64_t dataSize,
                                          uint8_t protocolId, uint64_t chunkSize) const override;
    uint64_t ExtractData(Ptr<Packet> packet, uint8_t protocolId,
                         uint8_t* outData, uint64_t maxDataSize) const override;
    uint64_t GetWireSize(uint64_t dataSize, uint8_t protocolId) const override;
    uint64_t GetEffectiveDataSize(uint64_t wireSize, uint8_t protocolId) const override;
    std::string GetVendorName() const override;

private:
    static constexpr uint64_t ROCE_HEADER_SIZE = 8; // RDMA header per chunk
};

} // namespace ns3

#endif // ROCE_PAYLOAD_BUILDER_H