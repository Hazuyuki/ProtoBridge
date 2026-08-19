/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Raw MCCL payload segmentation for MetaX GPU systems.
 */

#ifndef MCCL_PAYLOAD_BUILDER_H
#define MCCL_PAYLOAD_BUILDER_H

#include "protocol-payload-builder.h"

namespace ns3
{

class McclPayloadBuilder : public ProtocolPayloadBuilder
{
public:
    static TypeId GetTypeId();

    McclPayloadBuilder();
    ~McclPayloadBuilder() override;

    std::vector<Ptr<Packet>> BuildChunks(const uint8_t* data,
                                         uint64_t dataSize,
                                         uint8_t protocolId,
                                         uint64_t chunkSize) const override;
    uint64_t ExtractData(Ptr<Packet> packet,
                         uint8_t protocolId,
                         uint8_t* outData,
                         uint64_t maxDataSize) const override;
    uint64_t GetWireSize(uint64_t dataSize, uint8_t protocolId) const override;
    uint64_t GetEffectiveDataSize(uint64_t wireSize, uint8_t protocolId) const override;
    std::string GetVendorName() const override;
};

} // namespace ns3

#endif // MCCL_PAYLOAD_BUILDER_H
