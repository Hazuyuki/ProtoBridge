/*
 * SPDX-License-Identifier: GPL-2.0-only
 * RoCE v2 (RDMA over Converged Ethernet) protocol model for the scale-out
 * fabric. Mirrors the McclProtocolModel SIMPLE-only skeleton with RoCEv2
 * spec/NIC-derived constants (uncalibrated lower-bound).
 */

#include "roce-protocol-model.h"

#include "ns3/double.h"
#include "ns3/log.h"
#include "ns3/uinteger.h"

#include <cmath>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("RoceProtocolModel");
NS_OBJECT_ENSURE_REGISTERED(RoceProtocolModel);

TypeId
RoceProtocolModel::GetTypeId()
{
    static TypeId tid =
        TypeId("ns3::RoceProtocolModel")
            .SetParent<ProtocolModel>()
            .SetGroupName("GpuCluster")
            .AddConstructor<RoceProtocolModel>()
            .AddAttribute("StartupDelayNs",
                          "RoCEv2 per-message RDMA startup (NIC doorbell + WQE) in ns",
                          UintegerValue(40000),
                          MakeUintegerAccessor(&RoceProtocolModel::m_startupDelayNs),
                          MakeUintegerChecker<uint64_t>())
            .AddAttribute("PerStepLatencyNs",
                          "Per-switch-hop serialization + cut-through latency in ns",
                          UintegerValue(800),
                          MakeUintegerAccessor(&RoceProtocolModel::m_perStepLatencyNs),
                          MakeUintegerChecker<uint64_t>())
            .AddAttribute("ChunkSize",
                          "RoCE coalesce granularity in bytes (per-builder segment size; "
                          "real 9 KB MTU frame splitting is deferred -- see CHANGES.md WU3)",
                          UintegerValue(512 * 1024),
                          MakeUintegerAccessor(&RoceProtocolModel::m_chunkSize),
                          MakeUintegerChecker<uint64_t>(1))
            .AddAttribute("WireEfficiency",
                          "Payload bytes divided by transmitted bytes "
                          "(Eth+IP+UDP+BTH+aETH + RDMA pipeline)",
                          DoubleValue(0.95),
                          MakeDoubleAccessor(&RoceProtocolModel::m_wireEfficiency),
                          MakeDoubleChecker<double>(0.01, 1.0));
    return tid;
}

RoceProtocolModel::RoceProtocolModel()
    : m_startupDelayNs(40000),
      m_perStepLatencyNs(800),
      m_chunkSize(512 * 1024),
      m_wireEfficiency(0.95)
{
}

RoceProtocolModel::~RoceProtocolModel() = default;

uint8_t
RoceProtocolModel::GetProtocolId(uint64_t dataSize) const
{
    (void)dataSize;
    return static_cast<uint8_t>(RoceProtocol::SIMPLE);
}

uint64_t
RoceProtocolModel::GetWireSize(uint64_t dataSize, uint8_t protocolId) const
{
    (void)protocolId;
    return static_cast<uint64_t>(std::ceil(dataSize / m_wireEfficiency));
}

uint64_t
RoceProtocolModel::GetDataSize(uint64_t wireSize, uint8_t protocolId) const
{
    (void)protocolId;
    return static_cast<uint64_t>(wireSize * m_wireEfficiency);
}

double
RoceProtocolModel::GetEfficiency(uint8_t protocolId) const
{
    (void)protocolId;
    return m_wireEfficiency;
}

uint64_t
RoceProtocolModel::GetChunkSize(uint8_t protocolId) const
{
    (void)protocolId;
    return m_chunkSize;
}

std::string
RoceProtocolModel::GetVendorName() const
{
    return "Ethernet-RoCEv2";
}

uint64_t
RoceProtocolModel::GetStartupDelayNs(uint8_t protocolId) const
{
    (void)protocolId;
    return m_startupDelayNs;
}

uint64_t
RoceProtocolModel::GetStartupDelayNs(uint8_t protocolId, uint16_t numGpus) const
{
    (void)protocolId;
    (void)numGpus;
    return m_startupDelayNs;
}

uint64_t
RoceProtocolModel::GetPerStepLatencyNs(uint8_t protocolId) const
{
    (void)protocolId;
    return m_perStepLatencyNs;
}

uint64_t
RoceProtocolModel::GetMemoryLatencyNs(MemoryAccessType accessType) const
{
    switch (accessType)
    {
        case MemoryAccessType::SYNC_LOAD_STORE:
            return 1000; // synchronous RDMA load/store round-trip (ns)
        case MemoryAccessType::ASYNC_URMA:
            return 5000; // asynchronous remote memory access (ns)
        case MemoryAccessType::DMA_BULK:
        default:
            return 0; // bulk DMA: latency is modeled on the wire, not here
    }
}

} // namespace ns3
