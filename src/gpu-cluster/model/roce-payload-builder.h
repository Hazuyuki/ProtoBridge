/*
 * SPDX-License-Identifier: GPL-2.0-only
 * roce-payload-builder.h
 *
 * Builds RoCE v2 RDMA payload segments for the scale-out fabric. Each segment
 * carries a SoHeader (ns3::Header) -- the scale-out protocol header that
 * holds the ECN congestion bit (marked in-flight by the So switch, read on
 * receive to emit a CNP) plus the per-segment RDMA framing (data size +
 * chunk index). The aggregate wire overhead is accounted by the paired
 * RoceProtocolModel::GetWireSize (timing); this builder only shapes packets.
 */
#ifndef ROCE_PAYLOAD_BUILDER_H
#define ROCE_PAYLOAD_BUILDER_H

#include "protocol-payload-builder.h"
#include "so-header.h"

#include <cstdint>
#include <vector>

namespace ns3
{

class RocePayloadBuilder : public ProtocolPayloadBuilder
{
public:
    static TypeId GetTypeId();

    RocePayloadBuilder();
    ~RocePayloadBuilder() override;

    std::vector<Ptr<Packet>> BuildChunks(const uint8_t* data, uint64_t dataSize,
                                         uint8_t protocolId, uint64_t chunkSize) const override;
    uint64_t ExtractData(Ptr<Packet> packet, uint8_t protocolId,
                         uint8_t* outData, uint64_t maxDataSize) const override;
    uint64_t GetWireSize(uint64_t dataSize, uint8_t protocolId) const override;
    uint64_t GetEffectiveDataSize(uint64_t wireSize, uint8_t protocolId) const override;
    std::string GetVendorName() const override;
};

} // namespace ns3

#endif // ROCE_PAYLOAD_BUILDER_H
