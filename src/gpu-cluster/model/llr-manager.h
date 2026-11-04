/*
 * SPDX-License-Identifier: GPL-2.0-only
 * llr-manager.h
 *
 * Link Layer Retry (LLR) Manager for reliable packet transmission.
 * Supports Go-Back-N and SACK retransmission modes.
 */

#ifndef LLR_MANAGER_H
#define LLR_MANAGER_H

#include "fabric-header.h"

#include "ns3/object.h"
#include "ns3/traced-callback.h"
#include "ns3/event-id.h"
#include "ns3/packet.h"
#include "ns3/nstime.h"
#include "ns3/callback.h"

#include <vector>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <queue>
#include <cstdint>

namespace ns3
{

/**
 * @ingroup gpu-cluster
 * @brief LLR retransmission mode
 */
enum class LlrMode : uint8_t
{
    GO_BACK_N = 0,  ///< Retransmit all packets from NACK point onward
    SACK = 1        ///< Retransmit only specifically NACKed packets
};

/**
 * @ingroup gpu-cluster
 * @brief Buffer overflow policy when retry buffer exceeds capacity
 */
enum class LlrOverflowPolicy : uint8_t
{
    DROP_OLDEST = 0,  ///< Drop oldest entries (lowest seqNum)
    DROP_NEWEST = 1   ///< Drop newest entries (highest seqNum), refuse store
};

/**
 * @ingroup gpu-cluster
 * @brief Link Layer Retry (LLR) Manager
 *
 * Tracks sent packets in a retry buffer per link. When a packet has a
 * simulated CRC error (from LinkDegradationModel or FEC uncorrectable),
 * the receiver sends RETRY_REQUEST back and the sender retransmits.
 *
 * Supports two modes:
 * - Go-Back-N: retransmit all packets from the NACK point onward
 * - SACK: retransmit only the specifically NACKed packets
 *
 * Configurable retry buffer size with overflow policy.
 * Retry timeout triggers retransmission when ACK is not received.
 */
class LlrManager : public Object
{
  public:
    struct Retransmission
    {
        Ptr<Packet> packet;
        Time readyDelay;
        bool sourceReload;
    };

    static TypeId GetTypeId();

    LlrManager();
    ~LlrManager() override;

    // --- Mode configuration ---

    void SetLlrMode(LlrMode mode);
    LlrMode GetLlrMode() const;

    // --- Buffer management ---

    /**
     * @brief Store a packet in the retry buffer for potential retransmission
     * @param seqNum Packet sequence number
     * @param packet The packet to store
     * @param destRank Destination rank
     * @return True if retained in the retry buffer. False means the source
     *         record remains available but retransmission requires a reload.
     */
    bool StorePacket(uint32_t seqNum, Ptr<Packet> packet, uint16_t destRank);

    /**
     * @brief (Re)arm the retry-timeout on a stored source entry for a packet
     *        that is only NOW reaching the wire.
     *
     * StorePacket arms the timer at enqueue time, but several gates can hold
     * a packet after StorePacket and before its first transmission: the
     * DCQCN/RTTCC per-flow pacer (shapingDelay schedules the NetDevice send
     * into the future), WFQ contention service time, and FEC encode latency.
     * A timeout shorter than that hold makes the retry budget expire for a
     * packet that was never lost — it degrades to permanent loss on a
     * loss-free link (codex round 12, finding 4). The egress sites call
     * this at the moment the packet actually leaves.
     */
    void ArmRetryTimeoutAtTransmission(uint32_t seqNum, uint16_t destRank,
                                       uint16_t flowId, uint8_t vcId = 0);

    /**
     * @brief Remove a packet from the retry buffer (individual ACK received)
     * @param seqNum Sequence number of the ACKed packet
     * @param destRank Destination rank of the ACKed packet
     * @param flowId Flow ID of the packet
     * @return true if an entry was actually removed (the ACK retired a live
     *         retry entry); false for a duplicate ACK whose entry is gone
     */
    bool RemovePacket(uint32_t seqNum, uint16_t destRank, uint16_t flowId, uint8_t vcId = 0);

    /**
     * @brief Peek the FabricHeader packet type of a live outstanding entry.
     *
     * Credit bookkeeping on ACK/permanent-loss needs to know whether the
     * ACKed sequence actually consumed a credit on send. NVLS/SHARP uploads
     * (ALLREDUCE/ALLGATHER) bypass credit, yet the switch's LLR ack for them
     * now travels the same ProcessAckPacket path — without the type check
     * the ack-side recovery would return a credit the send never drew and
     * grow the VC pool past its total.
     *
     * @param seqNum Sequence number of the stored packet
     * @param destRank Destination rank of the stored packet
     * @param flowId Flow ID of the stored packet
     * @param vcId Virtual channel of the stored packet
     * @param outType Set to the entry's FabricHeader packet type
     * @return true when a live entry was found (outType valid)
     */
    bool PeekOutstandingPacketType(uint32_t seqNum, uint16_t destRank,
                                   uint16_t flowId, uint8_t vcId,
                                   uint8_t& outType) const;

    /**
     * @brief Remove all packets up to a sequence number (cumulative ACK)
     * @param seqNum Highest sequence number acknowledged
     * @param destRank Destination rank of the packets
     * @param flowId Flow ID of the packets
     */
    void RemovePacketsUpTo(uint32_t seqNum, uint16_t destRank, uint16_t flowId, uint8_t vcId = 0);

    /**
     * @brief Remove specifically ACKed packets (SACK block)
     * @param ackedSeqNums Set of sequence numbers that have been received
     * @param destRank Destination rank of the packets
     * @param flowId Flow ID of the packets
     */
    void RemoveSackedPackets(const std::unordered_set<uint32_t>& ackedSeqNums, uint16_t destRank, uint16_t flowId, uint8_t vcId = 0);

    // --- Retransmission ---

    /**
     * @brief Handle a retry request from a peer (Go-Back-N mode)
     * @param seqNum Sequence number of the failed packet
     * @param sourceRank Rank that sent the retry request
     * @param flowId Flow ID of the failed packet
     * @param vcId Virtual channel of the failed packet (from the NACK header)
     * @return Retransmissions and their source-read delays
     */
    std::vector<Retransmission> HandleRetryRequest(uint32_t seqNum,
                                                   uint16_t sourceRank,
                                                   uint16_t flowId,
                                                   uint8_t vcId);

    /**
     * @brief Handle a SACK retry request from a peer
     * @param nackSeqNums Set of sequence numbers that need retransmission
     * @param sourceRank Rank that sent the SACK
     * @param flowId Flow ID of the packets
     * @param vcId Virtual channel of the packets (from the NACK header)
     * @return Retransmissions and their source-read delays
     */
    std::vector<Retransmission> HandleSackRequest(
        const std::unordered_set<uint32_t>& nackSeqNums,
        uint16_t sourceRank,
        uint16_t flowId,
        uint8_t vcId);

    // --- Retry limit ---

    bool IsRetryLimitExceeded(uint32_t seqNum) const;

    void SetRetryLimit(uint32_t limit);
    uint32_t GetRetryLimit() const;

    // --- Timeout ---

    void SetRetryTimeout(Time timeout);
    Time GetRetryTimeout() const;

    /**
     * @brief Set callback invoked when a retry timeout fires
     * The callback receives (seqNum, destRank, flowId) and should trigger retransmission
     */
    typedef Callback<void, uint32_t, uint16_t, uint16_t, uint8_t> TimeoutCallback;
    void SetTimeoutCallback(TimeoutCallback cb);

    // --- Buffer size ---

    void SetMaxBufferSize(uint32_t maxSize);
    uint32_t GetMaxBufferSize() const;
    uint32_t GetBufferSize() const;
    uint32_t GetPeakBufferSize() const;
    bool HasOutstandingPacket(uint32_t seqNum, uint16_t destRank, uint16_t flowId, uint8_t vcId = 0) const;
    uint64_t GetRetransmittedPackets() const;
    uint64_t GetOverflowCount() const;
    uint64_t GetPermanentLossCount() const;
    uint64_t GetRetryMissCount() const;
    uint64_t GetSourceReloadCount() const;
    uint64_t GetSourceReloadBytes() const;
    Time GetSourceReloadServiceTime() const;

    void SetSourceReloadBandwidth(uint64_t bytesPerSecond);
    uint64_t GetSourceReloadBandwidth() const;
    void SetSourceReloadLatency(Time latency);
    Time GetSourceReloadLatency() const;

    void SetOverflowPolicy(LlrOverflowPolicy policy);
    LlrOverflowPolicy GetOverflowPolicy() const;

    /**
     * @brief Clear the retry buffer
     */
    void Clear();

    // Callback for permanent delivery failure (retry count exceeded)
    // Passes the full FabricHeader from the stored original packet so the
    // collective layer can access effectiveDataSize, flowId, etc.
    typedef Callback<void, FabricHeader> PermanentLossCallback;
    void SetPermanentLossCallback(PermanentLossCallback cb);

    /**
     * @brief Sever every endpoint-binding and cancel every armed timer.
     *
     * Called by the owning FabricEndpoint::DoDispose BEFORE it drops its
     * m_llrManager reference. The timeout and permanent-loss callbacks hold
     * the endpoint's raw `this`, and the per-entry retry timers can outlive
     * the endpoint when an external holder keeps the manager alive via
     * GetLlrManager() — a firing timer then invokes OnLlrRetryTimeout on
     * freed endpoint memory (codex round 17, finding 3). Nulling the
     * callbacks makes a late OnRetryTimeout a no-op; Clear() cancels the
     * timers outright.
     *
     * Identity-detach discipline (codex round 31, finding 1): a disposing
     * endpoint passes its OWN callback bindings as owners and only severs
     * the ones it installed. When a successor endpoint has already re-bound
     * either callback, the manager belongs to the successor: the disposing
     * endpoint detaches nothing, so the successor's bindings and — crucially
     * — its stored retry entries (Clear() drops them) survive. Null owners
     * (the default) mean the unconditional legacy teardown.
     */
    void DetachFromEndpoint(TimeoutCallback timeoutOwner = TimeoutCallback(),
                            PermanentLossCallback lossOwner = PermanentLossCallback());

    // Traced callbacks
    typedef TracedCallback<uint32_t, uint16_t> RetryTracedCallback;
    typedef TracedCallback<uint32_t, uint16_t> AckTracedCallback;
    typedef TracedCallback<uint32_t, uint32_t> OverflowTracedCallback; ///< seqNum, bufferSize
    typedef TracedCallback<uint32_t, uint16_t> PermanentLossTracedCallback; ///< seqNum, destRank

  private:
    void DoDispose() override;

    struct RetryEntry
    {
        Ptr<Packet> packet;
        uint16_t destRank;
    };

    struct SourceEntry
    {
        Ptr<Packet> packet;
        uint16_t destRank;
        uint32_t retryCount;
        // True once the packet's retry-timeout countdown has started (first
        // wire transmission, or a NACK/timeout retransmission). A fresh send
        // held by the egress pacer/contention/FEC gates has NOT started its
        // countdown — starting it at enqueue time expiring the retry budget
        // for a packet that was never lost (codex round 12, finding 4).
        bool wireArmed{false};
        // The retry-timeout timer lives on the SOURCE entry, not the
        // retry-buffer entry: the source store is the packet's lifetime
        // (until ACK), while a retry-buffer copy can be evicted by
        // EnforceBufferLimit or refused outright by DROP_NEWEST -- either
        // would strand an un-ACKed packet with no timeout backstop.
        EventId timeoutEvent;
    };

    /**
     * @brief Handle retry timeout for a specific packet
     */
    void OnRetryTimeout(uint32_t seqNum, uint16_t destRank, uint16_t flowId, uint8_t vcId = 0);

    /**
     * @brief Enforce buffer size limit by evicting entries
     * @return True if space was made (or was available), false if couldn't store
     */
    bool EnforceBufferLimit();
    Time ReserveSourceReload(uint32_t bytes);

    // Retry buffer key: the FULL (destRank, vcId, flowId, seqNum) tuple.
    // Previously packed into a uint64 as (destRank << 56) | (vc << 48) |
    // (flow << 32) | seq — but destRank is 16 bits, so only its low 8 bits
    // survived the shift and destinations 1 and 257 collided. The struct
    // carries every field at full width.
    struct RetryKey
    {
        uint32_t seqNum;
        uint16_t destRank;
        uint16_t flowId;
        uint8_t vcId;
        bool operator==(const RetryKey& o) const
        {
            return seqNum == o.seqNum && destRank == o.destRank
                && flowId == o.flowId && vcId == o.vcId;
        }
    };
    static RetryKey MakeRetryKey(uint32_t seqNum, uint16_t destRank, uint16_t flowId,
                                 uint8_t vcId = 0)
    {
        return RetryKey{seqNum, destRank, flowId, vcId};
    }
    struct RetryKeyHash
    {
        size_t operator()(const RetryKey& k) const
        {
            // seqNum is the discriminating field for entries that share a
            // (dest, vc, flow); mix it with the flow identity.
            return std::hash<uint64_t>()(
                (static_cast<uint64_t>(k.destRank) << 48)
                ^ (static_cast<uint64_t>(k.vcId) << 40)
                ^ (static_cast<uint64_t>(k.flowId) << 24)
                ^ k.seqNum);
        }
    };
    std::unordered_map<RetryKey, RetryEntry, RetryKeyHash> m_retryBuffer;
    std::unordered_map<RetryKey, SourceEntry, RetryKeyHash> m_sourceStore;
    void RemoveOutstandingPacket(const RetryKey& key);
    uint32_t m_retryLimit;
    Time m_retryTimeout;
    LlrMode m_llrMode;
    uint32_t m_maxBufferSize;        ///< 0 = unlimited
    LlrOverflowPolicy m_overflowPolicy;
    TimeoutCallback m_timeoutCallback;
    PermanentLossCallback m_permanentLossCallback;
    uint32_t m_peakBufferSize;
    uint64_t m_retransmittedPackets;
    uint64_t m_overflowCount;
    uint64_t m_permanentLossCount;
    uint64_t m_retryMissCount;
    uint64_t m_sourceReloadCount;
    uint64_t m_sourceReloadBytes;
    Time m_sourceReloadServiceTime;
    uint64_t m_sourceReloadBandwidth;
    Time m_sourceReloadLatency;
    Time m_sourceReloadAvailable;

    TracedCallback<uint32_t, uint16_t> m_retryTrace;
    TracedCallback<uint32_t, uint16_t> m_ackTrace;
    TracedCallback<uint32_t, uint32_t> m_overflowTrace;
    TracedCallback<uint32_t, uint16_t> m_permanentLossTrace;
};

} // namespace ns3

#endif // LLR_MANAGER_H
