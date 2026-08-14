/*
 * llr-manager.cc
 *
 * Link Layer Retry (LLR) Manager implementation
 */

#include "llr-manager.h"
#include "fabric-header.h"

#include "ns3/log.h"
#include "ns3/simulator.h"
#include "ns3/uinteger.h"
#include "ns3/enum.h"

#include <algorithm>
#include <utility>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("LlrManager");

NS_OBJECT_ENSURE_REGISTERED(LlrManager);

TypeId
LlrManager::GetTypeId()
{
    static TypeId tid = TypeId("ns3::LlrManager")
        .SetParent<Object>()
        .SetGroupName("GpuCluster")
        .AddConstructor<LlrManager>()
        .AddAttribute("RetryLimit",
                      "Maximum number of retries per packet before giving up",
                      UintegerValue(3),
                      MakeUintegerAccessor(&LlrManager::m_retryLimit),
                      MakeUintegerChecker<uint32_t>())
        .AddAttribute("RetryTimeout",
                      "Timeout before retrying a packet",
                      TimeValue(NanoSeconds(1000)),
                      MakeTimeAccessor(&LlrManager::m_retryTimeout),
                      MakeTimeChecker())
        .AddAttribute("LlrMode",
                      "Retransmission mode",
                      EnumValue<LlrMode>(LlrMode::GO_BACK_N),
                      MakeEnumAccessor<LlrMode>(&LlrManager::SetLlrMode,
                                                &LlrManager::GetLlrMode),
                      MakeEnumChecker<LlrMode>(
                          LlrMode::GO_BACK_N, "ns3::LlrMode::GO_BACK_N",
                          LlrMode::SACK, "ns3::LlrMode::SACK"))
        .AddAttribute("MaxBufferSize",
                      "Maximum retry buffer entries (0 = unlimited). Bounds memory; "
                      "evicted fast copies are reconstructed from the backing source "
                      "when needed. 8192 covers normal "
                      "credit-window depth with retransmission headroom.",
                      UintegerValue(8192),
                      MakeUintegerAccessor(&LlrManager::m_maxBufferSize),
                      MakeUintegerChecker<uint32_t>())
        .AddAttribute("SourceReloadBandwidth",
                      "Bandwidth in bytes/s for reconstructing an evicted packet "
                      "from its backing source (0 = unlimited)",
                      UintegerValue(4800000000000ULL),
                      MakeUintegerAccessor(&LlrManager::m_sourceReloadBandwidth),
                      MakeUintegerChecker<uint64_t>())
        .AddAttribute("SourceReloadLatency",
                      "Fixed access latency for reconstructing an evicted packet",
                      TimeValue(NanoSeconds(300)),
                      MakeTimeAccessor(&LlrManager::m_sourceReloadLatency),
                      MakeTimeChecker())
        .AddAttribute("OverflowPolicy",
                      "Buffer overflow policy",
                      EnumValue<LlrOverflowPolicy>(LlrOverflowPolicy::DROP_OLDEST),
                      MakeEnumAccessor<LlrOverflowPolicy>(&LlrManager::SetOverflowPolicy,
                                                          &LlrManager::GetOverflowPolicy),
                      MakeEnumChecker<LlrOverflowPolicy>(
                          LlrOverflowPolicy::DROP_OLDEST, "ns3::LlrOverflowPolicy::DROP_OLDEST",
                          LlrOverflowPolicy::DROP_NEWEST, "ns3::LlrOverflowPolicy::DROP_NEWEST"))
        .AddTraceSource("Retry",
                        "A packet is retried",
                        MakeTraceSourceAccessor(&LlrManager::m_retryTrace),
                        "ns3::TracedValueCallback::Uint32")
        .AddTraceSource("Ack",
                        "A packet ACK is received",
                        MakeTraceSourceAccessor(&LlrManager::m_ackTrace),
                        "ns3::TracedValueCallback::Uint32")
        .AddTraceSource("Overflow",
                        "Retry buffer overflow",
                        MakeTraceSourceAccessor(&LlrManager::m_overflowTrace),
                        "ns3::TracedValueCallback::Uint32")
        .AddTraceSource("PermanentLoss",
                        "A packet has permanently failed after exceeding retry limit",
                        MakeTraceSourceAccessor(&LlrManager::m_permanentLossTrace),
                        "ns3::TracedValueCallback::Uint32");
    return tid;
}

LlrManager::LlrManager()
    : m_retryLimit(UINT32_MAX),
      m_retryTimeout(NanoSeconds(1000)),
      m_llrMode(LlrMode::GO_BACK_N),
      m_maxBufferSize(8192),
      m_overflowPolicy(LlrOverflowPolicy::DROP_OLDEST),
      m_peakBufferSize(0),
      m_retransmittedPackets(0),
      m_overflowCount(0),
      m_permanentLossCount(0),
      m_retryMissCount(0),
      m_sourceReloadCount(0),
      m_sourceReloadBytes(0),
      m_sourceReloadServiceTime(Seconds(0)),
      m_sourceReloadBandwidth(4800000000000ULL),
      m_sourceReloadLatency(NanoSeconds(300)),
      m_sourceReloadAvailable(Seconds(0))
{
    NS_LOG_FUNCTION(this);
}

LlrManager::~LlrManager()
{
    NS_LOG_FUNCTION(this);
    Clear();
}

void
LlrManager::DoDispose()
{
    Clear();
    Object::DoDispose();
}

void
LlrManager::SetPermanentLossCallback(PermanentLossCallback cb)
{
    m_permanentLossCallback = cb;
}

void
LlrManager::SetLlrMode(LlrMode mode)
{
    NS_LOG_FUNCTION(this << static_cast<int>(mode));
    m_llrMode = mode;
}

LlrMode
LlrManager::GetLlrMode() const
{
    return m_llrMode;
}

bool
LlrManager::StorePacket(uint32_t seqNum, Ptr<Packet> packet, uint16_t destRank)
{
    NS_LOG_FUNCTION(this << seqNum << destRank);

    // Extract flowId from packet header for composite key
    FabricHeader hdr;
    packet->PeekHeader(hdr);
    uint16_t flowId = hdr.GetFlowId();
    RetryKey key = MakeRetryKey(seqNum, destRank, flowId);

    // The finite retry buffer stores a fast packet copy. The source store
    // represents the original payload in GPU memory and is retained until ACK.
    auto sourceIt = m_sourceStore.find(key);
    if (sourceIt == m_sourceStore.end())
    {
        SourceEntry source;
        source.packet = packet->Copy();
        source.destRank = destRank;
        source.retryCount = 0;
        m_sourceStore.emplace(key, std::move(source));
    }
    else
    {
        sourceIt->second.packet = packet->Copy();
        sourceIt->second.destRank = destRank;
    }

    // Remove any existing entry with the same composite key
    auto it = m_retryBuffer.find(key);
    if (it != m_retryBuffer.end())
    {
        if (!it->second.timeoutEvent.IsExpired())
        {
            Simulator::Cancel(it->second.timeoutEvent);
        }
        m_retryBuffer.erase(it);
    }

    // Enforce buffer limit
    if (m_maxBufferSize > 0 && m_retryBuffer.size() >= m_maxBufferSize)
    {
        if (m_overflowPolicy == LlrOverflowPolicy::DROP_NEWEST)
        {
            // Refuse the fast copy; the source record remains reconstructable.
            m_overflowTrace(seqNum, m_retryBuffer.size());
            m_overflowCount++;
            NS_LOG_DEBUG("Retry buffer full (DROP_NEWEST), refusing seqNum " << seqNum);
            return false;
        }
        // DROP_OLDEST: evict oldest entries
        EnforceBufferLimit();
    }

    RetryEntry entry;
    entry.packet = packet->Copy();
    entry.destRank = destRank;

    // Schedule retry timeout
    if (m_retryTimeout > Time(0))
    {
        entry.timeoutEvent = Simulator::Schedule(m_retryTimeout,
                                                  &LlrManager::OnRetryTimeout,
                                                  this, seqNum, destRank, flowId);
    }

    m_retryBuffer[key] = entry;
    m_peakBufferSize = std::max(m_peakBufferSize,
                                static_cast<uint32_t>(m_retryBuffer.size()));
    return true;
}

void
LlrManager::RemovePacket(uint32_t seqNum, uint16_t destRank, uint16_t flowId)
{
    NS_LOG_FUNCTION(this << seqNum << destRank << flowId);

    RetryKey key = MakeRetryKey(seqNum, destRank, flowId);
    if (m_sourceStore.find(key) != m_sourceStore.end())
    {
        m_ackTrace(seqNum, destRank);
    }
    RemoveOutstandingPacket(key);
}

void
LlrManager::RemovePacketsUpTo(uint32_t seqNum, uint16_t destRank, uint16_t flowId)
{
    NS_LOG_FUNCTION(this << seqNum << destRank << flowId);

    std::vector<RetryKey> ackedKeys;
    for (const auto& pair : m_sourceStore)
    {
        uint32_t entrySeqNum = pair.first & 0xFFFFFFFF;
        uint16_t entryFlowId = (pair.first >> 32) & 0xFFFF;
        uint16_t entryDestRank = pair.first >> 48;
        if (entryDestRank == destRank && entryFlowId == flowId && entrySeqNum <= seqNum)
        {
            ackedKeys.push_back(pair.first);
        }
    }
    for (RetryKey key : ackedKeys)
    {
        m_ackTrace(static_cast<uint32_t>(key), destRank);
        RemoveOutstandingPacket(key);
    }
}

void
LlrManager::RemoveSackedPackets(const std::unordered_set<uint32_t>& ackedSeqNums, uint16_t destRank, uint16_t flowId)
{
    NS_LOG_FUNCTION(this << ackedSeqNums.size() << destRank << flowId);

    for (uint32_t seqNum : ackedSeqNums)
    {
        RetryKey key = MakeRetryKey(seqNum, destRank, flowId);
        if (m_sourceStore.find(key) != m_sourceStore.end())
        {
            m_ackTrace(seqNum, destRank);
        }
        RemoveOutstandingPacket(key);
    }
}

std::vector<LlrManager::Retransmission>
LlrManager::HandleRetryRequest(uint32_t seqNum, uint16_t sourceRank, uint16_t flowId)
{
    NS_LOG_FUNCTION(this << seqNum << sourceRank << flowId);

    std::vector<Retransmission> retransmissions;

    // Go-Back-N: retransmit all packets from seqNum onwards in sequence order
    // Match by destRank == sourceRank AND flowId for correct flow isolation
    std::vector<RetryKey> keys;
    for (const auto& pair : m_sourceStore)
    {
        uint32_t entrySeqNum = pair.first & 0xFFFFFFFF;
        uint16_t entryFlowId = (pair.first >> 32) & 0xFFFF;
        if (entryFlowId == flowId && entrySeqNum >= seqNum && pair.second.destRank == sourceRank)
        {
            keys.push_back(pair.first);
        }
    }
    std::sort(keys.begin(), keys.end());

    Time orderingDelay = Seconds(0);
    for (RetryKey key : keys)
    {
        auto sourceIt = m_sourceStore.find(key);
        if (sourceIt == m_sourceStore.end())
        {
            continue;
        }

        uint32_t sn = key & 0xFFFFFFFF;
        sourceIt->second.retryCount++;
        if (sourceIt->second.retryCount > m_retryLimit)
        {
            NS_LOG_WARN("Retry limit exceeded for seqNum " << sn
                        << ", permanently lost to destRank " << sourceIt->second.destRank);
            uint16_t lostDestRank = sourceIt->second.destRank;
            FabricHeader lostHdr;
            sourceIt->second.packet->PeekHeader(lostHdr);
            RemoveOutstandingPacket(key);
            m_permanentLossTrace(sn, lostDestRank);
            m_permanentLossCount++;
            if (!m_permanentLossCallback.IsNull())
            {
                m_permanentLossCallback(lostHdr);
            }
            continue;
        }

        Retransmission retransmission;
        auto bufferIt = m_retryBuffer.find(key);
        if (bufferIt != m_retryBuffer.end())
        {
            retransmission.packet = bufferIt->second.packet->Copy();
            retransmission.readyDelay = orderingDelay;
            retransmission.sourceReload = false;

            if (!bufferIt->second.timeoutEvent.IsExpired())
            {
                Simulator::Cancel(bufferIt->second.timeoutEvent);
            }
            if (m_retryTimeout > Time(0))
            {
                bufferIt->second.timeoutEvent = Simulator::Schedule(
                    orderingDelay + m_retryTimeout,
                    &LlrManager::OnRetryTimeout,
                    this,
                    sn,
                    sourceRank,
                    flowId);
            }
        }
        else
        {
            FabricHeader storedHeader;
            sourceIt->second.packet->PeekHeader(storedHeader);
            uint32_t payloadBytes = storedHeader.GetPayloadSize();
            orderingDelay = std::max(orderingDelay, ReserveSourceReload(payloadBytes));
            retransmission.packet = sourceIt->second.packet->Copy();
            retransmission.readyDelay = orderingDelay;
            retransmission.sourceReload = true;
            m_retryMissCount++;
        }

        m_retryTrace(sn, sourceIt->second.destRank);
        m_retransmittedPackets++;
        retransmissions.push_back(retransmission);
    }

    if (keys.empty())
    {
        m_retryMissCount++;
    }
    return retransmissions;
}

std::vector<LlrManager::Retransmission>
LlrManager::HandleSackRequest(const std::unordered_set<uint32_t>& nackSeqNums,
                               uint16_t sourceRank, uint16_t flowId)
{
    NS_LOG_FUNCTION(this << nackSeqNums.size() << sourceRank << flowId);

    std::vector<Retransmission> retransmissions;

    std::vector<uint32_t> orderedSeqNums(nackSeqNums.begin(), nackSeqNums.end());
    std::sort(orderedSeqNums.begin(), orderedSeqNums.end());
    for (uint32_t seqNum : orderedSeqNums)
    {
        RetryKey key = MakeRetryKey(seqNum, sourceRank, flowId);
        auto sourceIt = m_sourceStore.find(key);
        if (sourceIt == m_sourceStore.end())
        {
            m_retryMissCount++;
            continue;
        }

        sourceIt->second.retryCount++;
        if (sourceIt->second.retryCount > m_retryLimit)
        {
            NS_LOG_WARN("Retry limit exceeded for seqNum " << seqNum
                        << ", permanently lost to destRank " << sourceIt->second.destRank);
            uint16_t lostDestRank = sourceIt->second.destRank;
            FabricHeader lostHdr;
            sourceIt->second.packet->PeekHeader(lostHdr);
            RemoveOutstandingPacket(key);
            m_permanentLossTrace(seqNum, lostDestRank);
            m_permanentLossCount++;
            if (!m_permanentLossCallback.IsNull())
            {
                m_permanentLossCallback(lostHdr);
            }
            continue;
        }

        Retransmission retransmission;
        auto bufferIt = m_retryBuffer.find(key);
        if (bufferIt != m_retryBuffer.end())
        {
            retransmission.packet = bufferIt->second.packet->Copy();
            retransmission.readyDelay = Seconds(0);
            retransmission.sourceReload = false;

            if (!bufferIt->second.timeoutEvent.IsExpired())
            {
                Simulator::Cancel(bufferIt->second.timeoutEvent);
            }
            if (m_retryTimeout > Time(0))
            {
                bufferIt->second.timeoutEvent = Simulator::Schedule(
                    m_retryTimeout,
                    &LlrManager::OnRetryTimeout,
                    this,
                    seqNum,
                    sourceRank,
                    flowId);
            }
        }
        else
        {
            FabricHeader storedHeader;
            sourceIt->second.packet->PeekHeader(storedHeader);
            retransmission.packet = sourceIt->second.packet->Copy();
            retransmission.readyDelay = ReserveSourceReload(storedHeader.GetPayloadSize());
            retransmission.sourceReload = true;
            m_retryMissCount++;
        }

        m_retryTrace(seqNum, sourceIt->second.destRank);
        m_retransmittedPackets++;
        retransmissions.push_back(retransmission);
    }

    return retransmissions;
}

bool
LlrManager::IsRetryLimitExceeded(uint32_t seqNum) const
{
    for (const auto& pair : m_sourceStore)
    {
        if ((pair.first & 0xFFFFFFFF) == seqNum)
        {
            return pair.second.retryCount > m_retryLimit;
        }
    }
    return false;
}

void
LlrManager::SetRetryLimit(uint32_t limit)
{
    m_retryLimit = limit;
}

uint32_t
LlrManager::GetRetryLimit() const
{
    return m_retryLimit;
}

void
LlrManager::SetRetryTimeout(Time timeout)
{
    NS_LOG_FUNCTION(this << timeout);
    m_retryTimeout = timeout;
}

Time
LlrManager::GetRetryTimeout() const
{
    return m_retryTimeout;
}

void
LlrManager::SetTimeoutCallback(TimeoutCallback cb)
{
    m_timeoutCallback = cb;
}

void
LlrManager::SetMaxBufferSize(uint32_t maxSize)
{
    NS_LOG_FUNCTION(this << maxSize);
    m_maxBufferSize = maxSize;
}

uint32_t
LlrManager::GetMaxBufferSize() const
{
    return m_maxBufferSize;
}

uint32_t
LlrManager::GetBufferSize() const
{
    return m_retryBuffer.size();
}

uint32_t
LlrManager::GetPeakBufferSize() const
{
    return m_peakBufferSize;
}

bool
LlrManager::HasOutstandingPacket(uint32_t seqNum, uint16_t destRank, uint16_t flowId) const
{
    return m_sourceStore.find(MakeRetryKey(seqNum, destRank, flowId)) != m_sourceStore.end();
}

uint64_t
LlrManager::GetRetransmittedPackets() const
{
    return m_retransmittedPackets;
}

uint64_t
LlrManager::GetOverflowCount() const
{
    return m_overflowCount;
}

uint64_t
LlrManager::GetPermanentLossCount() const
{
    return m_permanentLossCount;
}

uint64_t
LlrManager::GetRetryMissCount() const
{
    return m_retryMissCount;
}

uint64_t
LlrManager::GetSourceReloadCount() const
{
    return m_sourceReloadCount;
}

uint64_t
LlrManager::GetSourceReloadBytes() const
{
    return m_sourceReloadBytes;
}

Time
LlrManager::GetSourceReloadServiceTime() const
{
    return m_sourceReloadServiceTime;
}

void
LlrManager::SetSourceReloadBandwidth(uint64_t bytesPerSecond)
{
    m_sourceReloadBandwidth = bytesPerSecond;
}

uint64_t
LlrManager::GetSourceReloadBandwidth() const
{
    return m_sourceReloadBandwidth;
}

void
LlrManager::SetSourceReloadLatency(Time latency)
{
    m_sourceReloadLatency = latency;
}

Time
LlrManager::GetSourceReloadLatency() const
{
    return m_sourceReloadLatency;
}

void
LlrManager::SetOverflowPolicy(LlrOverflowPolicy policy)
{
    NS_LOG_FUNCTION(this << static_cast<int>(policy));
    m_overflowPolicy = policy;
}

LlrOverflowPolicy
LlrManager::GetOverflowPolicy() const
{
    return m_overflowPolicy;
}

void
LlrManager::Clear()
{
    for (auto& pair : m_retryBuffer)
    {
        if (!pair.second.timeoutEvent.IsExpired())
        {
            Simulator::Cancel(pair.second.timeoutEvent);
        }
    }
    m_retryBuffer.clear();
    m_sourceStore.clear();
    m_sourceReloadAvailable = Seconds(0);
}

void
LlrManager::OnRetryTimeout(uint32_t seqNum, uint16_t destRank, uint16_t flowId)
{
    NS_LOG_FUNCTION(this << seqNum << destRank << flowId);

    RetryKey key = MakeRetryKey(seqNum, destRank, flowId);
    auto it = m_retryBuffer.find(key);
    if (it != m_retryBuffer.end())
    {
        auto& entry = it->second;
        uint32_t retryCount = 0;
        auto sourceIt = m_sourceStore.find(key);
        if (sourceIt != m_sourceStore.end())
        {
            retryCount = sourceIt->second.retryCount;
        }
        NS_LOG_DEBUG("Retry timeout for seqNum " << seqNum
                    << " destRank=" << destRank
                    << " flowId=" << flowId
                    << " retryCount=" << retryCount);

        if (!m_timeoutCallback.IsNull())
        {
            m_timeoutCallback(seqNum, entry.destRank, flowId);
        }
    }
}

Time
LlrManager::ReserveSourceReload(uint32_t bytes)
{
    Time transferTime = Seconds(0);
    if (m_sourceReloadBandwidth > 0 && bytes > 0)
    {
        transferTime = Seconds(static_cast<double>(bytes) /
                               static_cast<double>(m_sourceReloadBandwidth));
    }
    Time serviceTime = m_sourceReloadLatency + transferTime;
    Time now = Simulator::Now();
    Time start = std::max(now, m_sourceReloadAvailable);
    m_sourceReloadAvailable = start + serviceTime;

    m_sourceReloadCount++;
    m_sourceReloadBytes += bytes;
    m_sourceReloadServiceTime += serviceTime;
    return m_sourceReloadAvailable - now;
}

void
LlrManager::RemoveOutstandingPacket(uint64_t key)
{
    auto bufferIt = m_retryBuffer.find(key);
    if (bufferIt != m_retryBuffer.end())
    {
        if (!bufferIt->second.timeoutEvent.IsExpired())
        {
            Simulator::Cancel(bufferIt->second.timeoutEvent);
        }
        m_retryBuffer.erase(bufferIt);
    }
    m_sourceStore.erase(key);
}

bool
LlrManager::EnforceBufferLimit()
{
    if (m_maxBufferSize == 0)
    {
        return true;
    }

    while (m_retryBuffer.size() >= m_maxBufferSize)
    {
        if (m_overflowPolicy == LlrOverflowPolicy::DROP_OLDEST)
        {
            // Find the entry with the smallest composite key
            // (lowest destRank, then flowId, then seqNum)
            uint64_t oldestKey = UINT64_MAX;
            for (const auto& pair : m_retryBuffer)
            {
                if (pair.first < oldestKey)
                {
                    oldestKey = pair.first;
                }
            }
            if (oldestKey == UINT64_MAX) break;

            auto it = m_retryBuffer.find(oldestKey);
            if (it != m_retryBuffer.end())
            {
                uint32_t evictedSeq = it->first & 0xFFFFFFFF;
                // Eviction drops the retry-buffer entry — the packet itself
                // is already on the wire and may still arrive successfully.
                // Do NOT fire PermanentLossCallback: that would tell the
                // receiver to skip a packet that is still in flight,
                // corrupting the collective. If the packet IS subsequently
                // lost and a retry request arrives, the sender reconstructs
                // it from the backing source and pays the configured reload
                // delay.
                if (!it->second.timeoutEvent.IsExpired())
                {
                    Simulator::Cancel(it->second.timeoutEvent);
                }
                m_overflowTrace(evictedSeq, m_retryBuffer.size());
                m_overflowCount++;
                m_retryBuffer.erase(it);
            }
        }
        else
        {
            // DROP_NEWEST shouldn't reach here (refused at StorePacket)
            break;
        }
    }
    return m_retryBuffer.size() < m_maxBufferSize;
}

} // namespace ns3
