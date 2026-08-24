/*
 * SPDX-License-Identifier: GPL-2.0-only
 * so-header.h
 *
 * Scale-out (So) protocol header, carried on inter-node (Ethernet RoCEv2)
 * packets only -- inner to the shared FabricHeader. It holds the COMMON
 * congestion field (an ECN bit) that the So fabric switch marks in-flight
 * and the So receiver reads to emit a CNP, plus the per-segment RDMA framing
 * (data size + chunk index) that previously lived in RocePayloadBuilder's
 * raw 8-byte prefix.
 *
 * Protocol-pluggable: m_protocolId discriminates the So protocol (RoCE today;
 * a future non-RoCE So protocol that needs vendor-specific wire fields
 * subclasses SoHeader and adds its fields). The switch marks only the common
 * ECN bit and is blind to the vendor layout, so adding a new So protocol
 * needs no switch change. Su (intra-node NVLink) packets never carry a
 * SoHeader, so the Su parity path is untouched.
 */
#ifndef SO_HEADER_H
#define SO_HEADER_H

#include "ns3/header.h"
#include "ns3/packet.h"

#include <cstdint>

namespace ns3
{

/**
 * @ingroup gpu-cluster
 * @brief Scale-out (RoCEv2) protocol header carrying the ECN congestion bit.
 *
 * Wire size: 10 bytes (1 protocolId + 1 flags + 4 dataSize + 4 chunkIndex).
 */
class SoHeader : public Header
{
  public:
    static TypeId GetTypeId();

    SoHeader();
    ~SoHeader() override;

    TypeId GetInstanceTypeId() const override;
    uint32_t GetSerializedSize() const override;
    void Serialize(Buffer::Iterator start) const override;
    uint32_t Deserialize(Buffer::Iterator start) override;
    void Print(std::ostream& os) const override;

    /// ECN congestion bit (bit 0 of m_flags).
    bool GetEcn() const
    {
        return (m_flags & ECN_FLAG) != 0;
    }

    void SetEcn(bool v);

    uint8_t GetProtocolId() const
    {
        return m_protocolId;
    }

    void SetProtocolId(uint8_t p)
    {
        m_protocolId = p;
    }

    uint32_t GetDataSize() const
    {
        return m_dataSize;
    }

    void SetDataSize(uint32_t s)
    {
        m_dataSize = s;
    }

    uint32_t GetChunkIndex() const
    {
        return m_chunkIndex;
    }

    void SetChunkIndex(uint32_t c)
    {
        m_chunkIndex = c;
    }

    /**
     * Peek the ECN bit of a packet whose SoHeader is outermost (i.e. after
     * the FabricHeader has been peeled on the receive path). Non-mutating.
     */
    static bool PeekEcn(Ptr<const Packet> packet);

  private:
    static constexpr uint8_t ECN_FLAG = 0x01;

    uint8_t m_protocolId;  ///< So protocol discriminator (RoCE = 1 today)
    uint8_t m_flags;       ///< bit0 = ECN
    uint32_t m_dataSize;   ///< data bytes in this segment
    uint32_t m_chunkIndex; ///< segment sequence number
};

} // namespace ns3

#endif // SO_HEADER_H
