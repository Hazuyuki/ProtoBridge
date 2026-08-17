/*
 * SPDX-License-Identifier: GPL-2.0-only
 * ub-protocol-model.h
 *
 * Simplified UB (Unified Bus) protocol model for Huawei's SuperPod interconnect.
 * Models UB's transaction layer with 4 transaction types and configurable
 * latency/bandwidth/reliability. Simplified from the full 7-layer UB stack.
 */

#ifndef UB_PROTOCOL_MODEL_H
#define UB_PROTOCOL_MODEL_H

#include "protocol-model.h"
#include "ns3/object.h"

#include <cstdint>

namespace ns3
{

/**
 * @ingroup gpu-cluster
 * @brief UB transaction type enumeration
 */
enum class UbTransaction : uint8_t
{
    MEM_SYNC = 0,       ///< Synchronous memory access (load/store), ~200ns, 64B-4KB
    MEM_ASYNC = 1,      ///< Asynchronous URMA, ~2-5µs, 64B-4KB variable
    MESSAGE = 2,        ///< Point-to-point/multicast message, µs range, arbitrary size
    MAINTENANCE = 3     ///< Cache/status updates, ~100ns, small payload
};

/**
 * @ingroup gpu-cluster
 * @brief Simplified UB Protocol Model
 *
 * Captures key behavioral differences between UB and other fabrics:
 * - Very high wire efficiency (98%) due to Flit-level framing
 * - Memory-semantic access types (sync load/store, async URMA)
 * - 4 transaction types with different latency/size characteristics
 * - LLR enabled by default for reliable optical links
 */
class UbProtocolModel : public ProtocolModel
{
  public:
    static TypeId GetTypeId();

    UbProtocolModel();
    ~UbProtocolModel() override;

    // ProtocolModel interface
    uint8_t GetProtocolId(uint64_t dataSize) const override;
    uint64_t GetWireSize(uint64_t dataSize, uint8_t protocolId) const override;
    uint64_t GetDataSize(uint64_t wireSize, uint8_t protocolId) const override;
    double GetEfficiency(uint8_t protocolId) const override;
    uint64_t GetChunkSize(uint8_t protocolId) const override;
    uint64_t GetStartupDelayNs(uint8_t protocolId) const override;
    uint64_t GetStartupDelayNs(uint8_t protocolId, uint16_t numGpus) const override;
    uint64_t GetPerStepLatencyNs(uint8_t protocolId) const override;
    std::string GetVendorName() const override;
    uint64_t GetMemoryLatencyNs(MemoryAccessType accessType) const override;

    // UB-specific getters
    uint64_t GetSyncMemLatencyNs() const;
    uint64_t GetAsyncMemLatencyNs() const;
    uint64_t GetMaxSyncTransactionSize() const;

  private:
    uint64_t m_syncMemLatencyNs;       ///< Sync load/store latency (default 200ns)
    uint64_t m_asyncMemLatencyNs;      ///< Async URMA latency (default 2000ns)
    uint64_t m_perStepLatencyNs;       ///< Per-step latency for collectives (100ns)
    uint64_t m_startupDelayNs;         ///< Collective launch startup delay (50µs)
    uint64_t m_perGpuStartupDelayNs; ///< Per-GPU scaling factor (default: 0)
    double m_wireEfficiency;           ///< Wire efficiency (0.98 for Flit-level framing)
    uint64_t m_maxSyncTransactionSize; ///< Max size for sync memory transaction (4KB)
};

} // namespace ns3

#endif // UB_PROTOCOL_MODEL_H