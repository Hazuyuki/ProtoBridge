/*
 * roce-protocol-model.h
 *
 * Intel Gaudi RoCEv2 protocol model. Derives from ProtocolModel.
 * RoCE uses RDMA over Converged Ethernet with ~95% efficiency (5% header overhead).
 * Startup delay: ~30-50µs for Habana collective launch.
 */

#ifndef ROCE_PROTOCOL_MODEL_H
#define ROCE_PROTOCOL_MODEL_H

#include "protocol-model.h"
#include "ns3/core-module.h"
#include <cstdint>

namespace ns3
{

// RoCE protocol IDs
enum class RoceProtocol : uint8_t
{
    NONE = 0,
    SIMPLE = 1   // RoCE has one data transfer mode (with 5% RDMA header overhead)
};

class RoceProtocolModel : public ProtocolModel
{
public:
    static TypeId GetTypeId();

    RoceProtocolModel();
    virtual ~RoceProtocolModel();

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
    uint64_t m_startupDelayNs;     // Startup delay (default: 40µs)
    uint64_t m_perGpuStartupDelayNs; // Per-GPU scaling factor (default: 0)
    uint64_t m_perStepLatencyNs;   // Per-step latency (default: 800ns)
    uint64_t m_chunkSize;          // Default chunk size (default: 512KB)
    double m_wireEfficiency;       // Wire efficiency (default: 0.95)
};

} // namespace ns3

#endif // ROCE_PROTOCOL_MODEL_H