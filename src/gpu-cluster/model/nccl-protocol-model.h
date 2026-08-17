/*
 * SPDX-License-Identifier: GPL-2.0-only
 * nccl-protocol-model.h
 *
 * NVIDIA NCCL protocol model. Derives from ProtocolModel and implements
 * vendor-specific protocol selection, wire size calculation, and payload metadata.
 *
 * NCCL protocol overhead:
 * - LL: 50% efficiency (64 data + 64 flags per 128B line), <8KB messages
 * - LL128: 93.75% efficiency (8 header + 120 data per 128B chunk), 8KB-2MB
 * - SIMPLE: 100% efficiency (pure data), >2MB
 *
 * Per-protocol startup latency:
 * - LL/LL128: persistent kernel, startup dominated by kernel launch+sync (~65us)
 * - SIMPLE: traditional kernel launch (~65us, similar total cost)
 *
 * The protocol-specific difference is mainly in wire efficiency (bandwidth cost),
 * not in initial startup delay. Startup is dominated by kernel launch and barrier
 * synchronization, which is similar across protocols.
 */

#ifndef NCCL_PROTOCOL_MODEL_H
#define NCCL_PROTOCOL_MODEL_H

#include "protocol-model.h"
#include "nccl-protocol.h"
#include "ns3/core-module.h"
#include <cstdint>

namespace ns3
{

class NcclProtocolModel : public ProtocolModel
{
public:
    static TypeId GetTypeId();

    NcclProtocolModel();
    virtual ~NcclProtocolModel();

    // --- ProtocolModel virtual overrides ---
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

    // --- Instance methods using configurable thresholds/Attributes ---
    NcclProtocol GetProtocolForSize(uint64_t dataSize) const;
    uint64_t GetStartupDelayNsForSize(uint64_t dataSize) const;

    // --- Static convenience methods (backward compat, hardcoded thresholds) ---
    static NcclProtocol GetProtocol(uint64_t dataSize);
    static uint64_t GetWireSize(uint64_t dataSize, NcclProtocol protocol);
    static uint64_t GetDataSize(uint64_t wireSize, NcclProtocol protocol);
    static double GetEfficiency(NcclProtocol protocol);
    static uint64_t GetChunkSize(NcclProtocol protocol);
    static uint64_t GetLineSize(NcclProtocol protocol);
    static uint64_t GetStartupDelayNs(NcclProtocol protocol);
    static uint64_t GetPerStepLatencyNs(NcclProtocol protocol);

private:
    // Protocol selection thresholds (configurable via Attributes)
    uint64_t m_llThreshold;        // LL→LL128 boundary (default 8KB)
    uint64_t m_ll128Threshold;     // LL128→SIMPLE boundary (default 2MB)

    // Wire structure constants
    static constexpr uint64_t LL_LINE_SIZE = 128;
    static constexpr uint64_t LL_DATA_PER_LINE = 64;

    static constexpr uint64_t LL128_LINE_SIZE = 128;
    static constexpr uint64_t LL128_HEADER_SIZE = 8;
    static constexpr uint64_t LL128_DATA_PER_LINE = 120;

    static constexpr uint64_t LL_CHUNK_SIZE = 16384;
    static constexpr uint64_t LL128_CHUNK_SIZE = 8192;
    static constexpr uint64_t SIMPLE_CHUNK_SIZE = 524288;

    // Per-protocol startup delays (ns)
    uint64_t m_startupDelayLL;
    uint64_t m_startupDelayLL128;
    uint64_t m_startupDelaySIMPLE;
    double m_simpleWireEfficiency;

    // Per-GPU startup delay scaling factor (ns, added per GPU to base delay)
    uint64_t m_perGpuStartupDelayNs;
};

} // namespace ns3

#endif // NCCL_PROTOCOL_MODEL_H
