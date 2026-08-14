/*
 * hccs-protocol-model.cc
 */

#include "hccs-protocol-model.h"
#include "ns3/log.h"
#include "ns3/uinteger.h"
#include "ns3/double.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("HccsProtocolModel");

NS_OBJECT_ENSURE_REGISTERED(HccsProtocolModel);

TypeId
HccsProtocolModel::GetTypeId()
{
    static TypeId tid = TypeId("ns3::HccsProtocolModel")
                            .SetParent<ProtocolModel>()
                            .SetGroupName("GpuCluster")
                            .AddConstructor<HccsProtocolModel>()
                            .AddAttribute("StartupDelayNs",
                                          "HCCS startup delay in nanoseconds",
                                          UintegerValue(60000),
                                          MakeUintegerAccessor(&HccsProtocolModel::m_startupDelayNs),
                                          MakeUintegerChecker<uint64_t>())
                            .AddAttribute("PerGpuStartupDelayNs",
                                          "Per-GPU scaling factor for startup delay (ns)",
                                          UintegerValue(0),
                                          MakeUintegerAccessor(&HccsProtocolModel::m_perGpuStartupDelayNs),
                                          MakeUintegerChecker<uint64_t>())
                            .AddAttribute("PerStepLatencyNs",
                                          "Per-step latency overhead in nanoseconds",
                                          UintegerValue(200),
                                          MakeUintegerAccessor(&HccsProtocolModel::m_perStepLatencyNs),
                                          MakeUintegerChecker<uint64_t>())
                            .AddAttribute("ChunkSize",
                                          "Default chunk size in bytes",
                                          UintegerValue(524288),
                                          MakeUintegerAccessor(&HccsProtocolModel::m_chunkSize),
                                          MakeUintegerChecker<uint64_t>())
                            .AddAttribute("WireEfficiency",
                                          "Wire efficiency ratio (0.0-1.0, default 0.95 for 5% overhead)",
                                          DoubleValue(0.95),
                                          MakeDoubleAccessor(&HccsProtocolModel::m_wireEfficiency),
                                          MakeDoubleChecker<double>(0.0, 1.0));
    return tid;
}

HccsProtocolModel::HccsProtocolModel()
    : m_startupDelayNs(60000),
      m_perGpuStartupDelayNs(0),
      m_perStepLatencyNs(200),
      m_chunkSize(524288),
      m_wireEfficiency(0.95)
{
}

HccsProtocolModel::~HccsProtocolModel()
{
}

uint8_t
HccsProtocolModel::GetProtocolId(uint64_t dataSize) const
{
    return static_cast<uint8_t>(HccsProtocol::SIMPLE);
}

uint64_t
HccsProtocolModel::GetWireSize(uint64_t dataSize, uint8_t protocolId) const
{
    // 5% overhead: wire size = data size / efficiency
    return static_cast<uint64_t>(dataSize / m_wireEfficiency);
}

uint64_t
HccsProtocolModel::GetDataSize(uint64_t wireSize, uint8_t protocolId) const
{
    // Reverse: data size = wire size * efficiency
    return static_cast<uint64_t>(wireSize * m_wireEfficiency);
}

double
HccsProtocolModel::GetEfficiency(uint8_t protocolId) const
{
    return m_wireEfficiency;
}

uint64_t
HccsProtocolModel::GetChunkSize(uint8_t protocolId) const
{
    return m_chunkSize;
}

std::string
HccsProtocolModel::GetVendorName() const
{
    return "Huawei";
}

uint64_t
HccsProtocolModel::GetStartupDelayNs(uint8_t protocolId) const
{
    return m_startupDelayNs;
}

uint64_t
HccsProtocolModel::GetStartupDelayNs(uint8_t protocolId, uint16_t numGpus) const
{
    return m_startupDelayNs + m_perGpuStartupDelayNs * numGpus;
}

uint64_t
HccsProtocolModel::GetPerStepLatencyNs(uint8_t protocolId) const
{
    return m_perStepLatencyNs;
}

uint64_t
HccsProtocolModel::GetMemoryLatencyNs(MemoryAccessType accessType) const
{
    switch (accessType)
    {
        case MemoryAccessType::SYNC_LOAD_STORE: return 200;
        case MemoryAccessType::ASYNC_URMA: return 2000;
        default: return 0; // DMA_BULK has no specific latency
    }
}

} // namespace ns3