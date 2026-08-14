/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Author: GPU Cluster Simulator Team
 */

#include "fabric-header.h"

#include "ns3/log.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("FabricHeader");

NS_OBJECT_ENSURE_REGISTERED(FabricHeader);

TypeId
FabricHeader::GetTypeId()
{
    static TypeId tid = TypeId("ns3::FabricHeader")
                            .SetParent<Header>()
                            .SetGroupName("GpuCluster")
                            .AddConstructor<FabricHeader>();
    return tid;
}

FabricHeader::FabricHeader()
    : m_packetType(static_cast<uint8_t>(FabricPacketType::DATA)),
      m_fabricType(static_cast<uint8_t>(FabricType::NVLINK)),
      m_virtualChannel(0),
      m_virtualLane(0),
      m_memoryAccessType(static_cast<uint8_t>(MemoryAccessType::DMA_BULK)),
      m_flowId(0),
      m_sequenceNumber(0),
      m_sourceRank(0),
      m_destRank(0),
      m_payloadSize(0),
      m_creditCount(0),
      m_protocol(0),
      m_ttl(64),
      m_effectiveDataSize(0)
{
    std::memset(m_srcMac, 0, 6);
    std::memset(m_dstMac, 0, 6);
}

FabricHeader::~FabricHeader()
{
}

FabricHeader::FabricHeader(const FabricHeader& other)
    : m_packetType(other.m_packetType),
      m_fabricType(other.m_fabricType),
      m_virtualChannel(other.m_virtualChannel),
      m_virtualLane(other.m_virtualLane),
      m_memoryAccessType(other.m_memoryAccessType),
      m_flowId(other.m_flowId),
      m_sequenceNumber(other.m_sequenceNumber),
      m_sourceRank(other.m_sourceRank),
      m_destRank(other.m_destRank),
      m_payloadSize(other.m_payloadSize),
      m_creditCount(other.m_creditCount),
      m_protocol(other.m_protocol),
      m_ttl(other.m_ttl),
      m_effectiveDataSize(other.m_effectiveDataSize)
{
    std::memcpy(m_srcMac, other.m_srcMac, 6);
    std::memcpy(m_dstMac, other.m_dstMac, 6);
}

FabricHeader&
FabricHeader::operator=(const FabricHeader& other)
{
    if (this != &other)
    {
        m_packetType = other.m_packetType;
        m_fabricType = other.m_fabricType;
        m_virtualChannel = other.m_virtualChannel;
        m_virtualLane = other.m_virtualLane;
        m_memoryAccessType = other.m_memoryAccessType;
        m_flowId = other.m_flowId;
        m_sequenceNumber = other.m_sequenceNumber;
        m_sourceRank = other.m_sourceRank;
        m_destRank = other.m_destRank;
        m_payloadSize = other.m_payloadSize;
        m_creditCount = other.m_creditCount;
        m_protocol = other.m_protocol;
        m_ttl = other.m_ttl;
        m_effectiveDataSize = other.m_effectiveDataSize;
        std::memcpy(m_srcMac, other.m_srcMac, 6);
        std::memcpy(m_dstMac, other.m_dstMac, 6);
    }
    return *this;
}

TypeId
FabricHeader::GetInstanceTypeId() const
{
    return GetTypeId();
}

uint32_t
FabricHeader::GetSerializedSize() const
{
    return 39;
}

void
FabricHeader::Serialize(Buffer::Iterator start) const
{
    Buffer::Iterator i = start;

    i.WriteU8(m_packetType);
    i.WriteU8(m_fabricType);
    i.WriteU8(m_virtualChannel);
    i.WriteU8(m_virtualLane);
    i.WriteU8(m_memoryAccessType);
    i.WriteHtonU16(m_flowId);
    i.WriteHtonU32(m_sequenceNumber);
    i.WriteHtonU16(m_sourceRank);
    i.WriteHtonU16(m_destRank);
    i.WriteHtonU32(m_payloadSize);
    i.WriteHtonU16(m_creditCount);
    i.WriteU8(m_protocol);
    i.WriteU8(m_ttl);
    i.WriteHtonU32(m_effectiveDataSize);

    for (int j = 0; j < 6; j++)
    {
        i.WriteU8(m_srcMac[j]);
    }
    for (int j = 0; j < 6; j++)
    {
        i.WriteU8(m_dstMac[j]);
    }
}

uint32_t
FabricHeader::Deserialize(Buffer::Iterator start)
{
    Buffer::Iterator i = start;

    m_packetType = i.ReadU8();
    m_fabricType = i.ReadU8();
    m_virtualChannel = i.ReadU8();
    m_virtualLane = i.ReadU8();
    m_memoryAccessType = i.ReadU8();
    m_flowId = i.ReadNtohU16();
    m_sequenceNumber = i.ReadNtohU32();
    m_sourceRank = i.ReadNtohU16();
    m_destRank = i.ReadNtohU16();
    m_payloadSize = i.ReadNtohU32();
    m_creditCount = i.ReadNtohU16();
    m_protocol = i.ReadU8();
    m_ttl = i.ReadU8();
    m_effectiveDataSize = i.ReadNtohU32();

    for (int j = 0; j < 6; j++)
    {
        m_srcMac[j] = i.ReadU8();
    }
    for (int j = 0; j < 6; j++)
    {
        m_dstMac[j] = i.ReadU8();
    }

    return GetSerializedSize();
}

void
FabricHeader::Print(std::ostream& os) const
{
    os << "FabricHeader: "
       << "Type=" << static_cast<int>(m_packetType)
       << " Fabric=" << FabricTypeToString(static_cast<FabricType>(m_fabricType))
       << " VC=" << static_cast<int>(m_virtualChannel)
       << " VL=" << static_cast<int>(m_virtualLane)
       << " MemAcc=" << static_cast<int>(m_memoryAccessType)
       << " FlowId=" << m_flowId
       << " SeqNum=" << m_sequenceNumber
       << " SrcRank=" << m_sourceRank
       << " DstRank=" << m_destRank
       << " PayloadSize=" << m_payloadSize
       << " CreditCount=" << m_creditCount
       << " Protocol=" << static_cast<int>(m_protocol)
       << " TTL=" << static_cast<int>(m_ttl);
}

// Getters
FabricPacketType
FabricHeader::GetPacketType() const
{
    return static_cast<FabricPacketType>(m_packetType);
}

uint16_t
FabricHeader::GetFlowId() const
{
    return m_flowId;
}

uint32_t
FabricHeader::GetSequenceNumber() const
{
    return m_sequenceNumber;
}

uint8_t
FabricHeader::GetVirtualChannel() const
{
    return m_virtualChannel;
}

uint16_t
FabricHeader::GetSourceRank() const
{
    return m_sourceRank;
}

uint16_t
FabricHeader::GetDestRank() const
{
    return m_destRank;
}

uint32_t
FabricHeader::GetPayloadSize() const
{
    return m_payloadSize;
}

uint16_t
FabricHeader::GetCreditCount() const
{
    return m_creditCount;
}

Mac48Address
FabricHeader::GetSourceMac() const
{
    Mac48Address addr;
    addr.CopyFrom(m_srcMac);
    return addr;
}

Mac48Address
FabricHeader::GetDestMac() const
{
    Mac48Address addr;
    addr.CopyFrom(m_dstMac);
    return addr;
}

// Setters
void
FabricHeader::SetPacketType(FabricPacketType type)
{
    m_packetType = static_cast<uint8_t>(type);
}

void
FabricHeader::SetFlowId(uint16_t flowId)
{
    m_flowId = flowId;
}

void
FabricHeader::SetSequenceNumber(uint32_t seqNum)
{
    m_sequenceNumber = seqNum;
}

void
FabricHeader::SetVirtualChannel(uint8_t vc)
{
    m_virtualChannel = vc;
}

void
FabricHeader::SetSourceRank(uint16_t rank)
{
    m_sourceRank = rank;
}

void
FabricHeader::SetDestRank(uint16_t rank)
{
    m_destRank = rank;
}

void
FabricHeader::SetPayloadSize(uint32_t size)
{
    m_payloadSize = size;
}

void
FabricHeader::SetCreditCount(uint16_t count)
{
    m_creditCount = count;
}

void
FabricHeader::SetSourceMac(Mac48Address addr)
{
    addr.CopyTo(m_srcMac);
}

void
FabricHeader::SetDestMac(Mac48Address addr)
{
    addr.CopyTo(m_dstMac);
}

bool
FabricHeader::IsControlPacket() const
{
    FabricPacketType type = GetPacketType();
    return type == FabricPacketType::CREDIT ||
           type == FabricPacketType::ACK ||
           type == FabricPacketType::NACK ||
           type == FabricPacketType::PERMANENT_LOSS;
}

bool
FabricHeader::IsCollectivePacket() const
{
    FabricPacketType type = GetPacketType();
    return type == FabricPacketType::ALLREDUCE ||
           type == FabricPacketType::ALLGATHER ||
           type == FabricPacketType::ALLTOALL ||
           type == FabricPacketType::REDUCESCATTER ||
           type == FabricPacketType::BROADCAST;
}

bool
FabricHeader::IsP2pPacket() const
{
    return GetPacketType() == FabricPacketType::P2P;
}

bool
FabricHeader::IsMemoryPacket() const
{
    FabricPacketType type = GetPacketType();
    return type == FabricPacketType::MEMORY_READ ||
           type == FabricPacketType::MEMORY_WRITE ||
           type == FabricPacketType::MEMORY_RESP;
}

bool
FabricHeader::IsRetryPacket() const
{
    FabricPacketType type = GetPacketType();
    return type == FabricPacketType::RETRY_REQUEST ||
           type == FabricPacketType::RETRY_ACK;
}

FabricType
FabricHeader::GetFabricType() const
{
    return static_cast<FabricType>(m_fabricType);
}

void
FabricHeader::SetFabricType(FabricType type)
{
    m_fabricType = static_cast<uint8_t>(type);
}

// Generic protocol field
uint8_t
FabricHeader::GetProtocol() const
{
    return m_protocol;
}

void
FabricHeader::SetProtocol(uint8_t protocolId)
{
    m_protocol = protocolId;
}

// TTL field
uint8_t
FabricHeader::GetTtl() const
{
    return m_ttl;
}

void
FabricHeader::SetTtl(uint8_t ttl)
{
    m_ttl = ttl;
}

// NCCL-specific convenience wrappers
NcclProtocol
FabricHeader::GetNcclProtocol() const
{
    return static_cast<NcclProtocol>(m_protocol);
}

void
FabricHeader::SetNcclProtocol(NcclProtocol protocol)
{
    m_protocol = static_cast<uint8_t>(protocol);
}

uint32_t
FabricHeader::GetEffectiveDataSize() const
{
    return m_effectiveDataSize;
}

void
FabricHeader::SetEffectiveDataSize(uint32_t size)
{
    m_effectiveDataSize = size;
}

MemoryAccessType
FabricHeader::GetMemoryAccessType() const
{
    return static_cast<MemoryAccessType>(m_memoryAccessType);
}

void
FabricHeader::SetMemoryAccessType(MemoryAccessType type)
{
    m_memoryAccessType = static_cast<uint8_t>(type);
}

uint8_t
FabricHeader::GetVirtualLane() const
{
    return m_virtualLane;
}

void
FabricHeader::SetVirtualLane(uint8_t lane)
{
    m_virtualLane = lane;
}

} // namespace ns3