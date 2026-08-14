/*
 * hccs-protocol-model.h
 *
 * Huawei HCCS protocol model. Derives from ProtocolModel.
 * HCCS uses a unified transfer protocol — no LL/LL128/SIMPLE distinction.
 * Wire efficiency: ~95% (5% header overhead per chunk).
 * Startup delay: ~50-70µs for Huawei collective launch.
 */

#ifndef HCCS_PROTOCOL_MODEL_H
#define HCCS_PROTOCOL_MODEL_H

#include "protocol-model.h"
#include "ns3/core-module.h"
#include <cstdint>

namespace ns3
{

// HCCS protocol IDs
enum class HccsProtocol : uint8_t
{
    NONE = 0,
    SIMPLE = 1   // HCCS has only one data transfer mode (with 5% overhead)
};

class HccsProtocolModel : public ProtocolModel
{
public:
    static TypeId GetTypeId();

    HccsProtocolModel();
    virtual ~HccsProtocolModel();

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
    uint64_t m_startupDelayNs;     // Startup delay (default: 60µs)
    uint64_t m_perGpuStartupDelayNs; // Per-GPU scaling factor (default: 0)
    uint64_t m_perStepLatencyNs;   // Per-step latency (default: 200ns)
    uint64_t m_chunkSize;          // Default chunk size (default: 512KB)
    double m_wireEfficiency;       // Wire efficiency (default: 0.95)
};

} // namespace ns3

#endif // HCCS_PROTOCOL_MODEL_H