/*
 * SPDX-License-Identifier: GPL-2.0-only
 * protocol-model.h
 *
 * Abstract base class for vendor-specific communication protocol models.
 * Each vendor (NVIDIA NCCL, AMD RCCL, etc.) implements this interface
 * to provide protocol selection, wire size calculation, and payload metadata.
 */

#ifndef PROTOCOL_MODEL_H
#define PROTOCOL_MODEL_H

#include "fabric-header.h"
#include "protocol-transaction.h"
#include "ns3/core-module.h"
#include <cstdint>
#include <string>
#include <vector>

namespace ns3
{

class ProtocolModel : public Object
{
public:
    static TypeId GetTypeId();

    ProtocolModel();
    virtual ~ProtocolModel();

    /// Select protocol ID for a given message size (vendor-specific encoding)
    virtual uint8_t GetProtocolId(uint64_t dataSize) const = 0;

    /// Calculate bytes on wire for given data size and protocol ID
    virtual uint64_t GetWireSize(uint64_t dataSize, uint8_t protocolId) const = 0;

    /// Calculate effective data bytes from wire size and protocol ID
    virtual uint64_t GetDataSize(uint64_t wireSize, uint8_t protocolId) const = 0;

    /// Get payload efficiency ratio (0.0 to 1.0) for a protocol ID
    virtual double GetEfficiency(uint8_t protocolId) const = 0;

    /// Get default chunk size for a protocol ID
    virtual uint64_t GetChunkSize(uint8_t protocolId) const = 0;

    /// Get startup delay for a given protocol (vendor-specific, in nanoseconds)
    virtual uint64_t GetStartupDelayNs(uint8_t protocolId) const = 0;

    /// Get startup delay for a given protocol and GPU count (base + perGpu * numGpus)
    virtual uint64_t GetStartupDelayNs(uint8_t protocolId, uint16_t numGpus) const = 0;

    /// Get per-step latency overhead for a given protocol (vendor-specific, in nanoseconds)
    virtual uint64_t GetPerStepLatencyNs(uint8_t protocolId) const = 0;

    /// Identify the vendor this model represents
    virtual std::string GetVendorName() const = 0;

    /// Get memory-semantic latency for a given access type (in nanoseconds)
    virtual uint64_t GetMemoryLatencyNs(MemoryAccessType accessType) const = 0;

    /**
     * Append the protocol actions and delivery dependency for one logical
     * transfer. Protocol implementations may override this method to emit a
     * multi-stage transaction while reusing the same fabric executor.
     *
     * @return The terminal wait node for this transaction fragment.
     */
    virtual ProtocolTransactionNodeId AddTransaction(
        ProtocolTransactionGraph& graph,
        const ProtocolTransactionRequest& request,
        const std::vector<ProtocolTransactionNodeId>& dependencies = {}) const;

    /// Force a specific protocol for all sizes (0=auto-select)
    void SetForceProtocolId(uint8_t protocolId);
    uint8_t GetForceProtocolId() const;

protected:
    uint8_t m_forceProtocolId = 0;  ///< 0=auto, non-zero=force this protocol ID
};

} // namespace ns3

#endif // PROTOCOL_MODEL_H
