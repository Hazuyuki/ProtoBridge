/*
 * MCCL collective protocol profile for MetaX C500 systems.
 */

#include "mccl-protocol-model.h"

#include "ns3/double.h"
#include "ns3/log.h"
#include "ns3/uinteger.h"

#include <cmath>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("McclProtocolModel");
NS_OBJECT_ENSURE_REGISTERED(McclProtocolModel);

TypeId
McclProtocolModel::GetTypeId()
{
    static TypeId tid =
        TypeId("ns3::McclProtocolModel")
            .SetParent<ProtocolModel>()
            .SetGroupName("GpuCluster")
            .AddConstructor<McclProtocolModel>()
            .AddAttribute("StartupDelayNs",
                          "MCCL collective startup delay in nanoseconds",
                          UintegerValue(0),
                          MakeUintegerAccessor(&McclProtocolModel::m_startupDelayNs),
                          MakeUintegerChecker<uint64_t>())
            .AddAttribute("PerGpuStartupDelayNs",
                          "Additional startup delay per participating GPU in nanoseconds",
                          UintegerValue(0),
                          MakeUintegerAccessor(&McclProtocolModel::m_perGpuStartupDelayNs),
                          MakeUintegerChecker<uint64_t>())
            .AddAttribute("PerStepLatencyNs",
                          "MCCL processing delay per communication step in nanoseconds",
                          UintegerValue(0),
                          MakeUintegerAccessor(&McclProtocolModel::m_perStepLatencyNs),
                          MakeUintegerChecker<uint64_t>())
            .AddAttribute("ChunkSize",
                          "Default MCCL packetization chunk in bytes",
                          UintegerValue(8 * 1024 * 1024),
                          MakeUintegerAccessor(&McclProtocolModel::m_chunkSize),
                          MakeUintegerChecker<uint64_t>(1))
            .AddAttribute("WireEfficiency",
                          "Payload bytes divided by transmitted bytes",
                          DoubleValue(1.0),
                          MakeDoubleAccessor(&McclProtocolModel::m_wireEfficiency),
                          MakeDoubleChecker<double>(0.01, 1.0));
    return tid;
}

McclProtocolModel::McclProtocolModel()
    : m_startupDelayNs(0),
      m_perGpuStartupDelayNs(0),
      m_perStepLatencyNs(0),
      m_chunkSize(8 * 1024 * 1024),
      m_wireEfficiency(1.0)
{
}

McclProtocolModel::~McclProtocolModel() = default;

uint8_t
McclProtocolModel::GetProtocolId(uint64_t dataSize) const
{
    (void)dataSize;
    return static_cast<uint8_t>(McclProtocol::SIMPLE);
}

uint64_t
McclProtocolModel::GetWireSize(uint64_t dataSize, uint8_t protocolId) const
{
    (void)protocolId;
    return static_cast<uint64_t>(std::ceil(dataSize / m_wireEfficiency));
}

uint64_t
McclProtocolModel::GetDataSize(uint64_t wireSize, uint8_t protocolId) const
{
    (void)protocolId;
    return static_cast<uint64_t>(wireSize * m_wireEfficiency);
}

double
McclProtocolModel::GetEfficiency(uint8_t protocolId) const
{
    (void)protocolId;
    return m_wireEfficiency;
}

uint64_t
McclProtocolModel::GetChunkSize(uint8_t protocolId) const
{
    (void)protocolId;
    return m_chunkSize;
}

std::string
McclProtocolModel::GetVendorName() const
{
    return "MetaX";
}

uint64_t
McclProtocolModel::GetStartupDelayNs(uint8_t protocolId) const
{
    (void)protocolId;
    return m_startupDelayNs;
}

uint64_t
McclProtocolModel::GetStartupDelayNs(uint8_t protocolId, uint16_t numGpus) const
{
    (void)protocolId;
    return m_startupDelayNs + m_perGpuStartupDelayNs * numGpus;
}

uint64_t
McclProtocolModel::GetPerStepLatencyNs(uint8_t protocolId) const
{
    (void)protocolId;
    return m_perStepLatencyNs;
}

uint64_t
McclProtocolModel::GetMemoryLatencyNs(MemoryAccessType accessType) const
{
    (void)accessType;
    return 0;
}

} // namespace ns3
