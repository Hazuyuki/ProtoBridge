/*
 * if-protocol-model.cc
 */

#include "if-protocol-model.h"
#include "ns3/log.h"
#include "ns3/uinteger.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("IfProtocolModel");

NS_OBJECT_ENSURE_REGISTERED(IfProtocolModel);

TypeId
IfProtocolModel::GetTypeId()
{
    static TypeId tid = TypeId("ns3::IfProtocolModel")
                            .SetParent<ProtocolModel>()
                            .SetGroupName("GpuCluster")
                            .AddConstructor<IfProtocolModel>()
                            .AddAttribute("StartupDelayNs",
                                          "IF startup delay in nanoseconds",
                                          UintegerValue(70000),
                                          MakeUintegerAccessor(&IfProtocolModel::m_startupDelayNs),
                                          MakeUintegerChecker<uint64_t>())
                            .AddAttribute("PerGpuStartupDelayNs",
                                          "Per-GPU scaling factor for startup delay (ns)",
                                          UintegerValue(0),
                                          MakeUintegerAccessor(&IfProtocolModel::m_perGpuStartupDelayNs),
                                          MakeUintegerChecker<uint64_t>())
                            .AddAttribute("PerStepLatencyNs",
                                          "Per-step latency overhead in nanoseconds",
                                          UintegerValue(150),
                                          MakeUintegerAccessor(&IfProtocolModel::m_perStepLatencyNs),
                                          MakeUintegerChecker<uint64_t>())
                            .AddAttribute("ChunkSize",
                                          "Default chunk size in bytes",
                                          UintegerValue(524288),
                                          MakeUintegerAccessor(&IfProtocolModel::m_chunkSize),
                                          MakeUintegerChecker<uint64_t>());
    return tid;
}

IfProtocolModel::IfProtocolModel()
    : m_startupDelayNs(70000),
      m_perGpuStartupDelayNs(0),
      m_perStepLatencyNs(150),
      m_chunkSize(524288)
{
}

IfProtocolModel::~IfProtocolModel()
{
}

uint8_t
IfProtocolModel::GetProtocolId(uint64_t dataSize) const
{
    return static_cast<uint8_t>(IfProtocol::SIMPLE);
}

uint64_t
IfProtocolModel::GetWireSize(uint64_t dataSize, uint8_t protocolId) const
{
    return dataSize; // 100% efficiency
}

uint64_t
IfProtocolModel::GetDataSize(uint64_t wireSize, uint8_t protocolId) const
{
    return wireSize; // 100% efficiency
}

double
IfProtocolModel::GetEfficiency(uint8_t protocolId) const
{
    return 1.0;
}

uint64_t
IfProtocolModel::GetChunkSize(uint8_t protocolId) const
{
    return m_chunkSize;
}

std::string
IfProtocolModel::GetVendorName() const
{
    return "AMD";
}

uint64_t
IfProtocolModel::GetStartupDelayNs(uint8_t protocolId) const
{
    return m_startupDelayNs;
}

uint64_t
IfProtocolModel::GetStartupDelayNs(uint8_t protocolId, uint16_t numGpus) const
{
    return m_startupDelayNs + m_perGpuStartupDelayNs * numGpus;
}

uint64_t
IfProtocolModel::GetPerStepLatencyNs(uint8_t protocolId) const
{
    return m_perStepLatencyNs;
}

uint64_t
IfProtocolModel::GetMemoryLatencyNs(MemoryAccessType accessType) const
{
    switch (accessType)
    {
        case MemoryAccessType::SYNC_LOAD_STORE: return 400;
        case MemoryAccessType::ASYNC_URMA: return 1800;
        default: return 0; // DMA_BULK has no specific latency
    }
}

} // namespace ns3