/*
 * ici-protocol-model.h
 *
 * Google TPU ICI protocol model. Derives from ProtocolModel.
 * ICI has a single unified transfer protocol — no LL/LL128/SIMPLE distinction.
 * Wire efficiency: 100% (no protocol framing overhead).
 * Startup delay: ~30-50µs for TPU collective launch.
 */

#ifndef ICI_PROTOCOL_MODEL_H
#define ICI_PROTOCOL_MODEL_H

#include "protocol-model.h"
#include "ns3/core-module.h"
#include <cstdint>

namespace ns3
{

// ICI protocol IDs (vendor-specific encoding)
enum class IciProtocol : uint8_t
{
    NONE = 0,
    SIMPLE = 1   // ICI has only one data transfer mode
};

class IciProtocolModel : public ProtocolModel
{
public:
    static TypeId GetTypeId();

    IciProtocolModel();
    virtual ~IciProtocolModel();

    // ProtocolModel virtual overrides
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
    uint64_t m_startupDelayNs;     // Startup delay in nanoseconds (default: 40µs)
    uint64_t m_perGpuStartupDelayNs; // Per-GPU scaling factor (default: 0)
    uint64_t m_perStepLatencyNs;   // Per-step latency overhead (default: 100ns)
    uint64_t m_chunkSize;          // Default chunk size (default: 512KB)
};

} // namespace ns3

#endif // ICI_PROTOCOL_MODEL_H