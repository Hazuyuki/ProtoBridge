/*
 * ub-payload-builder.h
 *
 * Simplified UB payload builder for Flit-level transmission and transaction framing.
 */

#ifndef UB_PAYLOAD_BUILDER_H
#define UB_PAYLOAD_BUILDER_H

#include "protocol-payload-builder.h"
#include "ub-protocol-model.h"

#include "ns3/object.h"
#include <cstdint>
#include <vector>

namespace ns3
{

/**
 * @ingroup gpu-cluster
 * @brief Simplified UB Payload Builder
 *
 * Models UB's Flit-level transmission and transaction framing.
 * Transaction header: 16 bytes (type, srcEID, dstEID, address, size).
 * Wire overhead: 2% Flit-level framing (very efficient compared to NCCL LL/LL128).
 */
class UbPayloadBuilder : public ProtocolPayloadBuilder
{
  public:
    static TypeId GetTypeId();

    UbPayloadBuilder();
    ~UbPayloadBuilder() override;

    std::vector<Ptr<Packet>> BuildChunks(const uint8_t* data, uint64_t dataSize,
                                          uint8_t protocolId, uint64_t chunkSize) const override;

    uint64_t ExtractData(Ptr<Packet> packet, uint8_t protocolId,
                         uint8_t* outData, uint64_t maxDataSize) const override;

    uint64_t GetWireSize(uint64_t dataSize, uint8_t protocolId) const override;
    uint64_t GetEffectiveDataSize(uint64_t wireSize, uint8_t protocolId) const override;

    std::string GetVendorName() const override;

  private:
    static const uint32_t UB_TRANSACTION_HEADER_SIZE = 16;
};

} // namespace ns3

#endif // UB_PAYLOAD_BUILDER_H