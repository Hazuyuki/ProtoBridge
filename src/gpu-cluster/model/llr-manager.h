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
     * @brief Remove a packet from the retry buffer (individual ACK received)
     * @param seqNum Sequence number of the ACKed packet
     * @param destRank Destination rank of the ACKed packet
     * @param flowId Flow ID of the packet
     */
    void RemovePacket(uint32_t seqNum, uint16_t destRank, uint16_t flowId);

    /**
     * @brief Remove all packets up to a sequence number (cumulative ACK)
     * @param seqNum Highest sequence number acknowledged
     * @param destRank Destination rank of the packets
     * @param flowId Flow ID of the packets
     */
    void RemovePacketsUpTo(uint32_t seqNum, uint16_t destRank, uint16_t flowId);

    /**
     * @brief Remove specifically ACKed packets (SACK block)
     * @param ackedSeqNums Set of sequence numbers that have been received
     * @param destRank Destination rank of the packets
     * @param flowId Flow ID of the packets
     */
    void RemoveSackedPackets(const std::unordered_set<uint32_t>& ackedSeqNums, uint16_t destRank, uint16_t flowId);

    // --- Retransmission ---

    /**
     * @brief Handle a retry request from a peer (Go-Back-N mode)
     * @param seqNum Sequence number of the failed packet
     * @param sourceRank Rank that sent the retry request
     * @param flowId Flow ID of the failed packet
     * @return Retransmissions and their source-read delays
     */
    std::vector<Retransmission> HandleRetryRequest(uint32_t seqNum,
                                                   uint16_t sourceRank,
                                                   uint16_t flowId);

    /**
     * @brief Handle a SACK retry request from a peer
     * @param nackSeqNums Set of sequence numbers that need retransmission
     * @param sourceRank Rank that sent the SACK
     * @param flowId Flow ID of the packets
     * @return Retransmissions and their source-read delays
     */
    std::vector<Retransmission> HandleSackRequest(
        const std::unordered_set<uint32_t>& nackSeqNums,
        uint16_t sourceRank,
        uint16_t flowId);

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
    typedef Callback<void, uint32_t, uint16_t, uint16_t> TimeoutCallback;
    void SetTimeoutCallback(TimeoutCallback cb);

    // --- Buffer size ---

    void SetMaxBufferSize(uint32_t maxSize);
    uint32_t GetMaxBufferSize() const;
    uint32_t GetBufferSize() const;
    uint32_t GetPeakBufferSize() const;
    bool HasOutstandingPacket(uint32_t seqNum, uint16_t destRank, uint16_t flowId) const;
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
        EventId timeoutEvent;
    };

    struct SourceEntry
    {
        Ptr<Packet> packet;
        uint16_t destRank;
        uint32_t retryCount;
    };

    /**
     * @brief Handle retry timeout for a specific packet
     */
    void OnRetryTimeout(uint32_t seqNum, uint16_t destRank, uint16_t flowId);

    /**
     * @brief Enforce buffer size limit by evicting entries
     * @return True if space was made (or was available), false if couldn't store
     */
    bool EnforceBufferLimit();
    Time ReserveSourceReload(uint32_t bytes);
    void RemoveOutstandingPacket(uint64_t key);

    // Retry buffer key: (destRank << 48) | (flowId << 32) | seqNum
    // This prevents collisions between different flows/destinations that produce the same seqNum.
    typedef uint64_t RetryKey;
    static RetryKey MakeRetryKey(uint32_t seqNum, uint16_t destRank, uint16_t flowId)
    {
        return (static_cast<uint64_t>(destRank) << 48) | (static_cast<uint64_t>(flowId) << 32) | seqNum;
    }
    std::unordered_map<uint64_t, RetryEntry> m_retryBuffer;
    std::unordered_map<uint64_t, SourceEntry> m_sourceStore;
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
