/*
 * SPDX-License-Identifier: GPL-2.0-only
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
#include <limits>
#include <utility>

#include "ns3/time-bounds.h"

namespace ns3
{

namespace
{

// Round 73 (codex finding 3): a degenerate far-future RetryTimeout
// (Time::Max(), or any value within RetryTimeout of Time::Max()) passed
// directly to Simulator::Schedule wraps inside DefaultSimulatorImpl's
// `delay + TimeStep(now)` — at tick 7 the event lands at
// -9223372036854775802 and fires immediately. DefaultSimulatorImpl also
// EXECUTES events scheduled at absolute Time::Max(), so "clamp to
// Time::Max()" is not "never": the timeout callback would retransmit at
// the end of time. A stored packet whose configured backstop can never
// fire within the representable horizon has no timer at all — the entry
// survives until an ACK, a NACK (which re-arms a plain timeout), or
// teardown. Same policy as the round-73 memory-response fix
// (FabricEndpoint::ProcessMemoryPacket): Time::Max() means "drop the
// deferred action", not "run it at Time::Max()".
bool
IsUnrepresentableTimeout(Time timeout)
{
    return !TimeBounds::CanSchedule(timeout);
}

// Saturating add for Time (raw-tick) ledgers: a sum that would pass
// INT64_MAX clamps to Time::Max() instead of wrapping negative (round 74
// codex finding 3 — the source-reload service/availability/cumulative
// ledgers and the returned readyDelay otherwise wrap and the caller's
// readyDelay > 0 test fails open, bypassing the reload hold).
Time
SaturatedAddTime(Time a, Time b)
{
    return TimeBounds::SaturatingAdd(a, b);
}

} // unnamed namespace

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
                      MakeTimeChecker(Time(0)))
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
                      MakeTimeChecker(Time(0)))
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
    uint8_t vcId = hdr.GetVirtualChannel();
    RetryKey key = MakeRetryKey(seqNum, destRank, flowId, vcId);

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

    // A FIRST store (fresh send) does NOT arm the timeout here: the packet
    // has not reached the wire yet, and the DCQCN/RTTCC pacer, WFQ service
    // time, or FEC encode latency can hold it past a short timeout — the
    // retry budget would expire for a packet that was never lost (codex
    // round 12, finding 4). The wire egress arms it instead
    // (ArmRetryTimeoutAtTransmission via FabricEndpoint::ArmRetryTimeoutOnWire).
    // A RE-store of an already-timed-out packet (the sourceReload refresh in
    // SendRetransmission) keeps its timer: that packet already lost its
    // first copy, so the backstop must stay armed.
    sourceIt = m_sourceStore.find(key);
    if (sourceIt->second.timeoutEvent.IsPending())
    {
        // Existing (pending) timer: leave its deadline alone — it was armed
        // relative to an earlier wire event and may already be shorter than
        // this re-store implies.
    }
    else if (!sourceIt->second.wireArmed)
    {
        // Fresh send awaiting its first transmission: no timer yet.
    }
    // A re-store after a timeout fired (event expired, wireArmed true) is a
    // reload refresh: re-arm the plain timeout so the un-ACKed reload keeps
    // its backstop. A degenerate far-future timeout gets no timer: it could
    // only fire at a wrapped (already-past) tick or at Time::Max() itself,
    // which DefaultSimulatorImpl executes (see IsUnrepresentableTimeout).
    else if (m_retryTimeout > Time(0) && !IsUnrepresentableTimeout(m_retryTimeout))
    {
        sourceIt->second.timeoutEvent = TimeBounds::Schedule(
            m_retryTimeout, &LlrManager::OnRetryTimeout,
            this, seqNum, destRank, flowId, vcId);
    }

    // Remove any existing entry with the same composite key
    auto it = m_retryBuffer.find(key);
    if (it != m_retryBuffer.end())
    {
        m_retryBuffer.erase(it);
    }

    // Enforce buffer limit
    if (m_maxBufferSize > 0 && m_retryBuffer.size() >= m_maxBufferSize)
    {
        if (m_overflowPolicy == LlrOverflowPolicy::DROP_NEWEST)
        {
            // Refuse only the fast copy; the source record (with its timeout)
            // remains, so the packet is still recoverable on NACK/timeout.
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

    m_retryBuffer[key] = entry;
    m_peakBufferSize = std::max(m_peakBufferSize,
                                static_cast<uint32_t>(m_retryBuffer.size()));
    return true;
}

void
LlrManager::ArmRetryTimeoutAtTransmission(uint32_t seqNum, uint16_t destRank,
                                          uint16_t flowId, uint8_t vcId)
{
    NS_LOG_FUNCTION(this << seqNum << destRank << flowId << static_cast<int>(vcId));
    if (m_retryTimeout <= Time(0))
    {
        return;
    }
    // A degenerate far-future timeout gets no timer (see
    // IsUnrepresentableTimeout): nothing reachable can arm one.
    if (IsUnrepresentableTimeout(m_retryTimeout))
    {
        return;
    }
    RetryKey key = MakeRetryKey(seqNum, destRank, flowId, vcId);
    auto sourceIt = m_sourceStore.find(key);
    if (sourceIt == m_sourceStore.end())
    {
        return; // already ACKed (or never stored): no timer to re-arm
    }
    // Only EXTEND, never shorten: a timer legitimately armed for an earlier
    // NACK-driven retransmission (readyDelay + timeout) keeps its deadline
    // when the wire send happens to land after it. Compare against the
    // pending event's deadline when one is armed.
    Time deadline = Simulator::Now() + m_retryTimeout;
    if (!sourceIt->second.timeoutEvent.IsExpired())
    {
        Time pending = Simulator::GetDelayLeft(sourceIt->second.timeoutEvent);
        if (pending + Simulator::Now() >= deadline)
        {
            sourceIt->second.wireArmed = true;
            return;
        }
        Simulator::Cancel(sourceIt->second.timeoutEvent);
    }
    sourceIt->second.wireArmed = true;
    sourceIt->second.timeoutEvent = TimeBounds::Schedule(
        m_retryTimeout, &LlrManager::OnRetryTimeout,
        this, seqNum, destRank, flowId, vcId);
}

bool
LlrManager::RemovePacket(uint32_t seqNum, uint16_t destRank, uint16_t flowId, uint8_t vcId)
{
    NS_LOG_FUNCTION(this << seqNum << destRank << flowId);

    RetryKey key = MakeRetryKey(seqNum, destRank, flowId, vcId);
    const bool hadEntry = m_sourceStore.find(key) != m_sourceStore.end();
    if (hadEntry)
    {
        m_ackTrace(seqNum, destRank);
    }
    RemoveOutstandingPacket(key);
    return hadEntry;
}

bool
LlrManager::PeekOutstandingPacketType(uint32_t seqNum, uint16_t destRank,
                                       uint16_t flowId, uint8_t vcId,
                                       uint8_t& outType) const
{
    RetryKey key = MakeRetryKey(seqNum, destRank, flowId, vcId);
    auto it = m_sourceStore.find(key);
    if (it == m_sourceStore.end() || !it->second.packet)
    {
        return false;
    }
    FabricHeader hdr;
    if (!it->second.packet->PeekHeader(hdr))
    {
        return false;
    }
    outType = static_cast<uint8_t>(hdr.GetPacketType());
    return true;
}

void
LlrManager::RemovePacketsUpTo(uint32_t seqNum, uint16_t destRank, uint16_t flowId, uint8_t vcId)
{
    NS_LOG_FUNCTION(this << seqNum << destRank << flowId);

    std::vector<RetryKey> ackedKeys;
    for (const auto& pair : m_sourceStore)
    {
        const RetryKey& entryKey = pair.first;
        if (entryKey.destRank == destRank && entryKey.flowId == flowId &&
            entryKey.vcId == vcId && entryKey.seqNum <= seqNum)
        {
            ackedKeys.push_back(entryKey);
        }
    }
    for (RetryKey key : ackedKeys)
    {
        m_ackTrace(key.seqNum, destRank);
        RemoveOutstandingPacket(key);
    }
}

void
LlrManager::RemoveSackedPackets(const std::unordered_set<uint32_t>& ackedSeqNums, uint16_t destRank, uint16_t flowId, uint8_t vcId)
{
    NS_LOG_FUNCTION(this << ackedSeqNums.size() << destRank << flowId);

    for (uint32_t seqNum : ackedSeqNums)
    {
        RetryKey key = MakeRetryKey(seqNum, destRank, flowId, vcId);
        if (m_sourceStore.find(key) != m_sourceStore.end())
        {
            m_ackTrace(seqNum, destRank);
        }
        RemoveOutstandingPacket(key);
    }
}

std::vector<LlrManager::Retransmission>
LlrManager::HandleRetryRequest(uint32_t seqNum, uint16_t sourceRank, uint16_t flowId, uint8_t vcId)
{
    NS_LOG_FUNCTION(this << seqNum << sourceRank << flowId);

    std::vector<Retransmission> retransmissions;

    // Go-Back-N: retransmit all packets from seqNum onwards in sequence order
    // Match by destRank == sourceRank AND flowId AND vcId for correct flow
    // isolation — the NACK names one VC's stream; a same-seq packet on a
    // DIFFERENT VC must not be retransmitted (nor burn its retry budget).
    std::vector<RetryKey> keys;
    for (const auto& pair : m_sourceStore)
    {
        if (pair.first.flowId == flowId && pair.first.seqNum >= seqNum
            && pair.second.destRank == sourceRank
            && pair.first.vcId == vcId)
        {
            keys.push_back(pair.first);
        }
    }
    // Sequence order within the flow (GBN retransmits a contiguous suffix).
    std::sort(keys.begin(), keys.end(),
              [](const RetryKey& a, const RetryKey& b) {
                  return a.seqNum < b.seqNum;
              });

    Time orderingDelay = Seconds(0);
    for (RetryKey key : keys)
    {
        auto sourceIt = m_sourceStore.find(key);
        if (sourceIt == m_sourceStore.end())
        {
            continue;
        }

        uint32_t sn = key.seqNum;
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

        // The retransmission's retry countdown starts when it reaches the
        // WIRE, not when it is queued (codex round 13, finding 1). The egress
        // pacer/WFQ/FEC gates hold retransmissions exactly as they hold fresh
        // sends; arming at readyDelay + timeout let the budget expire while
        // the repair was still queued (64-B LL packet, 0.1-Gbps pacer, 1-us
        // timeout, one retry: permanent loss declared at ~2 us while the
        // repair transmitted at 13.36 us). Drop any pending timer and clear
        // wireArmed; the wire egress (FabricEndpoint::ArmRetryTimeoutOnWire)
        // re-arms at the actual transmission.
        sourceIt->second.wireArmed = false;
        if (!sourceIt->second.timeoutEvent.IsExpired())
        {
            Simulator::Cancel(sourceIt->second.timeoutEvent);
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
                               uint16_t sourceRank, uint16_t flowId, uint8_t vcId)
{
    NS_LOG_FUNCTION(this << nackSeqNums.size() << sourceRank << flowId);

    std::vector<Retransmission> retransmissions;

    std::vector<uint32_t> orderedSeqNums(nackSeqNums.begin(), nackSeqNums.end());
    std::sort(orderedSeqNums.begin(), orderedSeqNums.end());
    for (uint32_t seqNum : orderedSeqNums)
    {
        // The NACK header carries the original packet's VC (SendNack echoes
        // dataHeader's fields), so look up the exact (seq, dest, flow, vc)
        // entry directly. The any-VC probe loop this replaces could match the
        // WRONG VC's entry when two VCs share a flow and seq.
        RetryKey key = MakeRetryKey(seqNum, sourceRank, flowId, vcId);
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

        // The retransmission's retry countdown starts when it reaches the
        // WIRE, not when it is queued (codex round 13, finding 1) — the same
        // rule the GBN path now follows. Drop any pending timer and clear
        // wireArmed; the wire egress re-arms at the actual transmission.
        sourceIt->second.wireArmed = false;
        if (!sourceIt->second.timeoutEvent.IsExpired())
        {
            Simulator::Cancel(sourceIt->second.timeoutEvent);
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
        if (pair.first.seqNum == seqNum)
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
LlrManager::DetachFromEndpoint(TimeoutCallback timeoutOwner,
                               PermanentLossCallback lossOwner)
{
    NS_LOG_FUNCTION(this);
    // Identity-detach (codex round 31, finding 1): a disposing endpoint
    // passes its own bindings; sever only those. If a successor endpoint
    // has re-bound either callback, the manager is now the successor's —
    // Clear() would drop the stores, taking the successor's outstanding
    // packets with it. Null owners mean the unconditional legacy teardown.
    bool successorTookOver = false;
    if (!timeoutOwner.IsNull())
    {
        if (!m_timeoutCallback.IsNull() && !m_timeoutCallback.IsEqual(timeoutOwner))
        {
            successorTookOver = true; // re-bound by someone else: leave it
        }
        else
        {
            m_timeoutCallback = MakeNullCallback<void, uint32_t, uint16_t, uint16_t, uint8_t>();
        }
    }
    if (!lossOwner.IsNull())
    {
        if (!m_permanentLossCallback.IsNull() &&
            !m_permanentLossCallback.IsEqual(lossOwner))
        {
            successorTookOver = true; // re-bound by someone else: leave it
        }
        else
        {
            m_permanentLossCallback = MakeNullCallback<void, FabricHeader>();
        }
    }
    if (successorTookOver)
    {
        // The successor owns the manager's lifecycle now — its own dispose
        // will tear down. Touch the stores and timers nothing.
        return;
    }
    // Null callbacks first so nothing NEW can bind the endpoint, then
    // cancel every armed retry timer (Clear() also drops the stores, which
    // the endpoint's disappearance makes unrecoverable anyway — no ACK can
    // arrive to purge them, and no retransmission can be dispatched).
    if (timeoutOwner.IsNull())
    {
        m_timeoutCallback = MakeNullCallback<void, uint32_t, uint16_t, uint16_t, uint8_t>();
    }
    if (lossOwner.IsNull())
    {
        m_permanentLossCallback = MakeNullCallback<void, FabricHeader>();
    }
    Clear();
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
LlrManager::HasOutstandingPacket(uint32_t seqNum, uint16_t destRank, uint16_t flowId, uint8_t vcId) const
{
    return m_sourceStore.find(MakeRetryKey(seqNum, destRank, flowId, vcId)) != m_sourceStore.end();
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
    NS_ABORT_MSG_IF(latency.IsStrictlyNegative(), "Latency must be nonnegative");
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
    for (auto& pair : m_sourceStore)
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
LlrManager::OnRetryTimeout(uint32_t seqNum, uint16_t destRank, uint16_t flowId, uint8_t vcId)
{
    NS_LOG_FUNCTION(this << seqNum << destRank << flowId);

    RetryKey key = MakeRetryKey(seqNum, destRank, flowId, vcId);
    // The timer lives on the source entry, so it fires for any un-ACKed
    // packet -- including ones whose retry-buffer copy was evicted or
    // refused (the timeout is their only recovery path).
    auto sourceIt = m_sourceStore.find(key);
    if (sourceIt != m_sourceStore.end())
    {
        NS_LOG_DEBUG("Retry timeout for seqNum " << seqNum
                    << " destRank=" << destRank
                    << " flowId=" << flowId
                    << " retryCount=" << sourceIt->second.retryCount);

        if (!m_timeoutCallback.IsNull())
        {
            m_timeoutCallback(seqNum, sourceIt->second.destRank, flowId, key.vcId);
        }
    }
}

Time
LlrManager::ReserveSourceReload(uint32_t bytes)
{
    Time transferTime = Seconds(0);
    if (m_sourceReloadBandwidth > 0 && bytes > 0)
    {
        transferTime = TimeBounds::FromRate(static_cast<double>(bytes),
                                                static_cast<double>(m_sourceReloadBandwidth));
    }
    // Round 74 (codex finding 3): saturate every time addition so the
    // ledgers and the returned readyDelay cannot wrap (probe: 64 B evicted
    // entry, SourceReloadLatency = Time::Max(), bandwidth = 1e9 B/s at tick
    // 7 -> service wraps to -9223372036854775745 ticks, the caller's
    // readyDelay > 0 test fails open and the retransmission fires
    // immediately instead of waiting for the reload service).
    Time serviceTime = SaturatedAddTime(m_sourceReloadLatency, transferTime);
    Time now = Simulator::Now();
    Time start = std::max(now, m_sourceReloadAvailable);
    m_sourceReloadAvailable = SaturatedAddTime(start, serviceTime);

    m_sourceReloadCount++;
    m_sourceReloadBytes += bytes;
    m_sourceReloadServiceTime = SaturatedAddTime(m_sourceReloadServiceTime, serviceTime);
    // readyDelay below is relative to now and cannot be negative: start is
    // max(now, available) >= now and SaturatedAddTime pins every sum at
    // Time::Max(), so available >= now always.
    return m_sourceReloadAvailable - now;
}

void
LlrManager::RemoveOutstandingPacket(const RetryKey& key)
{
    auto sourceIt = m_sourceStore.find(key);
    if (sourceIt != m_sourceStore.end())
    {
        if (!sourceIt->second.timeoutEvent.IsExpired())
        {
            Simulator::Cancel(sourceIt->second.timeoutEvent);
        }
        m_sourceStore.erase(sourceIt);
    }
    m_retryBuffer.erase(key);
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
            // Evict the lowest sequence number (the oldest chunk on the wire
            // under per-flow sequential seqNums).
            auto oldestIt = m_retryBuffer.end();
            for (auto it = m_retryBuffer.begin(); it != m_retryBuffer.end(); ++it)
            {
                if (oldestIt == m_retryBuffer.end()
                    || it->first.seqNum < oldestIt->first.seqNum)
                {
                    oldestIt = it;
                }
            }
            if (oldestIt == m_retryBuffer.end()) break;

            auto it = oldestIt;
            {
                uint32_t evictedSeq = it->first.seqNum;
                // Eviction drops ONLY the retry-buffer fast copy — the packet
                // itself is already on the wire and may still arrive
                // successfully, and its SOURCE-entry timeout stays armed as
                // the recovery backstop (canceling it here would strand an
                // un-ACKed packet whose NACK was lost).
                // Do NOT fire PermanentLossCallback: that would tell the
                // receiver to skip a packet that is still in flight,
                // corrupting the collective. If the packet IS subsequently
                // lost and a retry request arrives, the sender reconstructs
                // it from the backing source and pays the configured reload
                // delay.
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
