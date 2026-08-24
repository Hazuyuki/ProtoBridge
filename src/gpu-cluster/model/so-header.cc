/*
 * SPDX-License-Identifier: GPL-2.0-only
 * so-header.cc
 *
 * Implementation of the scale-out (RoCEv2) protocol header. Mirrors the
 * FabricHeader byte-ordering convention (host uint8 flags + network-order
 * uint32 fields) so the ECN bit and per-segment framing round-trip cleanly.
 */
#include "so-header.h"

#include "ns3/log.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("SoHeader");
NS_OBJECT_ENSURE_REGISTERED(SoHeader);

TypeId
SoHeader::GetTypeId()
{
    static TypeId tid = TypeId("ns3::SoHeader")
                            .SetParent<Header>()
                            .SetGroupName("GpuCluster")
                            .AddConstructor<SoHeader>();
    return tid;
}

SoHeader::SoHeader()
    : m_protocolId(0),
      m_flags(0),
      m_dataSize(0),
      m_chunkIndex(0)
{
}

SoHeader::~SoHeader() = default;

TypeId
SoHeader::GetInstanceTypeId() const
{
    return GetTypeId();
}

uint32_t
SoHeader::GetSerializedSize() const
{
    return 10; // 1 (protocolId) + 1 (flags) + 4 (dataSize) + 4 (chunkIndex)
}

void
SoHeader::Serialize(Buffer::Iterator start) const
{
    Buffer::Iterator i = start;
    i.WriteU8(m_protocolId);
    i.WriteU8(m_flags);
    i.WriteHtonU32(m_dataSize);
    i.WriteHtonU32(m_chunkIndex);
}

uint32_t
SoHeader::Deserialize(Buffer::Iterator start)
{
    Buffer::Iterator i = start;
    m_protocolId = i.ReadU8();
    m_flags = i.ReadU8();
    m_dataSize = i.ReadNtohU32();
    m_chunkIndex = i.ReadNtohU32();
    return GetSerializedSize();
}

void
SoHeader::Print(std::ostream& os) const
{
    os << "SoHeader(proto=" << static_cast<int>(m_protocolId) << ",ecn="
       << (GetEcn() ? 1 : 0) << ",size=" << m_dataSize << ",idx=" << m_chunkIndex
       << ")";
}

void
SoHeader::SetEcn(bool v)
{
    if (v)
    {
        m_flags |= ECN_FLAG;
    }
    else
    {
        m_flags &= static_cast<uint8_t>(~ECN_FLAG);
    }
}

bool
SoHeader::PeekEcn(Ptr<const Packet> packet)
{
    if (!packet)
    {
        return false;
    }
    SoHeader h;
    packet->PeekHeader(h);
    return h.GetEcn();
}

} // namespace ns3
