/*
 * if-protocol-model.h
 *
 * AMD Infinity Fabric (xGMI3) protocol model. Derives from ProtocolModel.
 * IF has a cache-coherent protocol with 100% efficiency for data transfer.
 * Startup delay: ~50-80µs for RCCL collective launch.
 */

#ifndef IF_PROTOCOL_MODEL_H
#define IF_PROTOCOL_MODEL_H

#include "protocol-model.h"
#include "ns3/core-module.h"
#include <cstdint>

namespace ns3
{

// IF protocol IDs
enum class IfProtocol : uint8_t
{
    NONE = 0,
    SIMPLE = 1   // IF has one data transfer mode (cache-coherent, 100% efficiency)
};

class IfProtocolModel : public ProtocolModel
{
public:
    static TypeId GetTypeId();

    IfProtocolModel();
    virtual ~IfProtocolModel();

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
    uint64_t m_startupDelayNs;     // Startup delay (default: 70µs)
    uint64_t m_perGpuStartupDelayNs; // Per-GPU scaling factor (default: 0)
    uint64_t m_perStepLatencyNs;   // Per-step latency (default: 150ns)
    uint64_t m_chunkSize;          // Default chunk size (default: 512KB)
};

} // namespace ns3

#endif // IF_PROTOCOL_MODEL_H