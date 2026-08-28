/*
 * SPDX-License-Identifier: GPL-2.0-only
 * ethernet-protocol-model.cc
 *
 * Plain (non-RoCE) lossy Ethernet protocol model for the scale-out fabric.
 * Mirrors the RoceProtocolModel skeleton with plain-Ethernet constants. See
 * ethernet-protocol-model.h for the lossy-vs-RoCE rationale.
 */

#include "ethernet-protocol-model.h"

#include "ns3/double.h"
#include "ns3/log.h"
#include "ns3/uinteger.h"

#include <cmath>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("EthernetProtocolModel");
NS_OBJECT_ENSURE_REGISTERED(EthernetProtocolModel);

TypeId
EthernetProtocolModel::GetTypeId()
{
    static TypeId tid =
        TypeId("ns3::EthernetProtocolModel")
            .SetParent<ProtocolModel>()
            .SetGroupName("GpuCluster")
            .AddConstructor<EthernetProtocolModel>()
            .AddAttribute("StartupDelayNs",
                          "Plain-Ethernet per-message NIC-driver startup in ns "
                          "(no RDMA doorbell; store-and-forward NIC path)",
                          UintegerValue(1500),
                          MakeUintegerAccessor(&EthernetProtocolModel::m_startupDelayNs),
                          MakeUintegerChecker<uint64_t>())
            .AddAttribute("PerStepLatencyNs",
                          "Per-switch-hop store-and-forward + cut-through latency in ns "
                          "(higher than RoCE cut-through)",
                          UintegerValue(800),
                          MakeUintegerAccessor(&EthernetProtocolModel::m_perStepLatencyNs),
                          MakeUintegerChecker<uint64_t>())
            .AddAttribute("ChunkSize",
                          "Segmentation granularity in bytes (jumbo-MTU bound; "
                          "plain Ethernet does not coalesce like RDMA)",
                          UintegerValue(9 * 1024),
                          MakeUintegerAccessor(&EthernetProtocolModel::m_chunkSize),
                          MakeUintegerChecker<uint64_t>(1))
            .AddAttribute("WireEfficiency",
                          "Payload / transmitted bytes (Ethernet+VLAN+IFG overhead). "
                          "MUST be 1.0: a value < 1.0 would trigger SoHeader stamping "
                          "(a RoCE-only path) in SendBulkWireTransferSize. Plain "
                          "Ethernet carries only the FabricHeader -- no inner SoHeader.",
                          DoubleValue(1.0),
                          MakeDoubleAccessor(&EthernetProtocolModel::m_wireEfficiency),
                          MakeDoubleChecker<double>(1.0, 1.0));
    return tid;
}

EthernetProtocolModel::EthernetProtocolModel()
    : m_startupDelayNs(1500),
      m_perStepLatencyNs(800),
      m_chunkSize(9 * 1024),
      m_wireEfficiency(1.0)
{
}

EthernetProtocolModel::~EthernetProtocolModel() = default;

uint8_t
EthernetProtocolModel::GetProtocolId(uint64_t dataSize) const
{
    (void)dataSize;
    // 0 (NONE): the non-bulk SendDataAutoProtocol path stamps
    // SetProtocol(protocolId) unconditionally, so any non-zero id would
    // collide with NcclProtocol::LL=1 and misroute Ethernet DATA into the
    // NCCL-LL receiver. The bulk path forces protocol 0 when stamp=false
    // (efficiency=1.0) anyway. Plain Ethernet has no line-protocol tiers.
    return 0;
}

uint64_t
EthernetProtocolModel::GetWireSize(uint64_t dataSize, uint8_t protocolId) const
{
    (void)protocolId;
    return static_cast<uint64_t>(std::ceil(dataSize / m_wireEfficiency));
}

uint64_t
EthernetProtocolModel::GetDataSize(uint64_t wireSize, uint8_t protocolId) const
{
    (void)protocolId;
    return static_cast<uint64_t>(wireSize * m_wireEfficiency);
}

double
EthernetProtocolModel::GetEfficiency(uint8_t protocolId) const
{
    (void)protocolId;
    return m_wireEfficiency;
}

uint64_t
EthernetProtocolModel::GetChunkSize(uint8_t protocolId) const
{
    (void)protocolId;
    return m_chunkSize;
}

std::string
EthernetProtocolModel::GetVendorName() const
{
    return "Ethernet-PLAIN";
}

uint64_t
EthernetProtocolModel::GetStartupDelayNs(uint8_t protocolId) const
{
    (void)protocolId;
    return m_startupDelayNs;
}

uint64_t
EthernetProtocolModel::GetStartupDelayNs(uint8_t protocolId, uint16_t numGpus) const
{
    (void)protocolId;
    (void)numGpus;
    return m_startupDelayNs;
}

uint64_t
EthernetProtocolModel::GetPerStepLatencyNs(uint8_t protocolId) const
{
    (void)protocolId;
    return m_perStepLatencyNs;
}

uint64_t
EthernetProtocolModel::GetMemoryLatencyNs(MemoryAccessType accessType) const
{
    switch (accessType)
    {
        case MemoryAccessType::SYNC_LOAD_STORE:
            return 1000; // synchronous NIC load/store round-trip (ns)
        case MemoryAccessType::ASYNC_URMA:
            return 5000; // asynchronous remote memory access (ns)
        case MemoryAccessType::DMA_BULK:
        default:
            return 0; // bulk DMA: latency is modeled on the wire, not here
    }
}

} // namespace ns3
