/*
 * SPDX-License-Identifier: GPL-2.0-only
 * ub-protocol-model.cc
 *
 * Simplified UB Protocol Model implementation
 */

#include "ub-protocol-model.h"

#include "ns3/log.h"
#include "ns3/uinteger.h"
#include "ns3/double.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("UbProtocolModel");

NS_OBJECT_ENSURE_REGISTERED(UbProtocolModel);

TypeId
UbProtocolModel::GetTypeId()
{
    static TypeId tid = TypeId("ns3::UbProtocolModel")
        .SetParent<ProtocolModel>()
        .SetGroupName("GpuCluster")
        .AddConstructor<UbProtocolModel>()
        .AddAttribute("SyncMemLatencyNs",
                      "Synchronous load/store latency in nanoseconds",
                      UintegerValue(200),
                      MakeUintegerAccessor(&UbProtocolModel::m_syncMemLatencyNs),
                      MakeUintegerChecker<uint64_t>())
        .AddAttribute("AsyncMemLatencyNs",
                      "Asynchronous URMA latency in nanoseconds",
                      UintegerValue(2000),
                      MakeUintegerAccessor(&UbProtocolModel::m_asyncMemLatencyNs),
                      MakeUintegerChecker<uint64_t>())
        .AddAttribute("PerStepLatencyNs",
                      "Per-step latency for collectives in nanoseconds",
                      UintegerValue(100),
                      MakeUintegerAccessor(&UbProtocolModel::m_perStepLatencyNs),
                      MakeUintegerChecker<uint64_t>())
        .AddAttribute("StartupDelayNs",
                      "Collective launch startup delay in nanoseconds",
                      UintegerValue(50000),
                      MakeUintegerAccessor(&UbProtocolModel::m_startupDelayNs),
                      MakeUintegerChecker<uint64_t>())
        .AddAttribute("PerGpuStartupDelayNs",
                      "Per-GPU scaling factor for startup delay (ns)",
                      UintegerValue(0),
                      MakeUintegerAccessor(&UbProtocolModel::m_perGpuStartupDelayNs),
                      MakeUintegerChecker<uint64_t>())
        .AddAttribute("WireEfficiency",
                      "Wire efficiency (0.98 for Flit-level framing)",
                      DoubleValue(0.98),
                      MakeDoubleAccessor(&UbProtocolModel::m_wireEfficiency),
                      MakeDoubleChecker<double>(0.0, 1.0))
        .AddAttribute("MaxSyncTransactionSize",
                      "Maximum size for sync memory transactions in bytes",
                      UintegerValue(4096),
                      MakeUintegerAccessor(&UbProtocolModel::m_maxSyncTransactionSize),
                      MakeUintegerChecker<uint64_t>());
    return tid;
}

UbProtocolModel::UbProtocolModel()
    : m_syncMemLatencyNs(200),
      m_asyncMemLatencyNs(2000),
      m_perStepLatencyNs(100),
      m_startupDelayNs(50000),
      m_perGpuStartupDelayNs(0),
      m_wireEfficiency(0.98),
      m_maxSyncTransactionSize(4096)
{
}

UbProtocolModel::~UbProtocolModel()
{
}

uint8_t
UbProtocolModel::GetProtocolId(uint64_t dataSize) const
{
    if (m_forceProtocolId != 0)
    {
        return m_forceProtocolId;
    }

    if (dataSize <= m_maxSyncTransactionSize && dataSize > 0)
    {
        return static_cast<uint8_t>(UbTransaction::MEM_SYNC);
    }
    else if (dataSize <= 128)
    {
        return static_cast<uint8_t>(UbTransaction::MAINTENANCE);
    }
    else
    {
        return static_cast<uint8_t>(UbTransaction::MESSAGE);
    }
}

uint64_t
UbProtocolModel::GetWireSize(uint64_t dataSize, uint8_t protocolId) const
{
    UbTransaction txn = static_cast<UbTransaction>(protocolId);

    switch (txn)
    {
        case UbTransaction::MEM_SYNC:
        case UbTransaction::MEM_ASYNC:
            // Transaction header (16B) + data * overhead factor
            return 16 + static_cast<uint64_t>(dataSize / m_wireEfficiency);

        case UbTransaction::MESSAGE:
            // Message header (16B) + data * Flit overhead
            return 16 + static_cast<uint64_t>(dataSize / m_wireEfficiency);

        case UbTransaction::MAINTENANCE:
            // Small fixed-format, ~128B wire for tiny payload
            return std::max(static_cast<uint64_t>(128), 16 + dataSize);

        default:
            return 16 + static_cast<uint64_t>(dataSize / m_wireEfficiency);
    }
}

uint64_t
UbProtocolModel::GetDataSize(uint64_t wireSize, uint8_t protocolId) const
{
    UbTransaction txn = static_cast<UbTransaction>(protocolId);

    switch (txn)
    {
        case UbTransaction::MEM_SYNC:
        case UbTransaction::MEM_ASYNC:
        case UbTransaction::MESSAGE:
            // Reverse: wireSize - header = data portion, then divide by overhead
            return static_cast<uint64_t>((wireSize - 16) * m_wireEfficiency);

        case UbTransaction::MAINTENANCE:
            return wireSize - 16;

        default:
            return static_cast<uint64_t>((wireSize - 16) * m_wireEfficiency);
    }
}

double
UbProtocolModel::GetEfficiency(uint8_t protocolId) const
{
    // UB uses Flit-level framing — very high efficiency for all transaction types
    return m_wireEfficiency;
}

uint64_t
UbProtocolModel::GetChunkSize(uint8_t protocolId) const
{
    UbTransaction txn = static_cast<UbTransaction>(protocolId);

    switch (txn)
    {
        case UbTransaction::MEM_SYNC:
        case UbTransaction::MEM_ASYNC:
            return m_maxSyncTransactionSize; // 4KB

        case UbTransaction::MESSAGE:
            return 524288; // 512KB for bulk messages

        case UbTransaction::MAINTENANCE:
            return 128;

        default:
            return 524288;
    }
}

uint64_t
UbProtocolModel::GetStartupDelayNs(uint8_t protocolId) const
{
    UbTransaction txn = static_cast<UbTransaction>(protocolId);

    switch (txn)
    {
        case UbTransaction::MEM_SYNC:
        case UbTransaction::MEM_ASYNC:
            return 0; // Memory semantics don't need collective launch delay

        case UbTransaction::MESSAGE:
            return m_startupDelayNs; // 50µs collective launch

        case UbTransaction::MAINTENANCE:
            return 0;

        default:
            return m_startupDelayNs;
    }
}

uint64_t
UbProtocolModel::GetStartupDelayNs(uint8_t protocolId, uint16_t numGpus) const
{
    return GetStartupDelayNs(protocolId) + m_perGpuStartupDelayNs * numGpus;
}

uint64_t
UbProtocolModel::GetPerStepLatencyNs(uint8_t protocolId) const
{
    return m_perStepLatencyNs;
}

std::string
UbProtocolModel::GetVendorName() const
{
    return "Huawei";
}

uint64_t
UbProtocolModel::GetMemoryLatencyNs(MemoryAccessType accessType) const
{
    switch (accessType)
    {
        case MemoryAccessType::SYNC_LOAD_STORE: return m_syncMemLatencyNs;
        case MemoryAccessType::ASYNC_URMA: return m_asyncMemLatencyNs;
        default: return 0; // DMA_BULK
    }
}

uint64_t
UbProtocolModel::GetSyncMemLatencyNs() const
{
    return m_syncMemLatencyNs;
}

uint64_t
UbProtocolModel::GetAsyncMemLatencyNs() const
{
    return m_asyncMemLatencyNs;
}

uint64_t
UbProtocolModel::GetMaxSyncTransactionSize() const
{
    return m_maxSyncTransactionSize;
}

} // namespace ns3
