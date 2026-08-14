/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Author: GPU Cluster Simulator Team
 *
 * Reorder Buffer for multi-path packet reordering
 */

#ifndef REORDER_BUFFER_H
#define REORDER_BUFFER_H

#include "fabric-header.h"
#include "ns3/object.h"
#include "ns3/packet.h"
#include "ns3/traced-callback.h"

#include <map>
#include <set>
#include <cstdint>

namespace ns3
{

/**
 * @ingroup gpu-cluster
 * @brief Buffer entry for reordered packets
 */
struct ReorderEntry
{
    uint32_t sequenceNumber;  ///< Sequence number of the packet
    Ptr<Packet> packet;       ///< The packet data
    FabricHeader header;      ///< The fabric header for delivery
    bool delivered;           ///< Whether this packet has been delivered
};

/**
 * @ingroup gpu-cluster
 * @brief Reorder Buffer for handling out-of-order packets from multi-path spraying
 *
 * This class implements:
 * - Buffering of out-of-order packets
 * - In-order delivery based on sequence numbers
 * - Duplicate detection
 * - Timeout-based delivery for stuck packets
 */
class ReorderBuffer : public Object
{
  public:
    /**
     * @brief Get the type ID.
     * @return the object TypeId
     */
    static TypeId GetTypeId();

    ReorderBuffer();
    ~ReorderBuffer() override;

    // Delete copy constructor and assignment operator
    ReorderBuffer(const ReorderBuffer&) = delete;
    ReorderBuffer& operator=(const ReorderBuffer&) = delete;

    /**
     * @brief Set the expected next sequence number
     * @param seqNum Initial expected sequence number
     */
    void SetExpectedSequence(uint32_t seqNum);

    /**
     * @brief Insert a packet into the reorder buffer
     * @param seqNum Sequence number of the packet
     * @param packet The packet to insert
     * @param header The fabric header associated with the packet
     * @return true if the packet was inserted (not a duplicate)
     */
    bool Insert(uint32_t seqNum, Ptr<Packet> packet, const FabricHeader& header = FabricHeader());

    /**
     * @brief Check if there are packets ready to deliver in order
     * @return true if packets are ready
     */
    bool HasReadyPackets() const;

    /**
     * @brief Get the next packet in order
     * @param packet Output: the packet to deliver
     * @param header Output: the fabric header associated with the packet
     * @return true if a packet was retrieved
     */
    bool GetNextPacket(Ptr<Packet>& packet, FabricHeader& header);

    /**
     * @brief Get the number of buffered packets
     * @return number of packets in buffer
     */
    uint32_t GetBufferSize() const;

    /**
     * @brief Get the next expected sequence number
     * @return expected sequence number
     */
    uint32_t GetExpectedSequence() const;

    /**
     * @brief Check if a sequence number is a duplicate
     * @param seqNum Sequence number to check
     * @return true if this is a duplicate
     */
    bool IsDuplicate(uint32_t seqNum) const;

    /**
     * @brief Set the maximum buffer size
     * @param size Maximum number of packets to buffer
     */
    void SetMaxBufferSize(uint32_t size);

    /**
     * @brief Mark a sequence number as permanently lost (unrecoverable gap)
     * @param seqNum The permanently lost sequence number
     * @param header The original packet's FabricHeader with metadata
     *
     * Subsequent HasReadyPackets/GetNextPacket calls skip over this gap.
     * When AdvancePastPermanentGaps() crosses the gap, the permanent-gap
     * callback fires with the stored header.
     */
    void MarkPermanentGap(uint32_t seqNum, const FabricHeader& header = FabricHeader());

    /**
     * @brief Clear all buffered packets
     */
    void Clear();

    /**
     * @brief Callback type for packet delivery
     */
    typedef Callback<void, Ptr<Packet>, uint32_t, FabricHeader> PacketDeliveryCallback;

    /**
     * @brief Callback type for permanent gap delivery
     *
     * Invoked when AdvancePastPermanentGaps() crosses a permanent gap.
     * The callback receives the original packet's FabricHeader with
     * metadata (effectiveDataSize, flowId, etc.) so the collective layer
     * can advance its progress for the permanently lost step.
     */
    typedef Callback<void, uint32_t, FabricHeader> PermanentGapCallback;

    /**
     * @brief Set callback for packet delivery
     * @param cb Callback to invoke when packets are ready for delivery
     */
    void SetPacketDeliveryCallback(PacketDeliveryCallback cb);

    /**
     * @brief Set callback for permanent gap delivery
     * @param cb Callback to invoke when a permanent gap is crossed in sequence order
     */
    void SetPermanentGapCallback(PermanentGapCallback cb);

    /**
     * @brief Deliver all ready packets through the callback
     */
    void DeliverReadyPackets();

    uint32_t GetReorderEventCount() const { return m_reorderEventCount; }
    uint32_t GetMaxOccupancy() const { return m_maxOccupancy; }

  private:
    void DoDispose() override;

    void AdvancePastPermanentGaps();

    std::map<uint32_t, ReorderEntry> m_buffer;  ///< Buffer indexed by sequence number
    std::set<uint32_t> m_permanentGaps;         ///< Permanently lost sequence numbers (skip in delivery)
    std::map<uint32_t, FabricHeader> m_permanentGapHeaders; ///< Original headers for permanent gaps
    uint32_t m_expectedSeq;                     ///< Next expected sequence number
    uint32_t m_maxBufferSize;                   ///< Maximum buffer capacity
    PacketDeliveryCallback m_deliveryCallback;  ///< Callback for packet delivery
    PermanentGapCallback m_permanentGapCallback; ///< Callback for permanent gap delivery

    // Traced sources
    TracedCallback<uint32_t, uint32_t> m_bufferSizeTrace; ///< Old size, new size
    TracedCallback<uint32_t> m_packetDeliveredTrace;      ///< Sequence number

    // Statistics counters
    uint32_t m_reorderEventCount{0};  ///< Number of out-of-order insertions
    uint32_t m_maxOccupancy{0};       ///< Maximum buffer occupancy observed
};

} // namespace ns3

#endif /* REORDER_BUFFER_H */
