/*
 * roce-protocol-model.cc
 */

#include "roce-protocol-model.h"
#include "ns3/log.h"
#include "ns3/uinteger.h"
#include "ns3/double.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("RoceProtocolModel");

NS_OBJECT_ENSURE_REGISTERED(RoceProtocolModel);

TypeId
RoceProtocolModel::GetTypeId()
{
    static TypeId tid = TypeId("ns3::RoceProtocolModel")
                            .SetParent<ProtocolModel>()
                            .SetGroupName("GpuCluster")
                            .AddConstructor<RoceProtocolModel>()
                            .AddAttribute("StartupDelayNs",
                                          "RoCE startup delay in nanoseconds",
                                          UintegerValue(40000),
                                          MakeUintegerAccessor(&RoceProtocolModel::m_startupDelayNs),
                                          MakeUintegerChecker<uint64_t>())
                            .AddAttribute("PerGpuStartupDelayNs",
                                          "Per-GPU scaling factor for startup delay (ns)",
                                          UintegerValue(0),
                                          MakeUintegerAccessor(&RoceProtocolModel::m_perGpuStartupDelayNs),
                                          MakeUintegerChecker<uint64_t>())
                            .AddAttribute("PerStepLatencyNs",
                                          "Per-step latency overhead in nanoseconds",
                                          UintegerValue(800),
                                          MakeUintegerAccessor(&RoceProtocolModel::m_perStepLatencyNs),
                                          MakeUintegerChecker<uint64_t>())
                            .AddAttribute("ChunkSize",
                                          "Default chunk size in bytes",
                                          UintegerValue(524288),
                                          MakeUintegerAccessor(&RoceProtocolModel::m_chunkSize),
                                          MakeUintegerChecker<uint64_t>())
                            .AddAttribute("WireEfficiency",
                                          "Wire efficiency ratio (0.0-1.0, default 0.95 for RDMA header overhead)",
                                          DoubleValue(0.95),
                                          MakeDoubleAccessor(&RoceProtocolModel::m_wireEfficiency),
                                          MakeDoubleChecker<double>(0.0, 1.0));
    return tid;
}

RoceProtocolModel::RoceProtocolModel()
    : m_startupDelayNs(40000),
      m_perGpuStartupDelayNs(0),
      m_perStepLatencyNs(800),
      m_chunkSize(524288),
      m_wireEfficiency(0.95)
{
}

RoceProtocolModel::~RoceProtocolModel()
{
}

uint8_t
RoceProtocolModel::GetProtocolId(uint64_t dataSize) const
{
    return static_cast<uint8_t>(RoceProtocol::SIMPLE);
}

uint64_t
RoceProtocolModel::GetWireSize(uint64_t dataSize, uint8_t protocolId) const
{
    return static_cast<uint64_t>(dataSize / m_wireEfficiency);
}

uint64_t
RoceProtocolModel::GetDataSize(uint64_t wireSize, uint8_t protocolId) const
{
    return static_cast<uint64_t>(wireSize * m_wireEfficiency);
}

double
RoceProtocolModel::GetEfficiency(uint8_t protocolId) const
{
    return m_wireEfficiency;
}

uint64_t
RoceProtocolModel::GetChunkSize(uint8_t protocolId) const
{
    return m_chunkSize;
}

std::string
RoceProtocolModel::GetVendorName() const
{
    return "Intel";
}

uint64_t
RoceProtocolModel::GetStartupDelayNs(uint8_t protocolId) const
{
    return m_startupDelayNs;
}

uint64_t
RoceProtocolModel::GetStartupDelayNs(uint8_t protocolId, uint16_t numGpus) const
{
    return m_startupDelayNs + m_perGpuStartupDelayNs * numGpus;
}

uint64_t
RoceProtocolModel::GetPerStepLatencyNs(uint8_t protocolId) const
{
    return m_perStepLatencyNs;
}

uint64_t
RoceProtocolModel::GetMemoryLatencyNs(MemoryAccessType accessType) const
{
    switch (accessType)
    {
        case MemoryAccessType::SYNC_LOAD_STORE: return 1000;
        case MemoryAccessType::ASYNC_URMA: return 5000;
        default: return 0; // DMA_BULK has no specific latency
    }
}

} // namespace ns3