/*
 * SPDX-License-Identifier: GPL-2.0-only
 * nccl-protocol-model.cc
 */

#include "nccl-protocol-model.h"
#include "ns3/log.h"

#include <cmath>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("NcclProtocolModel");

NS_OBJECT_ENSURE_REGISTERED(NcclProtocolModel);

TypeId
NcclProtocolModel::GetTypeId()
{
    static TypeId tid = TypeId("ns3::NcclProtocolModel")
                            .SetParent<ProtocolModel>()
                            .SetGroupName("GpuCluster")
                            .AddConstructor<NcclProtocolModel>()
                            .AddAttribute("LlThreshold",
                                          "Data size threshold for LL→LL128 protocol switch (bytes)",
                                          UintegerValue(8 * 1024),
                                          MakeUintegerAccessor(&NcclProtocolModel::m_llThreshold),
                                          MakeUintegerChecker<uint64_t>())
                            .AddAttribute("Ll128Threshold",
                                          "Data size threshold for LL128→SIMPLE protocol switch (bytes)",
                                          UintegerValue(2 * 1024 * 1024),
                                          MakeUintegerAccessor(&NcclProtocolModel::m_ll128Threshold),
                                          MakeUintegerChecker<uint64_t>())
                            .AddAttribute("StartupDelayLL",
                                          "Startup delay for LL protocol (persistent kernel) in ns",
                                          UintegerValue(65000),
                                          MakeUintegerAccessor(&NcclProtocolModel::m_startupDelayLL),
                                          MakeUintegerChecker<uint64_t>())
                            .AddAttribute("StartupDelayLL128",
                                          "Startup delay for LL128 protocol (persistent kernel) in ns",
                                          UintegerValue(65000),
                                          MakeUintegerAccessor(&NcclProtocolModel::m_startupDelayLL128),
                                          MakeUintegerChecker<uint64_t>())
                            .AddAttribute("StartupDelaySIMPLE",
                                          "Startup delay for SIMPLE protocol (traditional launch) in ns",
                                          UintegerValue(65000),
                                          MakeUintegerAccessor(&NcclProtocolModel::m_startupDelaySIMPLE),
                                          MakeUintegerChecker<uint64_t>())
                            .AddAttribute("SimpleWireEfficiency",
                                          "Payload bytes divided by transmitted bytes for SIMPLE",
                                          DoubleValue(1.0),
                                          MakeDoubleAccessor(&NcclProtocolModel::m_simpleWireEfficiency),
                                          MakeDoubleChecker<double>(0.01, 1.0))
                            .AddAttribute("PerGpuStartupDelayNs",
                                          "Per-GPU scaling factor for startup delay (ns, added per GPU to base delay)",
                                          UintegerValue(0),
                                          MakeUintegerAccessor(&NcclProtocolModel::m_perGpuStartupDelayNs),
                                          MakeUintegerChecker<uint64_t>());
    return tid;
}

NcclProtocolModel::NcclProtocolModel()
    : m_llThreshold(8 * 1024),
      m_ll128Threshold(2 * 1024 * 1024),
      m_startupDelayLL(65000),
      m_startupDelayLL128(65000),
      m_startupDelaySIMPLE(65000),
      m_simpleWireEfficiency(1.0),
      m_perGpuStartupDelayNs(0)
{
}

NcclProtocolModel::~NcclProtocolModel()
{
}

// --- ProtocolModel virtual overrides (uint8_t protocolId) ---

uint8_t
NcclProtocolModel::GetProtocolId(uint64_t dataSize) const
{
    if (m_forceProtocolId != 0)
    {
        return m_forceProtocolId;
    }
    return static_cast<uint8_t>(GetProtocolForSize(dataSize));
}

uint64_t
NcclProtocolModel::GetWireSize(uint64_t dataSize, uint8_t protocolId) const
{
    const auto protocol = static_cast<NcclProtocol>(protocolId);
    if (protocol == NcclProtocol::SIMPLE)
    {
        return static_cast<uint64_t>(std::ceil(dataSize / m_simpleWireEfficiency));
    }
    return GetWireSize(dataSize, protocol);
}

uint64_t
NcclProtocolModel::GetDataSize(uint64_t wireSize, uint8_t protocolId) const
{
    const auto protocol = static_cast<NcclProtocol>(protocolId);
    if (protocol == NcclProtocol::SIMPLE)
    {
        return static_cast<uint64_t>(wireSize * m_simpleWireEfficiency);
    }
    return GetDataSize(wireSize, protocol);
}

double
NcclProtocolModel::GetEfficiency(uint8_t protocolId) const
{
    const auto protocol = static_cast<NcclProtocol>(protocolId);
    if (protocol == NcclProtocol::SIMPLE)
    {
        return m_simpleWireEfficiency;
    }
    return GetEfficiency(protocol);
}

uint64_t
NcclProtocolModel::GetChunkSize(uint8_t protocolId) const
{
    return GetChunkSize(static_cast<NcclProtocol>(protocolId));
}

std::string
NcclProtocolModel::GetVendorName() const
{
    return "NVIDIA";
}

// --- Instance methods using configurable thresholds/Attributes ---

NcclProtocol
NcclProtocolModel::GetProtocolForSize(uint64_t dataSize) const
{
    if (dataSize < m_llThreshold)
    {
        return NcclProtocol::LL;
    }
    else if (dataSize < m_ll128Threshold)
    {
        return NcclProtocol::LL128;
    }
    else
    {
        return NcclProtocol::SIMPLE;
    }
}

uint64_t
NcclProtocolModel::GetStartupDelayNsForSize(uint64_t dataSize) const
{
    return GetStartupDelayNs(static_cast<uint8_t>(GetProtocolForSize(dataSize)));
}

// --- Static convenience methods (backward compat) ---

NcclProtocol
NcclProtocolModel::GetProtocol(uint64_t dataSize)
{
    if (dataSize < 8 * 1024)
    {
        return NcclProtocol::LL;
    }
    else if (dataSize < 2 * 1024 * 1024)
    {
        return NcclProtocol::LL128;
    }
    else
    {
        return NcclProtocol::SIMPLE;
    }
}

uint64_t
NcclProtocolModel::GetWireSize(uint64_t dataSize, NcclProtocol protocol)
{
    switch (protocol)
    {
        case NcclProtocol::LL:
            return ((dataSize + LL_DATA_PER_LINE - 1) / LL_DATA_PER_LINE) * LL_LINE_SIZE;
        case NcclProtocol::LL128:
            return ((dataSize + LL128_DATA_PER_LINE - 1) / LL128_DATA_PER_LINE) * LL128_LINE_SIZE;
        case NcclProtocol::SIMPLE:
        case NcclProtocol::NONE:
        default:
            return dataSize;
    }
}

uint64_t
NcclProtocolModel::GetDataSize(uint64_t wireSize, NcclProtocol protocol)
{
    switch (protocol)
    {
        case NcclProtocol::LL:
            return (wireSize / LL_LINE_SIZE) * LL_DATA_PER_LINE;
        case NcclProtocol::LL128:
            return (wireSize / LL128_LINE_SIZE) * LL128_DATA_PER_LINE;
        case NcclProtocol::SIMPLE:
        case NcclProtocol::NONE:
        default:
            return wireSize;
    }
}

double
NcclProtocolModel::GetEfficiency(NcclProtocol protocol)
{
    switch (protocol)
    {
        case NcclProtocol::LL:
            return static_cast<double>(LL_DATA_PER_LINE) / static_cast<double>(LL_LINE_SIZE);
        case NcclProtocol::LL128:
            return static_cast<double>(LL128_DATA_PER_LINE) / static_cast<double>(LL128_LINE_SIZE);
        case NcclProtocol::SIMPLE:
        case NcclProtocol::NONE:
        default:
            return 1.0;
    }
}

uint64_t
NcclProtocolModel::GetChunkSize(NcclProtocol protocol)
{
    switch (protocol)
    {
        case NcclProtocol::LL:
            return LL_CHUNK_SIZE;
        case NcclProtocol::LL128:
            return LL128_CHUNK_SIZE;
        case NcclProtocol::SIMPLE:
        case NcclProtocol::NONE:
        default:
            return SIMPLE_CHUNK_SIZE;
    }
}

uint64_t
NcclProtocolModel::GetLineSize(NcclProtocol protocol)
{
    switch (protocol)
    {
        case NcclProtocol::LL:
            return LL_LINE_SIZE;
        case NcclProtocol::LL128:
            return LL128_LINE_SIZE;
        case NcclProtocol::SIMPLE:
        case NcclProtocol::NONE:
        default:
            return LL_LINE_SIZE;
    }
}

// --- Instance methods (ProtocolModel virtual overrides) ---

uint64_t
NcclProtocolModel::GetStartupDelayNs(uint8_t protocolId) const
{
    NcclProtocol protocol = static_cast<NcclProtocol>(protocolId);
    switch (protocol)
    {
        case NcclProtocol::LL:     return m_startupDelayLL;
        case NcclProtocol::LL128:  return m_startupDelayLL128;
        case NcclProtocol::SIMPLE: return m_startupDelaySIMPLE;
        case NcclProtocol::NONE:
        default:                   return m_startupDelaySIMPLE;
    }
}

uint64_t
NcclProtocolModel::GetStartupDelayNs(uint8_t protocolId, uint16_t numGpus) const
{
    return GetStartupDelayNs(protocolId) + m_perGpuStartupDelayNs * numGpus;
}

uint64_t
NcclProtocolModel::GetPerStepLatencyNs(uint8_t protocolId) const
{
    return GetPerStepLatencyNs(static_cast<NcclProtocol>(protocolId));
}

// --- Static convenience methods for startup latency ---

uint64_t
NcclProtocolModel::GetStartupDelayNs(NcclProtocol protocol)
{
    switch (protocol)
    {
        case NcclProtocol::LL:     return 65000;   // 65us (kernel launch dominates)
        case NcclProtocol::LL128:  return 65000;   // 65us (kernel launch dominates)
        case NcclProtocol::SIMPLE: return 65000;   // 65us (traditional launch)
        case NcclProtocol::NONE:
        default:                   return 65000;
    }
}

uint64_t
NcclProtocolModel::GetPerStepLatencyNs(NcclProtocol protocol)
{
    switch (protocol)
    {
        case NcclProtocol::LL:     return 0;       // Persistent: no per-step overhead
        case NcclProtocol::LL128:  return 0;       // Persistent: no per-step overhead
        case NcclProtocol::SIMPLE: return 1000;    // ~1us proxy transition per step
        case NcclProtocol::NONE:
        default:                   return 0;
    }
}

uint64_t
NcclProtocolModel::GetMemoryLatencyNs(MemoryAccessType accessType) const
{
    switch (accessType)
    {
        case MemoryAccessType::SYNC_LOAD_STORE: return 500;
        case MemoryAccessType::ASYNC_URMA: return 1000;
        default: return 0; // DMA_BULK has no specific latency
    }
}

} // namespace ns3
