/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Author: GPU Cluster Simulator Team
 */

#include "reorder-buffer.h"

#include "ns3/log.h"
#include "ns3/simulator.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("ReorderBuffer");

NS_OBJECT_ENSURE_REGISTERED(ReorderBuffer);

TypeId
ReorderBuffer::GetTypeId()
{
    static TypeId tid = TypeId("ns3::ReorderBuffer")
                            .SetParent<Object>()
                            .SetGroupName("GpuCluster")
                            .AddConstructor<ReorderBuffer>()
                            .AddTraceSource("BufferSize",
                                            "Number of packets in reorder buffer",
                                            MakeTraceSourceAccessor(&ReorderBuffer::m_bufferSizeTrace),
                                            "ns3::TracedValueCallback::Uint32")
                            .AddTraceSource("PacketDelivered",
                                            "A packet was delivered in order",
                                            MakeTraceSourceAccessor(&ReorderBuffer::m_packetDeliveredTrace),
                                            "ns3::TracedValueCallback::Uint32");
    return tid;
}

ReorderBuffer::ReorderBuffer()
    : m_expectedSeq(0),
      m_maxBufferSize(1024)  // Default buffer size
{
    NS_LOG_FUNCTION(this);
}

ReorderBuffer::~ReorderBuffer()
{
    NS_LOG_FUNCTION(this);
}

void
ReorderBuffer::DoDispose()
{
    NS_LOG_FUNCTION(this);
    Clear();
    Object::DoDispose();
}

void
ReorderBuffer::SetExpectedSequence(uint32_t seqNum)
{
    NS_LOG_FUNCTION(this << seqNum);
    m_expectedSeq = seqNum;
}

bool
ReorderBuffer::Insert(uint32_t seqNum, Ptr<Packet> packet, const FabricHeader& header)
{
    NS_LOG_FUNCTION(this << seqNum);

    // Check for duplicates
    if (IsDuplicate(seqNum))
    {
        NS_LOG_DEBUG("Duplicate packet detected: seq=" << seqNum);
        return false;
    }

    // Check buffer capacity
    if (m_buffer.size() >= m_maxBufferSize)
    {
        NS_LOG_WARN("Reorder buffer full, dropping packet seq=" << seqNum);
        return false;
    }

    NS_ASSERT_MSG(m_buffer.size() < m_maxBufferSize,
                  "ReorderBuffer::Insert: buffer overflow, size=" << m_buffer.size()
                  << " max=" << m_maxBufferSize);

    // Special case: packet is exactly what we expect
    if (seqNum == m_expectedSeq)
    {
        NS_LOG_DEBUG("Received expected packet seq=" << seqNum);
        ReorderEntry entry;
        entry.sequenceNumber = seqNum;
        entry.packet = packet->Copy();
        entry.header = header;
        entry.delivered = false;
        m_buffer[seqNum] = entry;

        DeliverReadyPackets();
        return true;
    }

    // Packet is in the future, buffer it
    if (seqNum > m_expectedSeq)
    {
        NS_LOG_DEBUG("Buffering out-of-order packet seq=" << seqNum
                     << " (expected=" << m_expectedSeq << ")");
        m_reorderEventCount++;
        ReorderEntry entry;
        entry.sequenceNumber = seqNum;
        entry.packet = packet->Copy();
        entry.header = header;
        entry.delivered = false;
        m_buffer[seqNum] = entry;
        if (static_cast<uint32_t>(m_buffer.size()) > m_maxOccupancy)
        {
            m_maxOccupancy = static_cast<uint32_t>(m_buffer.size());
        }
        return true;
    }

    // Packet is in the past (already delivered), ignore
    NS_LOG_DEBUG("Packet seq=" << seqNum << " already processed (expected=" << m_expectedSeq << ")");
    return false;
}

bool
ReorderBuffer::HasReadyPackets() const
{
    auto it = m_buffer.find(m_expectedSeq);
    return it != m_buffer.end() && !it->second.delivered;
}

bool
ReorderBuffer::GetNextPacket(Ptr<Packet>& packet, FabricHeader& header)
{
    auto it = m_buffer.find(m_expectedSeq);
    if (it == m_buffer.end() || it->second.delivered)
    {
        return false;
    }

    NS_ASSERT_MSG(!it->second.delivered,
                  "GetNextPacket: entry for seq=" << m_expectedSeq << " already delivered");

    packet = it->second.packet->Copy();
    header = it->second.header;
    it->second.delivered = true;
    m_packetDeliveredTrace(m_expectedSeq);
    m_expectedSeq++;

    // Clean up delivered entries and permanent gaps behind m_expectedSeq
    while (!m_permanentGaps.empty() && *m_permanentGaps.begin() < m_expectedSeq)
    {
        m_permanentGaps.erase(m_permanentGaps.begin());
    }
    while (!m_buffer.empty())
    {
        auto front = m_buffer.begin();
        if (front->second.delivered && front->first < m_expectedSeq)
        {
            m_buffer.erase(front);
        }
        else
        {
            break;
        }
    }

    return true;
}

uint32_t
ReorderBuffer::GetBufferSize() const
{
    return static_cast<uint32_t>(m_buffer.size());
}

uint32_t
ReorderBuffer::GetExpectedSequence() const
{
    return m_expectedSeq;
}

bool
ReorderBuffer::IsDuplicate(uint32_t seqNum) const
{
    // Already processed
    if (seqNum < m_expectedSeq)
    {
        return true;
    }

    // Already in buffer
    auto it = m_buffer.find(seqNum);
    return it != m_buffer.end();
}

void
ReorderBuffer::SetMaxBufferSize(uint32_t size)
{
    m_maxBufferSize = size;
}

void
ReorderBuffer::MarkPermanentGap(uint32_t seqNum, const FabricHeader& header)
{
    NS_LOG_FUNCTION(this << seqNum);
    m_permanentGaps.insert(seqNum);
    m_permanentGapHeaders[seqNum] = header;
    NS_LOG_DEBUG("Marked seqNum " << seqNum << " as permanently lost");
}

void
ReorderBuffer::AdvancePastPermanentGaps()
{
    while (m_permanentGaps.count(m_expectedSeq))
    {
        NS_LOG_DEBUG("Skipping permanent gap seqNum " << m_expectedSeq);
        // Fire permanent-gap callback with the stored header metadata
        auto hdrIt = m_permanentGapHeaders.find(m_expectedSeq);
        if (!m_permanentGapCallback.IsNull() && hdrIt != m_permanentGapHeaders.end())
        {
            m_permanentGapCallback(m_expectedSeq, hdrIt->second);
        }
        m_expectedSeq++;
    }
    // Clean up gaps and headers behind m_expectedSeq
    while (!m_permanentGaps.empty() && *m_permanentGaps.begin() < m_expectedSeq)
    {
        m_permanentGapHeaders.erase(*m_permanentGaps.begin());
        m_permanentGaps.erase(m_permanentGaps.begin());
    }
    // Also clean up header entries that may have been added without gap entries
    auto hdrIt = m_permanentGapHeaders.begin();
    while (hdrIt != m_permanentGapHeaders.end())
    {
        if (hdrIt->first < m_expectedSeq)
        {
            hdrIt = m_permanentGapHeaders.erase(hdrIt);
        }
        else
        {
            ++hdrIt;
        }
    }
}

void
ReorderBuffer::Clear()
{
    m_buffer.clear();
    m_permanentGaps.clear();
    m_permanentGapHeaders.clear();
}

void
ReorderBuffer::SetPacketDeliveryCallback(PacketDeliveryCallback cb)
{
    m_deliveryCallback = cb;
}

void
ReorderBuffer::SetPermanentGapCallback(PermanentGapCallback cb)
{
    m_permanentGapCallback = cb;
}

void
ReorderBuffer::DeliverReadyPackets()
{
    if (m_deliveryCallback.IsNull())
    {
        return;
    }

    uint32_t oldSize = m_buffer.size();

    // Advance past permanent gaps before delivering
    AdvancePastPermanentGaps();

    while (HasReadyPackets())
    {
        Ptr<Packet> packet;
        FabricHeader header;
        if (GetNextPacket(packet, header))
        {
            m_deliveryCallback(packet, m_expectedSeq - 1, header);
            // GetNextPacket incremented m_expectedSeq; skip any permanent gap that follows
            AdvancePastPermanentGaps();
        }
    }

    uint32_t newSize = m_buffer.size();
    if (oldSize != newSize)
    {
        m_bufferSizeTrace(oldSize, newSize);
    }
}

} // namespace ns3
