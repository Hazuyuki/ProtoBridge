/*
 * SPDX-License-Identifier: GPL-2.0-only
 * nccl-protocol-model.cc
 */

#include "nccl-protocol-model.h"
#include "ns3/log.h"

#include <cmath>
#include <limits>

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
        // Checked conversion (codex round 51, finding 8): at dataSize =
        // UINT64_MAX even efficiency 1.0 rounds the double quotient to
        // 2^64, and the direct static_cast is undefined behavior (the
        // build silently yields 0). Efficiency 1.0 is exact — return the
        // input; otherwise saturate on an out-of-range quotient.
        if (m_simpleWireEfficiency == 1.0)
        {
            return dataSize;
        }
        const double quotient =
            std::ceil(static_cast<double>(dataSize) / m_simpleWireEfficiency);
        if (quotient >= 18446744073709551615.0) // >= 2^64 - 1
        {
            return std::numeric_limits<uint64_t>::max();
        }
        return static_cast<uint64_t>(quotient);
    }
    return GetWireSize(dataSize, protocol);
}

uint64_t
NcclProtocolModel::GetDataSize(uint64_t wireSize, uint8_t protocolId) const
{
    const auto protocol = static_cast<NcclProtocol>(protocolId);
    if (protocol == NcclProtocol::SIMPLE)
    {
        // Round 62: inverse-direction guard, mirroring the checked
        // GetWireSize forward form (codex round 51, finding 8). At
        // SimpleWireEfficiency 1.0 the forward helper returns the input
        // unconverted, but here double(UINT64_MAX) rounds up to 2^64 and
        // the static_cast is UB on any platform. Efficiency 1.0 is exact
        // — return the input; otherwise saturate on an out-of-range
        // product.
        if (m_simpleWireEfficiency == 1.0)
        {
            return wireSize;
        }
        const double product = static_cast<double>(wireSize) * m_simpleWireEfficiency;
        if (!(product < 18446744073709551616.0)) // >= 2^64
        {
            return std::numeric_limits<uint64_t>::max();
        }
        return static_cast<uint64_t>(product);
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
        case NcclProtocol::LL128: {
            // Checked framing (codex round 50, finding 2): the line count
            // times the line size wraps in uint64 for dataSize near 2^63
            // (LL: 2^63 data -> true 2^64 wire, returned 0), and a wrapped
            // zero ledger emitted a header-only "successful" transfer.
            // Saturate instead: the send paths abort on ledgers above the
            // serviceable bound, which a saturated value always exceeds.
            const uint64_t dataPerLine = (protocol == NcclProtocol::LL)
                ? LL_DATA_PER_LINE : LL128_DATA_PER_LINE;
            const uint64_t lineSize = (protocol == NcclProtocol::LL)
                ? LL_LINE_SIZE : LL128_LINE_SIZE;
            // Wrap-free ceiling (codex round 51, finding 7): the last
            // dataPerLine - 1 input values overflowed the ceil ADDITION
            // (dataSize + dataPerLine - 1) before the division, returning
            // a small line count — zero for dataSize = UINT64_MAX — that
            // bypassed the multiplication saturation below. Divide first,
            // then bump on a nonzero remainder.
            const uint64_t lines =
                dataSize / dataPerLine + (dataSize % dataPerLine != 0 ? 1 : 0);
            if (lines > std::numeric_limits<uint64_t>::max() / lineSize)
            {
                return std::numeric_limits<uint64_t>::max();
            }
            return lines * lineSize;
        }
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
