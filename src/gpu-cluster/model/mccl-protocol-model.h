/*
 * SPDX-License-Identifier: GPL-2.0-only
 * MCCL collective protocol profile for MetaX C500 systems.
 */

#ifndef MCCL_PROTOCOL_MODEL_H
#define MCCL_PROTOCOL_MODEL_H

#include "protocol-model.h"

namespace ns3
{

enum class McclProtocol : uint8_t
{
    NONE = 0,
    SIMPLE = 1
};

class McclProtocolModel : public ProtocolModel
{
public:
    static TypeId GetTypeId();

    McclProtocolModel();
    ~McclProtocolModel() override;

    uint8_t GetProtocolId(uint64_t dataSize) const override;
    uint64_t GetWireSize(uint64_t dataSize, uint8_t protocolId) const override;
    uint64_t GetDataSize(uint64_t wireSize, uint8_t protocolId) const override;
    double GetEfficiency(uint8_t protocolId) const override;
    uint64_t GetChunkSize(uint8_t protocolId) const override;
    std::string GetVendorName() const override;
    uint64_t GetStartupDelayNs(uint8_t protocolId) const override;
    uint64_t GetStartupDelayNs(uint8_t protocolId, uint16_t numGpus) const override;
    uint64_t GetPerStepLatencyNs(uint8_t protocolId) const override;
    uint64_t GetMemoryLatencyNs(MemoryAccessType accessType) const override;

private:
    uint64_t m_startupDelayNs;
    uint64_t m_perGpuStartupDelayNs;
    uint64_t m_perStepLatencyNs;
    uint64_t m_chunkSize;
    double m_wireEfficiency;
};

} // namespace ns3

#endif // MCCL_PROTOCOL_MODEL_H
