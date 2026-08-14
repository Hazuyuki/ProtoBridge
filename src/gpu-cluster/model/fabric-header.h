/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Author: GPU Cluster Simulator Team
 *
 * Custom Fabric Header for GPU-to-GPU communication
 * Generic L2 protocol header supporting multiple vendor protocols.
 */

#ifndef FABRIC_HEADER_H
#define FABRIC_HEADER_H

#include "fabric-type.h"
#include "nccl-protocol.h"
#include "ns3/header.h"
#include "ns3/mac48-address.h"

#include <cstdint>

namespace ns3
{

/**
 * @ingroup gpu-cluster
 * @brief Packet type enumeration for fabric protocol
 */
enum class FabricPacketType : uint8_t
{
    DATA = 0,
    CREDIT = 1,
    ACK = 2,
    NACK = 3,
    ALLREDUCE = 4,
    ALLGATHER = 5,
    ALLTOALL = 6,
    REDUCESCATTER = 7,
    BROADCAST = 8,
    P2P = 9,
    MEMORY_READ = 10,
    MEMORY_WRITE = 11,
    MEMORY_RESP = 12,
    RETRY_REQUEST = 13,
    RETRY_ACK = 14,
    PERMANENT_LOSS = 15
};

/**
 * @ingroup gpu-cluster
 * @brief Memory access type for semantic operations
 */
enum class MemoryAccessType : uint8_t
{
    DMA_BULK = 0,         ///< Bulk DMA transfer (existing MEMORY_READ/WRITE behavior)
    SYNC_LOAD_STORE = 1,  ///< Synchronous load/store (~100-500ns depending on fabric)
    ASYNC_URMA = 2        ///< Asynchronous remote memory access (~1-5µs depending on fabric)
};

/**
 * @ingroup gpu-cluster
 * @brief Custom Fabric Header for GPU cluster communication
 *
 * This header contains fields required for:
 * - Credit-based flow control
 * - Multi-path packet spraying
 * - In-order delivery with reordering
 * - Virtual channel isolation
 * - Vendor-specific protocol identification
 *
 * Total wire size: 39 bytes
 */
class FabricHeader : public Header
{
  public:
    static TypeId GetTypeId();

    FabricHeader();
    ~FabricHeader() override;

    FabricHeader(const FabricHeader& other);
    FabricHeader& operator=(const FabricHeader& other);

    TypeId GetInstanceTypeId() const override;
    uint32_t GetSerializedSize() const override;
    void Serialize(Buffer::Iterator start) const override;
    uint32_t Deserialize(Buffer::Iterator start) override;
    void Print(std::ostream& os) const override;

    // Getters
    FabricPacketType GetPacketType() const;
    uint16_t GetFlowId() const;
    uint32_t GetSequenceNumber() const;
    uint8_t GetVirtualChannel() const;
    uint16_t GetSourceRank() const;
    uint16_t GetDestRank() const;
    uint32_t GetPayloadSize() const;
    uint16_t GetCreditCount() const;
    Mac48Address GetSourceMac() const;
    Mac48Address GetDestMac() const;

    // Setters
    void SetPacketType(FabricPacketType type);
    void SetFlowId(uint16_t flowId);
    void SetSequenceNumber(uint32_t seqNum);
    void SetVirtualChannel(uint8_t vc);
    void SetSourceRank(uint16_t rank);
    void SetDestRank(uint16_t rank);
    void SetPayloadSize(uint32_t size);
    void SetCreditCount(uint16_t count);
    void SetSourceMac(Mac48Address addr);
    void SetDestMac(Mac48Address addr);

    bool IsControlPacket() const;
    bool IsCollectivePacket() const;
    bool IsP2pPacket() const;
    bool IsMemoryPacket() const;
    bool IsRetryPacket() const;

    FabricType GetFabricType() const;
    void SetFabricType(FabricType type);

    // Generic protocol field (uint8_t — vendor-specific encoding)
    uint8_t GetProtocol() const;
    void SetProtocol(uint8_t protocolId);

    // TTL for multi-hop forwarding (default 64, decremented by switches)
    uint8_t GetTtl() const;
    void SetTtl(uint8_t ttl);

    // NCCL-specific convenience wrappers (deprecated: use Get/SetProtocol)
    NcclProtocol GetNcclProtocol() const;
    void SetNcclProtocol(NcclProtocol protocol);

    uint32_t GetEffectiveDataSize() const;
    void SetEffectiveDataSize(uint32_t size);

    // Memory access type (for memory-semantic operations)
    MemoryAccessType GetMemoryAccessType() const;
    void SetMemoryAccessType(MemoryAccessType type);

    // Virtual lane (for deadlock-free routing in nD topologies)
    uint8_t GetVirtualLane() const;
    void SetVirtualLane(uint8_t lane);

  private:
    // Header fields (total: 39 bytes)
    uint8_t m_packetType;
    uint8_t m_fabricType;
    uint8_t m_virtualChannel;
    uint8_t m_virtualLane;          ///< Virtual lane for deadlock-free routing (default 0)
    uint8_t m_memoryAccessType;     ///< Memory access type (DMA_BULK/SYNC/ASYNC)
    uint16_t m_flowId;
    uint32_t m_sequenceNumber;
    uint16_t m_sourceRank;
    uint16_t m_destRank;
    uint32_t m_payloadSize;
    uint16_t m_creditCount;
    uint8_t m_protocol;           // Generic protocol ID (1 byte)
    uint8_t m_ttl;               // TTL for multi-hop (default 64)
    uint32_t m_effectiveDataSize;
    uint8_t m_srcMac[6];
    uint8_t m_dstMac[6];
};

} // namespace ns3

#endif /* FABRIC_HEADER_H */