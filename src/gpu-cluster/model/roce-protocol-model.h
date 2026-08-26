/*
 * SPDX-License-Identifier: GPL-2.0-only
 * roce-protocol-model.h
 *
 * RoCE v2 (RDMA over Converged Ethernet) protocol model for the scale-out
 * (inter-node) Ethernet fabric. SIMPLE-only: unlike NCCL LL/LL128 line
 * protocols (which are scale-up oriented), a RoCEv2 message is coalesced
 * into ChunkSize segments (default 512 KB; real 9 KB jumbo-MTU frame
 * splitting is deferred -- see CHANGES.md WU3), each carrying an 8-byte
 * RDMA transport header (BTH + aETH) over UDP/IP/Ethernet. The aggregate
 * wire overhead
 * (Eth+IP+UDP+BTH+aETH encapsulation + RDMA NIC pipeline) is captured by
 * WireEfficiency; the per-message RDMA NIC startup (doorbell + WQE) by
 * StartupDelayNs; the per-switch-hop serialization + cut-through latency by
 * PerStepLatencyNs. Uncalibrated lower-bound: constants are spec/NIC-derived
 * (ConnectX-class startup, jumbo MTU, ~95% wire efficiency) and anchor the
 * So fabric's timing to the ns-3 model, not to measured hardware.
 */
#ifndef ROCE_PROTOCOL_MODEL_H
#define ROCE_PROTOCOL_MODEL_H

#include "protocol-model.h"

namespace ns3
{

enum class RoceProtocol : uint8_t
{
    NONE = 0,
    SIMPLE = 1
};

/**
 * @ingroup gpu-cluster
 * @brief RoCE v2 protocol model for the scale-out Ethernet fabric.
 */
class RoceProtocolModel : public ProtocolModel
{
public:
    static TypeId GetTypeId();

    RoceProtocolModel();
    ~RoceProtocolModel() override;

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
    uint64_t m_perStepLatencyNs;
    uint64_t m_chunkSize;
    double m_wireEfficiency;
};

} // namespace ns3

#endif // ROCE_PROTOCOL_MODEL_H
