/*
 * SPDX-License-Identifier: GPL-2.0-only
 * ethernet-protocol-model.h
 *
 * Plain (non-RoCE) Ethernet protocol model for the scale-out (inter-node)
 * fabric. Lossy best-effort L2 contrast to RoceProtocolModel (lossless RDMA):
 * no SoHeader encapsulation (WireEfficiency=1.0 so SendBulkWireTransferSize's
 * `efficiency < 1.0 && RocePayloadBuilder` gate stays false), no DCQCN/per-QP
 * ceiling (those are RDMA-NIC concepts), store-and-forward per-step latency.
 * Congestion is resolved by LOSSY VOQ tail-drop in EthernetSwitch (a NACK back
 * to the source triggers the existing LLR/SACK retransmission) -- the opposite
 * of RoCE's lossless ECN mark. Constants are spec/NIC-derived (uncalibrated:
 * there is no perftest ground truth for plain Ethernet, unlike RoCE's
 * ib_write_lat); the inter-node startup here is the NIC-driver path, while the
 * collective-level NCCL launch startup is layered above via --interNodeStartup.
 */
#ifndef ETHERNET_PROTOCOL_MODEL_H
#define ETHERNET_PROTOCOL_MODEL_H

#include "protocol-model.h"

namespace ns3
{

/**
 * @ingroup gpu-cluster
 * @brief Plain (non-RoCE) lossy Ethernet protocol model for the scale-out fabric.
 */
class EthernetProtocolModel : public ProtocolModel
{
  public:
    static TypeId GetTypeId();

    EthernetProtocolModel();
    ~EthernetProtocolModel() override;

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

#endif // ETHERNET_PROTOCOL_MODEL_H
