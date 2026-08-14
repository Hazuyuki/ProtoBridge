/*
 * ici-protocol-model.cc
 */

#include "ici-protocol-model.h"
#include "ns3/log.h"
#include "ns3/uinteger.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("IciProtocolModel");

NS_OBJECT_ENSURE_REGISTERED(IciProtocolModel);

TypeId
IciProtocolModel::GetTypeId()
{
    static TypeId tid = TypeId("ns3::IciProtocolModel")
                            .SetParent<ProtocolModel>()
                            .SetGroupName("GpuCluster")
                            .AddConstructor<IciProtocolModel>()
                            .AddAttribute("StartupDelayNs",
                                          "ICI startup delay in nanoseconds",
                                          UintegerValue(40000),
                                          MakeUintegerAccessor(&IciProtocolModel::m_startupDelayNs),
                                          MakeUintegerChecker<uint64_t>())
                            .AddAttribute("PerGpuStartupDelayNs",
                                          "Per-GPU scaling factor for startup delay (ns)",
                                          UintegerValue(0),
                                          MakeUintegerAccessor(&IciProtocolModel::m_perGpuStartupDelayNs),
                                          MakeUintegerChecker<uint64_t>())
                            .AddAttribute("PerStepLatencyNs",
                                          "Per-step latency overhead in nanoseconds",
                                          UintegerValue(100),
                                          MakeUintegerAccessor(&IciProtocolModel::m_perStepLatencyNs),
                                          MakeUintegerChecker<uint64_t>())
                            .AddAttribute("ChunkSize",
                                          "Default chunk size in bytes",
                                          UintegerValue(524288),
                                          MakeUintegerAccessor(&IciProtocolModel::m_chunkSize),
                                          MakeUintegerChecker<uint64_t>());
    return tid;
}

IciProtocolModel::IciProtocolModel()
    : m_startupDelayNs(40000),
      m_perGpuStartupDelayNs(0),
      m_perStepLatencyNs(100),
      m_chunkSize(524288)
{
}

IciProtocolModel::~IciProtocolModel()
{
}

uint8_t
IciProtocolModel::GetProtocolId(uint64_t dataSize) const
{
    return static_cast<uint8_t>(IciProtocol::SIMPLE);
}

uint64_t
IciProtocolModel::GetWireSize(uint64_t dataSize, uint8_t protocolId) const
{
    // ICI has 100% efficiency — wire size = data size
    return dataSize;
}

uint64_t
IciProtocolModel::GetDataSize(uint64_t wireSize, uint8_t protocolId) const
{
    // 100% efficiency — data size = wire size
    return wireSize;
}

double
IciProtocolModel::GetEfficiency(uint8_t protocolId) const
{
    return 1.0;
}

uint64_t
IciProtocolModel::GetChunkSize(uint8_t protocolId) const
{
    return m_chunkSize;
}

std::string
IciProtocolModel::GetVendorName() const
{
    return "Google";
}

uint64_t
IciProtocolModel::GetStartupDelayNs(uint8_t protocolId) const
{
    return m_startupDelayNs;
}

uint64_t
IciProtocolModel::GetStartupDelayNs(uint8_t protocolId, uint16_t numGpus) const
{
    return m_startupDelayNs + m_perGpuStartupDelayNs * numGpus;
}

uint64_t
IciProtocolModel::GetPerStepLatencyNs(uint8_t protocolId) const
{
    return m_perStepLatencyNs;
}

uint64_t
IciProtocolModel::GetMemoryLatencyNs(MemoryAccessType accessType) const
{
    switch (accessType)
    {
        case MemoryAccessType::SYNC_LOAD_STORE: return 300;
        case MemoryAccessType::ASYNC_URMA: return 1500;
        default: return 0; // DMA_BULK has no specific latency
    }
}

} // namespace ns3